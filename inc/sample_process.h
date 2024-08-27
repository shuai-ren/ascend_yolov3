/**
* @file sample_process.h
*
* Copyright (C) 2020. Huawei Technologies Co., Ltd. All rights reserved.
*
* This program is distributed in the hope that it will be useful,
* but WITHOUT ANY WARRANTY; without even the implied warranty of
* MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
*/
#pragma once
#include "utils.h"
#include "acl/acl.h"
#include "server.hpp"
#include<stdlib.h>

struct DrawImageInfo
{
    //Point  ascenddk/presenter/agent/presenter_channel.h" 在这里头还有有一个 加上cv:: 就不会影响 这个是叫做命名空间 namespace
    vector<vector<cv::Point>> contours;
    vector<vector<int>> line;
    vector<int> color;
    int width;
    bool drawAreaFlag;
    bool drawLineFlag;
    cv::Point startPoint;
    cv::Point endPoint;
    int output_h=360;
    int output_w=640;
    bool updateAreaFlag;
    bool updateLineFlag;
};

class SampleProcess {
public:
    /**
    * @brief Constructor
    */
    SampleProcess(int jpeg_port, int json_port, std::string streamName, float con, int32_t deviceId);

    /**
    * @brief Destructor
    */
    virtual ~SampleProcess();

    /**
    * @brief init reousce
    * @return result
    */
    Result InitResource();

    /**
    * @brief sample process
    * @return result
    */
    Result Process(int jpeg_port);

    void DrawBoundBoxToImage(std::vector<BBox>& detectionResults, PicDesc &picDesc);

    void* GetInferenceOutputItem(uint32_t& itemDataSize, const aclmdlDataset* inferenceOutput, uint32_t idx);

    Result Postprocess(std::string& send_info, const aclmdlDataset* modelOutput, PicDesc &picDesc, int modelWidth, int modelHeight, int jpeg_port);

    static DrawImageInfo GetJsonFromEtcBus();

private:
    void DestroyResource();
    int32_t deviceId_;
    aclrtContext context_;
    aclrtStream stream_;
    aclrtStream streamOutput_;
    int output_w = 640;
    int output_h = 360;

    std::shared_ptr<MJPG_sender> jpg_sender_;
    std::shared_ptr<JSON_sender> json_sender_;
    
    std::string streamName_;
    aclrtRunMode runMode_;
	FILE *fp = nullptr;
    float conf_;
    std::stringstream command;

    // cv::VideoWriter outputVideo_;
};

