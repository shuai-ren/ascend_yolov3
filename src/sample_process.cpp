/**
* @file sample_process.cpp
*
* Copyright (C) 2020. Huawei Technologies Co., Ltd. All rights reserved.
*
* This program is distributed in the hope that it will be useful,
* but WITHOUT ANY WARRANTY; without even the implied warranty of
* MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
*/
#include "sample_process.h"
#include <iostream>
#include "dvpp_process.h"
#include "model_process.h"
#include "decode.h"
#include "acl/acl.h"
#include "utils.h"
#include <dirent.h>
#include <string>
#include <stdio.h>
#include <sys/stat.h>
#include <fstream>
#include <nlohmann/json.hpp>
using namespace std;


struct _IGNORE_PIPE_SIGNAL
{
    struct sigaction new_actn, old_actn;
    _IGNORE_PIPE_SIGNAL() {
        new_actn.sa_handler = SIG_IGN;  // ignore the broken pipe signal
        sigemptyset(&new_actn.sa_mask);
        new_actn.sa_flags = 0;
        sigaction(SIGPIPE, &new_actn, &old_actn);
        // sigaction (SIGPIPE, &old_actn, NULL); // - to restore the previous signal handling
    }
} _init_once;


DrawImageInfo drawImageInfo = SampleProcess::GetJsonFromEtcBus();
bool firstFlag = true;
namespace {
    // const static std::vector<std::string> yolov3Label = { "person", "bicycle", "car", "motorbike",
    // "aeroplane","bus", "train", "truck", "boat",
    // "traffic light", "fire hydrant", "stop sign", "parking meter",
    // "bench", "bird", "cat", "dog", "horse",
    // "sheep", "cow", "elephant", "bear", "zebra",
    // "giraffe", "backpack", "umbrella", "handbag","tie",
    // "suitcase", "frisbee", "skis", "snowboard", "sports ball",
    // "kite", "baseball bat", "baseball glove", "skateboard", "surfboard",
    // "tennis racket", "bottle", "wine glass", "cup",
    // "fork", "knife", "spoon", "bowl", "banana",
    // "apple", "sandwich", "orange", "broccoli", "carrot",
    // "hot dog", "pizza", "donut", "cake", "chair",
    // "sofa", "potted plant", "bed", "dining table", "toilet",
    // "TV monitor", "laptop", "mouse", "remote", "keyboard",
    // "cell phone", "microwave", "oven", "toaster", "sink",
    // "refrigerator", "book", "clock", "vase","scissors",
    // "teddy bear", "hair drier", "toothbrush" };
    // const static std::vector<std::string> yolov3Label = { "person", "bicycle", "car"};

    const uint32_t kBBoxDataBufId = 0;
    const uint32_t kBoxNumDataBufId = 1;

    enum BBoxIndex { TOPLEFTX = 0, TOPLEFTY, BOTTOMRIGHTX, BOTTOMRIGHTY, SCORE, LABEL };
    // bounding box line solid
    const uint32_t kLineSolid = 2;

    // output image prefix
    const string kOutputFilePrefix = "out_";
    // opencv draw label params.
    const double kFountScale = 0.5;
    const cv::Scalar kFontColor(0, 0, 255);
    const uint32_t kLabelOffset = 11;
    const string kFileSperator = "/";

    // opencv color list for boundingbox
    const vector<cv::Scalar> kColors{
        cv::Scalar(237, 149, 100), cv::Scalar(0, 215, 255), cv::Scalar(50, 205, 50),
        cv::Scalar(139, 85, 26) };
}

void getLocalTime(char* localTime)
{
	time_t timer = time(NULL);
	strftime(localTime, 256, "%Y-%m-%d %H:%M:%S", localtime(&timer));
}

int detectProcessByName(const char* processName) {
    FILE* fp = NULL;
    int BUFSZ = 100;
    char buf[BUFSZ];
    char command[150];

    std::string sanitizedName(processName);
    std::replace(sanitizedName.begin(), sanitizedName.end(), '/', '/');

    if (snprintf(command, 150, "ps aux|grep -c %s", sanitizedName.c_str()) < 0)
        return -1;

    if ((fp = popen(command, "r")) == NULL) {
        return -2;
    }

    if ((fgets(buf, BUFSZ, fp)) != NULL) {
        pclose(fp);
        fp = NULL;

        // 获取到的结果是进程数，如果大于 0 表示进程存在
        int processCount = atoi(buf);
        int processCount_ = processCount -1;
        if (processCount_ > 1)
            return 0;
    }

    std::cout << "detectprocess cmd: " << command << std::endl;
    return -3;
}

SampleProcess::SampleProcess(int jpeg_port, int json_port, string streamName, float con, int32_t deviceId):
deviceId_(0), 
context_(nullptr), 
stream_(nullptr), 
streamOutput_(nullptr),
streamName_(streamName) {
    // int jpeg_timeout = 200000;
    // int jpeg_quality = 40;
    // INFO_LOG("time out %d", jpeg_timeout);
    // jpg_sender_ = make_shared<MJPG_sender>(jpeg_port, jpeg_timeout, jpeg_quality);
    char *env_w; 
    if((env_w = getenv("ACL_OUTPUT_W"))){ 
        printf("W=%s\n", env_w);
        string s(env_w); 
        output_w = stoi(s);        
    }
    char *env_h; 
    if((env_h = getenv("ACL_OUTPUT_H"))){ 
        printf("H=%s\n", env_h);
        string s(env_h); 
        output_h = stoi(s);   
    }
    DrawImageInfo drawImageInfo = SampleProcess::GetJsonFromEtcBus();
    printf("W_OUTPUT=%d\n", output_w);
    printf("H_OUTPUT=%d\n", output_h);
    int json_timeout = 20000;
    json_sender_ = make_shared<JSON_sender>(json_port, json_timeout);
    // string outpath = "rtsp://172.24.12.245:10011/rtsp";
    string rtsp = to_string(jpeg_port);
    // const string rtsp = to_string(jpeg_port);
    std::string outpath = "rtsp://localhost:"+ rtsp +"/rtsp";
    conf_ = con;
    deviceId_ = deviceId;
    std::cout << "conf: " << conf_ << " deviceId_: " << deviceId_ << std::endl;
    command << "ffmpeg ";

    // infile options
    command << "-an "            // force format to rawvideo
            << "-f rawvideo "        // force format to rawvideo
            << "-pix_fmt bgr24 "   // set pixel format to bgr24
            << "-s "            // set frame size (WxH or abbreviation)
            << to_string(drawImageInfo.output_w)
            << "x"
            << to_string(drawImageInfo.output_h)
            << " -r 25 "; // set frame rate (Hz value, fraction or abbreviation)
    command << " -i - "; //

    // outfile options
    command << "-c:v libx264 "    // Hyper fast Audio and Video encoder
            << "-pix_fmt yuv420p " // set pixel format to yuv420p
            << "-preset ultrafast " // set the libx264 encoding preset to ultrafast
            << "-tune zerolatency "
            << "-crf 30 "
            << "-f rtsp "            // force format to flv
            << "-rtsp_transport tcp "
            << outpath;
    ffmpeg_pipe = popen(command.str().c_str(), "w");
    std::cout << "ffmpeg cmd: " << command.str().c_str() << std::endl;
}

SampleProcess::~SampleProcess() {
    DestroyResource();
}

Result SampleProcess::InitResource() {
    // ACL init
    const char *aclConfigPath = "../src/acl.json";
    aclError ret = aclInit(aclConfigPath);
    if (ret != ACL_SUCCESS) {
        ERROR_LOG("acl init failed, errorCode = %d", static_cast<int32_t>(ret));
        return FAILED;
    }
    INFO_LOG("acl init success");

    // set device
    ret = aclrtSetDevice(deviceId_);
    if (ret != ACL_SUCCESS) {
        ERROR_LOG("acl set device %d failed, errorCode = %d", deviceId_, static_cast<int32_t>(ret));
        return FAILED;
    }
    INFO_LOG("set device %d success", deviceId_);

    // create context (set current)
    ret = aclrtCreateContext(&context_, deviceId_);
    if (ret != ACL_SUCCESS) {
        ERROR_LOG("acl create context failed, deviceId = %d, errorCode = %d",
            deviceId_, static_cast<int32_t>(ret));
        return FAILED;
    }
    INFO_LOG("create context success");

    // create stream
    ret = aclrtCreateStream(&stream_);
    if (ret != ACL_SUCCESS) {
        ERROR_LOG("acl create stream failed, deviceId = %d, errorCode = %d",
            deviceId_, static_cast<int32_t>(ret));
        return FAILED;
    }
    INFO_LOG("create stream success");

    // create output stream
    ret = aclrtCreateStream(&streamOutput_);
    if (ret != ACL_SUCCESS) {
        ERROR_LOG("acl create output stream failed, deviceId = %d, errorCode = %d",
            deviceId_, static_cast<int32_t>(ret));
        return FAILED;
    }
    INFO_LOG("create output stream success");

    // get run mode
    // runMode is ACL_HOST which represents app is running in host
    // runMode is ACL_DEVICE which represents app is running in device
    ret = aclrtGetRunMode(&runMode_);
    if (ret != ACL_SUCCESS) {
        ERROR_LOG("acl get run mode failed, errorCode = %d", static_cast<int32_t>(ret));
        return FAILED;
    }
    return SUCCESS;
}


Result SampleProcess::Postprocess(std::string& send_info, const aclmdlDataset* modelOutput, PicDesc &picDesc, int modelWidth, int modelHeight, int jpeg_port) {
    uint32_t dataSize = 0;
    float* detectData = (float *)GetInferenceOutputItem(dataSize, modelOutput,
    kBBoxDataBufId);

    uint32_t* boxNum = (uint32_t *)GetInferenceOutputItem(dataSize, modelOutput,
    kBoxNumDataBufId);
    if (boxNum == nullptr) return FAILED;

    uint32_t totalBox = boxNum[0];
    vector<BBox> detectResults;
    static int frame_id = 0;
    bool g_ProcessRun = false;
    char localTime[256] = {0};
    getLocalTime(localTime);
    char tmp[4096] = {0};
    sprintf(tmp, "{\"frame_id\":%d,\"time\":\"%s\",\"objects\": [", frame_id++,localTime);
    send_info += tmp;
    int first = 0;

    float widthScale = (float)(picDesc.width) / modelWidth;
    float heightScale = (float)(picDesc.height) / modelHeight;
    for (uint32_t i = 0; i < totalBox; i++) {
        BBox boundBox;

        uint32_t objIndex = (uint32_t)detectData[totalBox * LABEL + i];
        if ((yolov3Label[objIndex]==string("dont_show")) || (detectData[totalBox * SCORE + i] < conf_)) continue;
        first += 1;
        if (first > 1) {
            send_info += ",";
        }

        uint32_t score = uint32_t(detectData[totalBox * SCORE + i] * 100);
        boundBox.rect.ltX = detectData[totalBox * TOPLEFTX + i] * widthScale;
        boundBox.rect.ltY = detectData[totalBox * TOPLEFTY + i] * heightScale;
        boundBox.rect.rbX = detectData[totalBox * BOTTOMRIGHTX + i] * widthScale;
        boundBox.rect.rbY = detectData[totalBox * BOTTOMRIGHTY + i] * heightScale;

        // uint32_t objIndex = (uint32_t)detectData[totalBox * LABEL + i];
        boundBox.text = yolov3Label[objIndex] + std::to_string(score) + "\%";
        // printf("%d %d %d %d %s\n", boundBox.rect.ltX, boundBox.rect.ltY,
        // boundBox.rect.rbX, boundBox.rect.rbY, boundBox.text.c_str());

        auto cx = (((float)boundBox.rect.ltX + (float)boundBox.rect.rbX) / 2) / (float)(picDesc.width);
        auto cy = (((float)(boundBox.rect.ltY) + (float)(boundBox.rect.rbY)) / 2) / (float)(picDesc.height);
        auto cWidth = ((float)boundBox.rect.rbX - (float)boundBox.rect.ltX)/ (float)(picDesc.width); 
        auto cHeight = ((float)boundBox.rect.rbY - (float)boundBox.rect.ltY) / (float)(picDesc.height); 
        
        sprintf(tmp, "  {\"class_id\":%d, \"name\":\"%s\", \"relative_coordinates\":{\"center_x\":%f, \"center_y\":%f, \"width\":%f, \"height\":%f}, \"confidence\":%.2f}",
        objIndex, yolov3Label[objIndex].c_str(), cx, cy, cWidth, cHeight, float(score)/float(100));
        // objIndex, yolov3Label[objIndex].c_str(), boundBox.rect.ltX, boundBox.rect.ltY, boundBox.rect.rbX, boundBox.rect.rbY, float(score)/float(100));

        send_info += tmp;

        detectResults.emplace_back(boundBox);
    }

    send_info += "]}";
    
    string rtsp = to_string(jpeg_port);
    std::string outpath = rtsp +"/rtsp";
    char ch[50];
    strcpy(ch, outpath.c_str());
    if (frame_id % 250 == 0){
        int res = detectProcessByName(ch);
        printf("%d %s\n", res, ch);
        if(res < 0)
        {
            printf("not find ffmpeg rtsp_server_url\n");
            exit(0);
        }
    }

    DrawBoundBoxToImage(detectResults, picDesc);
    if (runMode_ == ACL_HOST) {
        delete[]((uint8_t *)detectData);
        delete[]((uint8_t*)boxNum);
    }

    return SUCCESS;
}

void* SampleProcess::GetInferenceOutputItem(uint32_t& itemDataSize,
const aclmdlDataset* inferenceOutput, uint32_t idx) {
    aclDataBuffer* dataBuffer = aclmdlGetDatasetBuffer(inferenceOutput, idx);
    if (dataBuffer == nullptr) {
        ERROR_LOG("Get the %dth dataset buffer from model "
        "inference output failed", idx);
        return nullptr;
    }

    void* dataBufferDev = aclGetDataBufferAddr(dataBuffer);
    if (dataBufferDev == nullptr) {
        ERROR_LOG("Get the %dth dataset buffer address "
        "from model inference output failed", idx);
        return nullptr;
    }

    size_t bufferSize = aclGetDataBufferSizeV2(dataBuffer);
    if (bufferSize == 0) {
        ERROR_LOG("The %dth dataset buffer size of "
        "model inference output is 0", idx);
        return nullptr;
    }

    void* data = nullptr;
    if (runMode_ == ACL_HOST) {
        data = Utils::CopyDataDeviceToLocal(dataBufferDev, bufferSize);
        if (data == nullptr) {
            ERROR_LOG("Copy inference output to host failed");
            return nullptr;
        }
    } else {
        data = dataBufferDev;
    }

    itemDataSize = bufferSize;
    return data;
}

void SampleProcess::DrawBoundBoxToImage(vector<BBox>& detectionResults, PicDesc &picDesc) {
    // cv::Mat ori_mat = picDesc.origImage.clone();

    for (int i = 0; i < detectionResults.size(); ++i) {
        cv::Point p1, p2;
        p1.x = detectionResults[i].rect.ltX;
        p1.y = detectionResults[i].rect.ltY;
        p2.x = detectionResults[i].rect.rbX;
        p2.y = detectionResults[i].rect.rbY;
        cv::rectangle(picDesc.origImage, p1, p2, kColors[i % kColors.size()], kLineSolid);
        cv::putText(picDesc.origImage, detectionResults[i].text, cv::Point(p1.x, p1.y + kLabelOffset),
        cv::FONT_HERSHEY_COMPLEX, kFountScale, kFontColor);
    }
    if (drawImageInfo.drawAreaFlag) {
        cv::Scalar color = cv::Scalar(drawImageInfo.color[0], drawImageInfo.color[1], drawImageInfo.color[2]);
        cv::polylines(picDesc.origImage, drawImageInfo.contours, true, color,drawImageInfo.width);

    }
    if (drawImageInfo.drawLineFlag) {
        cv::Scalar color = cv::Scalar(drawImageInfo.color[0], drawImageInfo.color[1], drawImageInfo.color[2]);
        cv::line(picDesc.origImage, drawImageInfo.startPoint, drawImageInfo.endPoint, color, drawImageInfo.width);
        // cv::line(mat, cv::Point(200,300), cv::Point(300,500), color, drawImageInfo.width);

    }
    firstFlag = false;

    try {
        // cv::resize(picDesc.origImage, picDesc.origImage, cv::Size(drawImageInfo.output_w, drawImageInfo.output_h));    
        // jpg_sender_->write(picDesc.origImage);
        // cv::imwrite("1.jpg", picDesc.origImage);
        fwrite(picDesc.origImage.data, sizeof(char), picDesc.origImage.total() * picDesc.origImage.elemSize(), ffmpeg_pipe);
    }
    catch (...) {
        cerr << " Error in send_mjpeg() function \n";
    }
}

Result SampleProcess::Process(int jpeg_port)
{
    // dvpp init
    DvppProcess dvppProcess(stream_);
    Result ret = dvppProcess.InitResource();
    if (ret != SUCCESS) {
        ERROR_LOG("init dvpp resource failed");
        return FAILED;
    }

    // output dvpp init
    DvppProcess dvppProcessOutput(streamOutput_);
    ret = dvppProcessOutput.InitResource();
    if (ret != SUCCESS) {
        ERROR_LOG("init output dvpp resource failed");
        return FAILED;
    }

    // model init
    ModelProcess modelProcess;
    const char* omModelPath = "../model/yolov3.om";
    ret = modelProcess.LoadModel(omModelPath);
    if (ret != SUCCESS) {
        ERROR_LOG("execute LoadModel failed");
        return FAILED;
    }
    ret = modelProcess.CreateDesc();
    if (ret != SUCCESS) {
        ERROR_LOG("execute CreateDesc failed");
        return FAILED;
    }
    ret = modelProcess.CreateOutput();
    if (ret != SUCCESS) {
        ERROR_LOG("execute CreateOutput failed");
        return FAILED;
    }
    int modelInputWidth;
    int modelInputHeight;
    ret = modelProcess.GetModelInputWH(modelInputWidth, modelInputHeight);
    if (ret != SUCCESS) {
        ERROR_LOG("execute GetModelInputWH failed");
        return FAILED;
    }

    const float imageInfo[4] = {(float)modelInputWidth, (float)modelInputHeight,
    (float)modelInputWidth, (float)modelInputHeight};
    size_t imageInfoSize_ = sizeof(imageInfo);
    void *imageInfoBuf_;
    if (runMode_ == ACL_HOST)
        imageInfoBuf_ = Utils::CopyDataHostToDevice((void *)imageInfo, imageInfoSize_);
    else
        imageInfoBuf_ = Utils::CopyDataDeviceToDevice((void *)imageInfo, imageInfoSize_);
    if (imageInfoBuf_ == nullptr) {
        ERROR_LOG("Copy image info to device failed");
        return FAILED;
    }

    // ffmpeg && vdec init
    DecodeProcess decodeProcess(stream_, runMode_, streamName_, context_);
    ret = decodeProcess.InitResource();
    if (ret != SUCCESS) {
        ERROR_LOG("init ffmpeg & vdec resource failed");
        return FAILED;
    }


    PicDesc testPic;
    bool readflag = true;
    while(readflag){
        ret = decodeProcess.ReadFrame(testPic);
        if (ret != SUCCESS) {
            break;
        }

        if (drawImageInfo.drawAreaFlag && !drawImageInfo.updateAreaFlag){
            float widthScale = (float)(drawImageInfo.output_w) / testPic.width;
            float heightScale = (float)(drawImageInfo.output_h) / testPic.height;
            for (int i = 0; i < drawImageInfo.contours.size(); ++i) {
                for (int j = 0; j < drawImageInfo.contours[i].size(); ++j) {
                    drawImageInfo.contours[i][j].x = static_cast<int>(drawImageInfo.contours[i][j].x * widthScale);
                    drawImageInfo.contours[i][j].y = static_cast<int>(drawImageInfo.contours[i][j].y * heightScale);
                }
            }
            drawImageInfo.updateAreaFlag = true;
        }

        if (drawImageInfo.drawLineFlag && !drawImageInfo.updateLineFlag){
            float widthScale = (float)(drawImageInfo.output_w) / testPic.width;
            float heightScale = (float)(drawImageInfo.output_h) / testPic.height;
            drawImageInfo.startPoint.x *= widthScale;
            drawImageInfo.startPoint.y *= heightScale;
            drawImageInfo.endPoint.x *= widthScale;
            drawImageInfo.endPoint.y *= heightScale;
            drawImageInfo.updateLineFlag = true;
        }

        ret = dvppProcess.InitDvppOutputPara(modelInputWidth, modelInputHeight);
        if (ret != SUCCESS) {
            ERROR_LOG("init dvpp output para failed");
            return FAILED;
        }
        ret = dvppProcess.Process(testPic);
        if (ret != SUCCESS) {
            ERROR_LOG("dvpp process failed");
            return FAILED;
        }
        void *dvppOutputBuffer = nullptr;
        int dvppOutputSize;
        dvppProcess.GetDvppOutput(&dvppOutputBuffer, dvppOutputSize);

        // 2.model process
        ret = modelProcess.CreateInput(dvppOutputBuffer, dvppOutputSize,
                                    imageInfoBuf_, imageInfoSize_);
        if (ret != SUCCESS) {
            ERROR_LOG("execute CreateInput failed");
            (void)acldvppFree(dvppOutputBuffer);
            return FAILED;
        }

        ret = modelProcess.Execute();
        if (ret != SUCCESS) {
            ERROR_LOG("execute inference failed");
            (void)acldvppFree(dvppOutputBuffer);
            return FAILED;
        }
        // release model input buffer
        (void)acldvppFree(dvppOutputBuffer);
        modelProcess.DestroyInput();

        const aclmdlDataset *modelOutput = modelProcess.GetModelOutputData();

        // output dvpp
        ret = dvppProcessOutput.InitDvppOutputPara(drawImageInfo.output_w, drawImageInfo.output_h);
        if (ret != SUCCESS) {
            ERROR_LOG("init output dvpp output para failed");
            return FAILED;
        }

        ret = dvppProcessOutput.Process(testPic);
        if (ret != SUCCESS) {
            ERROR_LOG("output dvpp process failed");
            return FAILED;
        }

        void *dvppOutputBuffer2 = nullptr;
        int dvppOutputSize2;
        dvppProcessOutput.GetDvppOutput(&dvppOutputBuffer2, dvppOutputSize2);

        void *hostImage = Utils::CopyDataDeviceToLocal(dvppOutputBuffer2, dvppOutputSize2);
        cv::Mat yuvimg(drawImageInfo.output_h * 3 / 2, drawImageInfo.output_w, CV_8UC1, hostImage);
        cv::cvtColor(yuvimg, testPic.origImage, CV_YUV2BGR_NV21);
        testPic.width = drawImageInfo.output_w;
        testPic.height = drawImageInfo.output_h;

        (void)acldvppFree(dvppOutputBuffer2);

        string send_info;
        ret = Postprocess(send_info, modelOutput, testPic, modelInputWidth, modelInputHeight, jpeg_port);
        if (ret != SUCCESS) {
            ERROR_LOG("Postprocess failed");
            return FAILED;
        }

        try {
            json_sender_->write(send_info.c_str());
        }
        catch (...) {
            cerr << " Error in send_json() function \n";
        }


        delete[]((uint8_t*)hostImage);

        }
    aclrtFree(imageInfoBuf_);
    // outputVideo_.release();
    return SUCCESS;
}

void SampleProcess::DestroyResource()
{
    pclose(ffmpeg_pipe);
    std::cout << "open ffmpeg cmd sucees: " << command.str().c_str() << std::endl;
    aclError ret;
    if (stream_ != nullptr) {
        ret = aclrtDestroyStream(stream_);
        if (ret != ACL_SUCCESS) {
            ERROR_LOG("destroy stream failed, errorCode = %d", static_cast<int32_t>(ret));
        }
        stream_ = nullptr;
    }
    INFO_LOG("end to destroy stream");

    if (context_ != nullptr) {
        ret = aclrtDestroyContext(context_);
        if (ret != ACL_SUCCESS) {
            ERROR_LOG("destroy context failed, errorCode = %d", static_cast<int32_t>(ret));
        }
        context_ = nullptr;
    }
    INFO_LOG("end to destroy context");

    ret = aclrtResetDevice(deviceId_);
    if (ret != ACL_SUCCESS) {
        ERROR_LOG("reset device %d failed, errorCode = %d", deviceId_, static_cast<int32_t>(ret));
    }
    INFO_LOG("end to reset device %d", deviceId_);

    ret = aclFinalize();
    if (ret != ACL_SUCCESS) {
        ERROR_LOG("finalize acl failed, errorCode = %d", static_cast<int32_t>(ret));
    }
    INFO_LOG("end to finalize acl");
}

DrawImageInfo SampleProcess::GetJsonFromEtcBus()
{
    DrawImageInfo drawImageInfo;
    char cwd[1024];
    if (getcwd(cwd, sizeof(cwd)) != nullptr)
    {
        string path(cwd);
        string last_path;
        size_t last_slash_idx = path.rfind('/');
        path = path.erase(last_slash_idx);
        last_slash_idx = path.rfind('/');
        last_path = path.substr(last_slash_idx + 2);
        // 定义bus配置文件路径
        vector<string> files = {"../business.json", "/root/MBAB/AI/" + last_path + "/etc/business.json", "/home/MBAB/AI/" + last_path + "/etc/business.json"};
        string bus_json_path;

        for (const auto& file : files) {
            ifstream inFile(file);
            if (inFile.is_open()) {
                bus_json_path = file;
                cout << "Json directory: " << bus_json_path  << endl;
                inFile.close();
                break;
            }
        }
        // 获取json并解析
        ifstream file(bus_json_path);
        if (!file.is_open()){
            cout << "not found business.json" << endl;
            drawImageInfo.drawAreaFlag = false;
            drawImageInfo.drawLineFlag = false;
            return drawImageInfo;
        }
        

        string content((istreambuf_iterator<char>(file)), istreambuf_iterator<char>());
        
        nlohmann::json j = nlohmann::json::parse(content);
        cout << j << endl;

        if (j["business_params"].contains("out_size")){
            nlohmann::json output_size = j["business_params"]["out_size"];
            drawImageInfo.output_w = output_size["out_width"].get<int>();
            drawImageInfo.output_h = output_size["out_height"].get<int>();
        }

        if (j["business_params"].contains("add_line"))
        {
            
            nlohmann::json add_line_j = j["business_params"]["add_line"];
            drawImageInfo.color = add_line_j["color"][0].get<vector<int>>();
            drawImageInfo.width = add_line_j["width"][0].get<int>();
            if (add_line_j.count("lines"))
            {
                cout << "Draw area flag is True!" << endl;
                vector<vector<int>> area = add_line_j["lines"][0].get<vector<vector<int>>>();
                vector<vector<cv::Point>> contours;
                vector<cv::Point> point_list;
                for (const auto& inner_vector : area) {
                    // point_list.push_back({static_cast<u_int8_t>(inner_vector[0]), static_cast<u_int8_t>(inner_vector[1])});
                    point_list.push_back(cv::Point(inner_vector[0], inner_vector[1]));
                }
                contours.push_back(point_list);
                drawImageInfo.drawAreaFlag = true;
                drawImageInfo.contours = contours;
            }
            else
            {
                cout << "Draw area flag is False!" << endl;
                drawImageInfo.drawAreaFlag = false;
            }
            if (add_line_j.count("line"))
            {
                cout << "Draw line flag is True!" << endl;
                vector<vector<int>> line = add_line_j["line"].get<vector<vector<int>>>();
                cout << "start point x:" << line[0][0] << "start point y:" << line[0][1] << endl;
                cv::Point startPoint = cv::Point(line[0][0], line[0][1]);
                cv::Point endPoint = cv::Point(line[1][0], line[1][1]);
                drawImageInfo.drawLineFlag = true;
                drawImageInfo.startPoint = startPoint;
				drawImageInfo.endPoint = endPoint;
            }
            else
            {
                cout << "Draw line flag is False!" << endl;
                drawImageInfo.drawLineFlag = false;
            }
        }
        else
        {
            cout << "Dont need to draw area or line!" << endl;
            drawImageInfo.drawAreaFlag = false;
            drawImageInfo.drawLineFlag = false;
        }
        return drawImageInfo;
     }
    else
    {
        cerr << "Failed to get current working directory." << endl;
    }
}
