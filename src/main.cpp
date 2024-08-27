/**
* @file main.cpp
*
* Copyright (C) 2020. Huawei Technologies Co., Ltd. All rights reserved.
*
* This program is distributed in the hope that it will be useful,
* but WITHOUT ANY WARRANTY; without even the implied warranty of
* MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
*/
#include <iostream>
#include "sample_process.h"
#include "utils.h"
#include <fstream>
using namespace std;
vector<string> yolov3Label;

int main(int argc, char *argv[])
{
    //Check the input when the application executes, which takes the path to the input video file
    if((argc < 5) || (argv[1] == nullptr)){
        ERROR_LOG("Please input: ./main rtsp_port json_port confidence npuid video_url");
        return FAILED;
    }
    
    int jpeg_port = atoi(argv[1]);
    int json_port = atoi(argv[2]);
    float con = atof(argv[3]);
    int npu_id = atoi(argv[4]);
    string streamName = string(argv[5]);

    string obj;
    ifstream infile;
    infile.open("../model/obj.txt");
    if (!infile.is_open())
    {
        cout << "open file failure" << endl;
        exit(1);
    }

    while (!infile.eof())
    {

        infile >> obj;
        obj.erase(std::remove(obj.begin(), obj.end(), '\n'),obj.end());
        yolov3Label.push_back(obj);
    }
    yolov3Label.pop_back();
    infile.close();

    INFO_LOG("jpeg_port:%d json_port:%d", jpeg_port, json_port);
    std::string s1 = "./rtsp.sh";
    //启动python进程
    string rtsp_ = to_string(jpeg_port);
    std::string res = s1 + " " + rtsp_;
    auto rts = system(res.c_str());
    SampleProcess sampleProcess(jpeg_port, json_port, streamName, con, npu_id);
    Result ret = sampleProcess.InitResource();
    if (ret != SUCCESS) {
        ERROR_LOG("sample init resource failed");
        return FAILED;
    }

    ret = sampleProcess.Process(jpeg_port);
    if (ret != SUCCESS) {
        ERROR_LOG("sample process failed");
        return FAILED;
    }

    INFO_LOG("execute sample success");
    return SUCCESS;
}