/*
 * Copyright (c) 2015 New Designs Unlimited, LLC
 * Opensource Automated License Plate Recognition [http://www.openalpr.com]
 *
 * This file is part of OpenAlpr.
 *
 * OpenAlpr is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Affero General Public License
 * version 3 as published by the Free Software Foundation
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Affero General Public License for more details.
 *
 * You should have received a copy of the GNU Affero General Public License
 * along with this program. If not, see <http://www.gnu.org/licenses/>.
*/

#include "alpr_impl.h"


void plateAnalysisThread(void* arg);

using namespace std;
using namespace cv;

namespace alpr
{
  AlprImpl::AlprImpl(const std::string country, const std::string configFile, const std::string runtimeDir)
  {
    config = new Config(country, configFile, runtimeDir);
    
    plateDetector = ALPR_NULL_PTR;
    stateIdentifier = ALPR_NULL_PTR;
    ocr = ALPR_NULL_PTR;
    prewarp = ALPR_NULL_PTR;
    
    // Config file or runtime dir not found.  Don't process any further.
    if (config->loaded == false)
    {
      return;
    }

    plateDetector = createDetector(config);
    ocr = new OCR(config);
    setNumThreads(0);

    setDetectRegion(DEFAULT_DETECT_REGION);
    this->topN = DEFAULT_TOPN;
    setDefaultRegion("");
    
    prewarp = new PreWarp(config);

  }
  AlprImpl::~AlprImpl()
  {
    delete config;

    if (plateDetector != ALPR_NULL_PTR)
      delete plateDetector;

    if (stateIdentifier != ALPR_NULL_PTR)
      delete stateIdentifier;

    if (ocr != ALPR_NULL_PTR)
      delete ocr;
    
    if (prewarp != ALPR_NULL_PTR)
      delete prewarp;
  }

  bool AlprImpl::isLoaded()
  {
    return config->loaded;
  }


  AlprFullDetails AlprImpl::recognizeFullDetails(cv::Mat img, std::vector<cv::Rect> regionsOfInterest)
  {
    timespec startTime;
    getTimeMonotonic(&startTime);


    AlprFullDetails response;

    response.results.epoch_time = getEpochTimeMs();
    response.results.img_width = img.cols;
    response.results.img_height = img.rows;

    for (unsigned int i = 0; i < regionsOfInterest.size(); i++)
    {
      response.results.regionsOfInterest.push_back(AlprRegionOfInterest(regionsOfInterest[i].x, regionsOfInterest[i].y,
              regionsOfInterest[i].width, regionsOfInterest[i].height));
    }

    if (!img.data)
    {
      // Invalid image
      if (this->config->debugGeneral)
        std::cerr << "Invalid image" << std::endl;

      return response;
    }

    // Convert image to grayscale if required
    Mat grayImg = img;
    if (img.channels() > 2)
      cvtColor( img, grayImg, CV_BGR2GRAY );
    
    // Prewarp the image and ROIs if configured]
    std::vector<cv::Rect> warpedRegionsOfInterest = regionsOfInterest;
    // Warp the image if prewarp is provided
    grayImg = prewarp->warpImage(grayImg);
    warpedRegionsOfInterest = prewarp->projectRects(regionsOfInterest, grayImg.cols, grayImg.rows, false);
    
    vector<PlateRegion> warpedPlateRegions;
    // Find all the candidate regions
    if (config->skipDetection == false)
    {
      warpedPlateRegions = plateDetector->detect(grayImg, warpedRegionsOfInterest);
    }
    else
    {
      // They have elected to skip plate detection.  Instead, return a list of plate regions
      // based on their regions of interest
      for (unsigned int i = 0; i < warpedRegionsOfInterest.size(); i++)
      {
        PlateRegion pr;
        pr.rect = cv::Rect(warpedRegionsOfInterest[i]);
        warpedPlateRegions.push_back(pr);
      }
    }

    queue<PlateRegion> plateQueue;
    for (unsigned int i = 0; i < warpedPlateRegions.size(); i++)
      plateQueue.push(warpedPlateRegions[i]);

    int platecount = 0;
    while(!plateQueue.empty())
    {
      PlateRegion plateRegion = plateQueue.front();
      plateQueue.pop();

      PipelineData pipeline_data(img, grayImg, plateRegion.rect, config);

      timespec platestarttime;
      getTimeMonotonic(&platestarttime);

      LicensePlateCandidate lp(&pipeline_data);

      lp.recognize();

      bool plateDetected = false;
      if (pipeline_data.plate_area_confidence > 10)
      {
        AlprPlateResult plateResult;
        plateResult.region = defaultRegion;
        plateResult.regionConfidence = 0;
        plateResult.plate_index = platecount++;

        // If using prewarp, remap the plate corners to the original image
        vector<Point2f> cornerPoints = pipeline_data.plate_corners;
        cornerPoints = prewarp->projectPoints(cornerPoints, true);
        
        for (int pointidx = 0; pointidx < 4; pointidx++)
        {
          plateResult.plate_points[pointidx].x = (int) cornerPoints[pointidx].x;
          plateResult.plate_points[pointidx].y = (int) cornerPoints[pointidx].y;
        }
        
        if (detectRegion)
        {
          stateIdentifier->recognize(&pipeline_data);
          if (pipeline_data.region_confidence > 0)
          {
            plateResult.region = pipeline_data.region_code;
            plateResult.regionConfidence = (int) pipeline_data.region_confidence;
          }
        }

        if (plateResult.region.length() > 0 && ocr->postProcessor.regionIsValid(plateResult.region) == false)
        {
          std::cerr << "Invalid pattern provided: " << plateResult.region << std::endl;
          std::cerr << "Valid patterns are located in the " << config->country << ".patterns file" << std::endl;
        }

        ocr->performOCR(&pipeline_data);
        ocr->postProcessor.analyze(plateResult.region, topN);
        const vector<PPResult> ppResults = ocr->postProcessor.getResults();


        int bestPlateIndex = 0;

        for (unsigned int pp = 0; pp < ppResults.size(); pp++)
        {
          if (pp >= topN)
            break;

          int plate_char_length = utf8::distance(ppResults[pp].letters.begin(), ppResults[pp].letters.end());
          if (plate_char_length >= config->postProcessMinCharacters &&
            plate_char_length <= config->postProcessMaxCharacters)
          {
            // Set our "best plate" match to either the first entry, or the first entry with a postprocessor template match
            if (bestPlateIndex == 0 && ppResults[pp].matchesTemplate)
              bestPlateIndex = plateResult.topNPlates.size();
            
            AlprPlate aplate;
            aplate.characters = ppResults[pp].letters;
            aplate.overall_confidence = ppResults[pp].totalscore;
            aplate.matches_template = ppResults[pp].matchesTemplate;
            
            // Grab detailed results for each character
            for (unsigned int c_idx = 0; c_idx < ppResults[pp].letter_details.size(); c_idx++)
            {
              AlprChar character_details;
              character_details.character = ppResults[pp].letter_details[c_idx].letter;
              character_details.confidence = ppResults[pp].letter_details[c_idx].totalscore;
              cv::Rect char_rect = pipeline_data.charRegions[ppResults[pp].letter_details[c_idx].charposition];
              std::vector<AlprCoordinate> charpoints = getCharacterPoints(char_rect, &pipeline_data );
              for (int cpt = 0; cpt < 4; cpt++)
                character_details.corners[cpt] = charpoints[cpt];
              aplate.character_details.push_back(character_details);
            }
            plateResult.topNPlates.push_back(aplate);
          }

        }

        if (plateResult.topNPlates.size() > bestPlateIndex)
        {
          AlprPlate bestPlate;
          bestPlate.characters = plateResult.topNPlates[bestPlateIndex].characters;
          bestPlate.matches_template = plateResult.topNPlates[bestPlateIndex].matches_template;
          bestPlate.overall_confidence = plateResult.topNPlates[bestPlateIndex].overall_confidence;
          bestPlate.character_details = plateResult.topNPlates[bestPlateIndex].character_details;
          
          plateResult.bestPlate = bestPlate;
        }

        timespec plateEndTime;
        getTimeMonotonic(&plateEndTime);
        plateResult.processing_time_ms = diffclock(platestarttime, plateEndTime);

        if (plateResult.topNPlates.size() > 0)
        {
          plateDetected = true;
          response.results.plates.push_back(plateResult);
        }
      }

      if (!plateDetected)
      {
        // Not a valid plate
        // Check if this plate has any children, if so, send them back up for processing
        for (unsigned int childidx = 0; childidx < plateRegion.children.size(); childidx++)
        {
          plateQueue.push(plateRegion.children[childidx]);
        }
      }




    }

    // Unwarp plate regions if necessary
    prewarp->projectPlateRegions(warpedPlateRegions, grayImg.cols, grayImg.rows, true);
    response.plateRegions = warpedPlateRegions;
    
    timespec endTime;
    getTimeMonotonic(&endTime);
    response.results.total_processing_time_ms = diffclock(startTime, endTime);

    if (config->debugTiming)
    {
      cout << "Total Time to process image: " << diffclock(startTime, endTime) << "ms." << endl;
    }

    if (config->debugGeneral && config->debugShowImages)
    {
      for (unsigned int i = 0; i < regionsOfInterest.size(); i++)
      {
        rectangle(img, regionsOfInterest[i], Scalar(0,255,0), 2);
      }

      for (unsigned int i = 0; i < response.plateRegions.size(); i++)
      {
        rectangle(img, response.plateRegions[i].rect, Scalar(0, 0, 255), 2);
      }

      for (unsigned int i = 0; i < response.results.plates.size(); i++)
      {
        // Draw a box around the license plate 
        for (int z = 0; z < 4; z++)
        {
          AlprCoordinate* coords = response.results.plates[i].plate_points;
          Point p1(coords[z].x, coords[z].y);
          Point p2(coords[(z + 1) % 4].x, coords[(z + 1) % 4].y);
          line(img, p1, p2, Scalar(255,0,255), 2);
        }
        
        // Draw the individual character boxes
        for (int q = 0; q < response.results.plates[i].bestPlate.character_details.size(); q++)
        {
          AlprChar details = response.results.plates[i].bestPlate.character_details[q];
          line(img, Point(details.corners[0].x, details.corners[0].y), Point(details.corners[1].x, details.corners[1].y), Scalar(0,255,0), 1);
          line(img, Point(details.corners[1].x, details.corners[1].y), Point(details.corners[2].x, details.corners[2].y), Scalar(0,255,0), 1);
          line(img, Point(details.corners[2].x, details.corners[2].y), Point(details.corners[3].x, details.corners[3].y), Scalar(0,255,0), 1);
          line(img, Point(details.corners[3].x, details.corners[3].y), Point(details.corners[0].x, details.corners[0].y), Scalar(0,255,0), 1);
        }
      }


      displayImage(config, "Main Image", img);

      // Sleep 1ms
      sleep_ms(1);

    }


    if (config->debugPauseOnFrame)
    {
      // Pause indefinitely until they press a key
      while ((char) cv::waitKey(50) == -1)
      {}
    }

    return response;
  }



  AlprResults AlprImpl::recognize( std::vector<char> imageBytes)
  {
    cv::Mat img = cv::imdecode(cv::Mat(imageBytes), 1);

    return this->recognize(img);
  }

  AlprResults AlprImpl::recognize( unsigned char* pixelData, int bytesPerPixel, int imgWidth, int imgHeight, std::vector<AlprRegionOfInterest> regionsOfInterest)
  {

    int arraySize = imgWidth * imgHeight * bytesPerPixel;
    cv::Mat imgData = cv::Mat(arraySize, 1, CV_8U, pixelData);
    cv::Mat img = imgData.reshape(bytesPerPixel, imgHeight);

    if (regionsOfInterest.size() == 0)
    {
      AlprRegionOfInterest fullFrame(0,0, img.cols, img.rows);

      regionsOfInterest.push_back(fullFrame);
    }

    return this->recognize(img, this->convertRects(regionsOfInterest));
  }

  AlprResults AlprImpl::recognize(cv::Mat img)
  {
    std::vector<cv::Rect> regionsOfInterest;
    regionsOfInterest.push_back(cv::Rect(0, 0, img.cols, img.rows));

    return this->recognize(img, regionsOfInterest);
  }

  AlprResults AlprImpl::recognize(cv::Mat img, std::vector<cv::Rect> regionsOfInterest)
  {
    AlprFullDetails fullDetails = recognizeFullDetails(img, regionsOfInterest);
    return fullDetails.results;
  }


   std::vector<cv::Rect> AlprImpl::convertRects(std::vector<AlprRegionOfInterest> regionsOfInterest)
   {
     std::vector<cv::Rect> rectRegions;
     for (unsigned int i = 0; i < regionsOfInterest.size(); i++)
     {
       rectRegions.push_back(cv::Rect(regionsOfInterest[i].x, regionsOfInterest[i].y, regionsOfInterest[i].width, regionsOfInterest[i].height));
     }

     return rectRegions;
   }

  string AlprImpl::toJson( const AlprResults results )
  {
    cJSON *root, *jsonResults;
    root = cJSON_CreateObject();


    cJSON_AddNumberToObject(root,"version",	2	  );
    cJSON_AddStringToObject(root,"data_type",	"alpr_results"	  );

    cJSON_AddNumberToObject(root,"epoch_time",	results.epoch_time	  );
    cJSON_AddNumberToObject(root,"img_width",	results.img_width	  );
    cJSON_AddNumberToObject(root,"img_height",	results.img_height	  );
    cJSON_AddNumberToObject(root,"processing_time_ms", results.total_processing_time_ms );

    // Add the regions of interest to the JSON
    cJSON *rois;
    cJSON_AddItemToObject(root, "regions_of_interest", 		rois=cJSON_CreateArray());
    for (unsigned int i=0;i<results.regionsOfInterest.size();i++)
    {
      cJSON *roi_object;
      roi_object = cJSON_CreateObject();
      cJSON_AddNumberToObject(roi_object, "x",  results.regionsOfInterest[i].x);
      cJSON_AddNumberToObject(roi_object, "y",  results.regionsOfInterest[i].y);
      cJSON_AddNumberToObject(roi_object, "width",  results.regionsOfInterest[i].width);
      cJSON_AddNumberToObject(roi_object, "height",  results.regionsOfInterest[i].height);

      cJSON_AddItemToArray(rois, roi_object);
    }


    cJSON_AddItemToObject(root, "results", 		jsonResults=cJSON_CreateArray());
    for (unsigned int i = 0; i < results.plates.size(); i++)
    {
      cJSON *resultObj = createJsonObj( &results.plates[i] );
      cJSON_AddItemToArray(jsonResults, resultObj);
    }

    // Print the JSON object to a string and return
    char *out;
    out=cJSON_PrintUnformatted(root);

    cJSON_Delete(root);

    string response(out);

    free(out);
    return response;
  }



  cJSON* AlprImpl::createJsonObj(const AlprPlateResult* result)
  {
    cJSON *root, *coords, *candidates;

    root=cJSON_CreateObject();

    cJSON_AddStringToObject(root,"plate",		result->bestPlate.characters.c_str());
    cJSON_AddNumberToObject(root,"confidence",		result->bestPlate.overall_confidence);
    cJSON_AddNumberToObject(root,"matches_template",	result->bestPlate.matches_template);

    cJSON_AddNumberToObject(root,"plate_index",               result->plate_index);

    cJSON_AddStringToObject(root,"region",		result->region.c_str());
    cJSON_AddNumberToObject(root,"region_confidence",	result->regionConfidence);

    cJSON_AddNumberToObject(root,"processing_time_ms",	result->processing_time_ms);
    cJSON_AddNumberToObject(root,"requested_topn",	result->requested_topn);

    cJSON_AddItemToObject(root, "coordinates", 		coords=cJSON_CreateArray());
    for (int i=0;i<4;i++)
    {
      cJSON *coords_object;
      coords_object = cJSON_CreateObject();
      cJSON_AddNumberToObject(coords_object, "x",  result->plate_points[i].x);
      cJSON_AddNumberToObject(coords_object, "y",  result->plate_points[i].y);

      cJSON_AddItemToArray(coords, coords_object);
    }


    cJSON_AddItemToObject(root, "candidates", 		candidates=cJSON_CreateArray());
    for (unsigned int i = 0; i < result->topNPlates.size(); i++)
    {
      cJSON *candidate_object;
      candidate_object = cJSON_CreateObject();
      cJSON_AddStringToObject(candidate_object, "plate",  result->topNPlates[i].characters.c_str());
      cJSON_AddNumberToObject(candidate_object, "confidence",  result->topNPlates[i].overall_confidence);
      cJSON_AddNumberToObject(candidate_object, "matches_template",  result->topNPlates[i].matches_template);

      cJSON_AddItemToArray(candidates, candidate_object);
    }

    return root;
  }

  AlprResults AlprImpl::fromJson(std::string json) {
    AlprResults allResults;

    cJSON* root = cJSON_Parse(json.c_str());

    int version = cJSON_GetObjectItem(root, "version")->valueint;
    allResults.epoch_time = (int64_t) cJSON_GetObjectItem(root, "epoch_time")->valuedouble;
    allResults.img_width = cJSON_GetObjectItem(root, "img_width")->valueint;
    allResults.img_height = cJSON_GetObjectItem(root, "img_height")->valueint;
    allResults.total_processing_time_ms = cJSON_GetObjectItem(root, "processing_time_ms")->valueint;


    cJSON* rois = cJSON_GetObjectItem(root,"regions_of_interest");
    int numRois = cJSON_GetArraySize(rois);
    for (int c = 0; c < numRois; c++)
    {
      cJSON* roi = cJSON_GetArrayItem(rois, c);
      int x = cJSON_GetObjectItem(roi, "x")->valueint;
      int y = cJSON_GetObjectItem(roi, "y")->valueint;
      int width = cJSON_GetObjectItem(roi, "width")->valueint;
      int height = cJSON_GetObjectItem(roi, "height")->valueint;

      AlprRegionOfInterest alprRegion(x,y,width,height);
      allResults.regionsOfInterest.push_back(alprRegion);
    }

    cJSON* resultsArray = cJSON_GetObjectItem(root,"results");
    int resultsSize = cJSON_GetArraySize(resultsArray);

    for (int i = 0; i < resultsSize; i++)
    {
      cJSON* item = cJSON_GetArrayItem(resultsArray, i);
      AlprPlateResult plate;

      //plate.bestPlate = cJSON_GetObjectItem(item, "plate")->valuestring;
      plate.processing_time_ms = cJSON_GetObjectItem(item, "processing_time_ms")->valuedouble;
      plate.plate_index = cJSON_GetObjectItem(item, "plate_index")->valueint;
      plate.region = std::string(cJSON_GetObjectItem(item, "region")->valuestring);
      plate.regionConfidence = cJSON_GetObjectItem(item, "region_confidence")->valueint;
      plate.requested_topn = cJSON_GetObjectItem(item, "requested_topn")->valueint;


      cJSON* coordinates = cJSON_GetObjectItem(item,"coordinates");
      for (int c = 0; c < 4; c++)
      {
        cJSON* coordinate = cJSON_GetArrayItem(coordinates, c);
        AlprCoordinate alprcoord;
        alprcoord.x = cJSON_GetObjectItem(coordinate, "x")->valueint;
        alprcoord.y = cJSON_GetObjectItem(coordinate, "y")->valueint;

        plate.plate_points[c] = alprcoord;
      }

      cJSON* candidates = cJSON_GetObjectItem(item,"candidates");
      int numCandidates = cJSON_GetArraySize(candidates);
      for (int c = 0; c < numCandidates; c++)
      {
        cJSON* candidate = cJSON_GetArrayItem(candidates, c);
        AlprPlate plateCandidate;
        plateCandidate.characters = std::string(cJSON_GetObjectItem(candidate, "plate")->valuestring);
        plateCandidate.overall_confidence = cJSON_GetObjectItem(candidate, "confidence")->valuedouble;
        plateCandidate.matches_template = (cJSON_GetObjectItem(candidate, "matches_template")->valueint) != 0;

        plate.topNPlates.push_back(plateCandidate);

        if (c == 0)
        {
          plate.bestPlate = plateCandidate;
        }
      }

      allResults.plates.push_back(plate);
    }


    cJSON_Delete(root);


    return allResults;
  }


  void AlprImpl::setDetectRegion(bool detectRegion)
  {
    this->detectRegion = detectRegion;
    if (detectRegion && this->stateIdentifier == NULL)
    {
        this->stateIdentifier = new StateIdentifier(this->config);
    }

  }
  void AlprImpl::setTopN(int topn)
  {
    this->topN = topn;
  }
  void AlprImpl::setDefaultRegion(string region)
  {
    this->defaultRegion = region;
  }

  std::string AlprImpl::getVersion()
  {
    std::stringstream ss;

    ss << OPENALPR_MAJOR_VERSION << "." << OPENALPR_MINOR_VERSION << "." << OPENALPR_PATCH_VERSION;
    return ss.str();
  }
  
  std::vector<AlprCoordinate> AlprImpl::getCharacterPoints(cv::Rect char_rect, PipelineData* pipeline_data ) {
    


    std::vector<Point2f> points;
    points.push_back(Point2f(char_rect.x, char_rect.y));
    points.push_back(Point2f(char_rect.x + char_rect.width, char_rect.y));
    points.push_back(Point2f(char_rect.x + char_rect.width, char_rect.y + char_rect.height));
    points.push_back(Point2f(char_rect.x, char_rect.y + char_rect.height));
    
    Mat img = pipeline_data->colorImg;
    Mat debugImg(img.size(), img.type());
    img.copyTo(debugImg);
    
    
   
    std::vector<Point2f> crop_corners;
    crop_corners.push_back(Point2f(0,0));
    crop_corners.push_back(Point2f(pipeline_data->crop_gray.cols,0));
    crop_corners.push_back(Point2f(pipeline_data->crop_gray.cols,pipeline_data->crop_gray.rows));
    crop_corners.push_back(Point2f(0,pipeline_data->crop_gray.rows));

    // Transform the points from the cropped region (skew corrected license plate region) back to the original image
    cv::Mat transmtx = cv::getPerspectiveTransform(crop_corners, pipeline_data->plate_corners);
    cv::perspectiveTransform(points, points, transmtx);
    
    // If using prewarp, remap the points to the original image
    points = prewarp->projectPoints(points, true);
        
    
    std::vector<AlprCoordinate> cornersvector;
    for (int i = 0; i < 4; i++)
    {
      AlprCoordinate coord;
      coord.x = round(points[i].x);
      coord.y = round(points[i].y);
      cornersvector.push_back(coord);
    }
    
    return cornersvector;
  }


}
