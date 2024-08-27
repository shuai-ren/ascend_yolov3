/**
* Copyright 2020 Huawei Technologies Co., Ltd
*
* Licensed under the Apache License, Version 2.0 (the "License");
* you may not use this file except in compliance with the License.
* You may obtain a copy of the License at

* http://www.apache.org/licenses/LICENSE-2.0

* Unless required by applicable law or agreed to in writing, software
* distributed under the License is distributed on an "AS IS" BASIS,
* WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
* See the License for the specific language governing permissions and
* limitations under the License.

* File main.cpp
* Description: dvpp sample main func
*/
#include <cstdint>
#include <iostream>
#include <stdlib.h>
#include <dirent.h>
#include <unistd.h>
#include <memory>
#include <vector>
#include <thread>
#include <string>
#include "utils.h"
#include "decode.h"
#include <sys/time.h>
#include "acl/acl.h"
#include "opencv2/opencv.hpp"
#include "opencv2/imgproc/types_c.h"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
}

using namespace std;

void dvpp_callback(aclrtExceptionInfo * exception_info)
{
    uint32_t taskId = aclrtGetTaskIdFromExceptionInfo(exception_info);
    uint32_t streamId = aclrtGetStreamIdFromExceptionInfo(exception_info);
    uint32_t deviceId = aclrtGetDeviceIdFromExceptionInfo(exception_info);

    if((taskId == 0xffffffff) || (streamId == 0xffffffff)) {
        //Device异常，强制退出进程
        INFO_LOG("device error****************************");
        exit(0);
    } 

    return;
}

namespace {
    const int kReadSlow = 10;
    const uint32_t kDecodeFrameQueueSize = 256;
    const int kDecodeQueueOpWait = 20000;
    const int kQueueOpRetryTimes = 1000 - 500;    //1000
    const int kFrameEnQueueRetryTimes = 1000;
    const int kNoFlag = 0; // no flag
    const int kInvalidVideoIndex = -1; // invalid video index
    const string kRtspTransport = "rtsp_transport"; // rtsp transport
    const string kUdp = "udp"; // video format udp
    const string kTcp = "tcp";
    const string kBufferSize = "buffer_size"; // buffer size string
    const string kMaxBufferSize = "10485760"; // maximum buffer size:10MB
    const string kMaxDelayStr = "max_delay"; // maximum delay string
    const string kMaxDelayValue = "100000000"; // maximum delay time:100s
    const string kTimeoutStr = "stimeout"; // timeout string
    const string kTimeoutValue = "5000000"; // timeout:5s
    const string kPktSize = "pkt_size"; // ffmpeg pakect size string
    const string kPktSizeValue = "10485760"; // ffmpeg packet size value:10MB
    const string kReorderQueueSize = "reorder_queue_size"; // reorder queue size
    const string kReorderQueueSizeValue = "0"; // reorder queue size value
    const int kErrorBufferSize = 1024; // buffer size for error info
    const uint32_t kDefaultFps = 25;
    const int64_t kUsec = 1000000;
    const int kOutputJamWait = 10000;
    const int kInvalidTpye = -1;
    bool g_RunFlag = true;
}

DecodeProcess::DecodeProcess(aclrtStream& stream, aclrtRunMode& runMode, string streamName, aclrtContext& context):
context_(context),
stream_(stream), 
runMode_(runMode),
streamName_(streamName),
vdecChannelDesc_(nullptr),
streamInputDesc_(nullptr),
picOutputDesc_(nullptr),
picOutBufferDev_(nullptr),
frameImageQueue_(kDecodeFrameQueueSize), 
format_(1),
enType_(2),      //改为 0 则可处理265编码的数据
lastDecodeTime_(0),
streamFormat_(H264_MAIN_LEVEL) {
}

DecodeProcess::~DecodeProcess() {
    DestroyResource();
}

void* DecodeProcess::ThreadFunc(aclrtContext sharedContext)
{
    if (sharedContext == nullptr) {
        ERROR_LOG("sharedContext can not be nullptr");
        return ((void*)(-1));
    }
    INFO_LOG("use shared context for this thread");
    aclError ret = aclrtSetCurrentContext(sharedContext);
    if (ret != ACL_SUCCESS) {
        ERROR_LOG("aclrtSetCurrentContext failed, errorCode = %d", static_cast<int32_t>(ret));
        return ((void*)(-1));
    }

    while (g_RunFlag) {
        // Notice: timeout 1000ms
        (void)aclrtProcessReport(1000);
    }
    return (void*)0;
}

Result DecodeProcess::FrameImageEnQueue(shared_ptr<PicDesc> frameData) {
    for (int count = 0; count < kFrameEnQueueRetryTimes; count++) {
        if (frameImageQueue_.Push(frameData))
            return SUCCESS;
        usleep(kDecodeQueueOpWait); 
    }
    ERROR_LOG("Video %s lost decoded image for queue full", 
                    streamName_.c_str());

    return FAILED;
}

void DecodeProcess::SleeptoNextFrameTime() {
    while(frameImageQueue_.Size() >  kReadSlow) {
        if (isStop_) {
            return;
        }
        usleep(kOutputJamWait);
    }

    //get current time
    timeval tv;
    gettimeofday(&tv, 0);
    int64_t now = (int64_t)tv.tv_sec * 1000000 + (int64_t)tv.tv_usec;

    if (lastDecodeTime_ == 0) {
        lastDecodeTime_ = now;
        return;
    }
    //calculate interval
    int64_t lastInterval = (now - lastDecodeTime_);
    int64_t sleepTime = (lastInterval < fpsInterval_)?(fpsInterval_-lastInterval):0;
    //consume rest time
    usleep(sleepTime);
    //record start time of next frame
    gettimeofday(&tv, 0);
    lastDecodeTime_ = (int64_t)tv.tv_sec * 1000000 + (int64_t)tv.tv_usec;

    return;
}

Result DecodeProcess::FrameDecodeCallback(void* userData, void* frameData, int frameSize) {
    if ((frameData == NULL) || (frameSize == 0)) {
        ERROR_LOG("Frame data is null");
        return FAILED;
    }
    DecodeProcess* videoDecoder = (DecodeProcess*)userData;
    void* inBufferDev_;

    if (runMode_ == ACL_HOST){
        INFO_LOG("[DecodeProcess::FrameDecodeCallback]");
        inBufferDev_ = Utils::CopyDataHostToDevice((void *)frameData, frameSize);
    }
    else{
        inBufferDev_ = Utils::CopyDataDeviceToDevice((void *)frameData, frameSize);
    }

    if (inBufferDev_ == nullptr) {
        ERROR_LOG("Copy frame h26x data to dvpp failed");
        return FAILED;
    }

    size_t DataSize_ = (videoDecoder->frameWidth_ * videoDecoder->frameHeight_ * 3) / 2;
    aclError ret = acldvppSetStreamDescData(videoDecoder->streamInputDesc_, inBufferDev_);
    ret = acldvppSetStreamDescSize(videoDecoder->streamInputDesc_, frameSize);
    ret = acldvppMalloc(&(videoDecoder->picOutBufferDev_), DataSize_);
    videoDecoder->picOutputDesc_ = acldvppCreatePicDesc();
    ret = acldvppSetPicDescWidth(videoDecoder->picOutputDesc_,videoDecoder->frameWidth_);
    ret = acldvppSetPicDescHeight(videoDecoder->picOutputDesc_, videoDecoder->frameHeight_);
    ret = acldvppSetPicDescWidthStride(videoDecoder->picOutputDesc_, videoDecoder->frameWidth_);
    ret = acldvppSetPicDescHeightStride(videoDecoder->picOutputDesc_, videoDecoder->frameHeight_);
    ret = acldvppSetPicDescData(videoDecoder->picOutputDesc_, videoDecoder->picOutBufferDev_);
    ret = acldvppSetPicDescSize(videoDecoder->picOutputDesc_, DataSize_);
    ret = acldvppSetPicDescFormat(videoDecoder->picOutputDesc_, static_cast<acldvppPixelFormat>(videoDecoder->format_));
    ret = aclrtSetExceptionInfoCallback(dvpp_callback);
    ret = aclvdecSendFrame(videoDecoder->vdecChannelDesc_, videoDecoder->streamInputDesc_, videoDecoder->picOutputDesc_, nullptr, userData);

    if (ret != 0) {
        ret = acldvppFree(videoDecoder->picOutBufferDev_);
    }
    usleep(kOutputJamWait);
    if (runMode_ == ACL_HOST){
        ret = acldvppFree(inBufferDev_);
    }
    else{
        ret = acldvppFree(inBufferDev_);
    }
    return SUCCESS;
}

void DecodeProcess::callback(acldvppStreamDesc *input, acldvppPicDesc *output, void *userdata)
{
    DecodeProcess* decoder = (DecodeProcess*)userdata;
    int retCode = acldvppGetPicDescRetCode(output);                     //encode success or fail
    if (retCode == 0) {
        void *vdecOutBufferDev = acldvppGetPicDescData(output);
        uint32_t size = acldvppGetPicDescSize(output);

        //Get decoded image parameters
        shared_ptr<PicDesc> image = make_shared<PicDesc>();
        image->width = acldvppGetPicDescWidth(output);
        image->height = acldvppGetPicDescHeight(output);
        image->jpegDecodeSize = acldvppGetPicDescSize(output);
        image->data = SHARED_PTR_DVPP_BUF(vdecOutBufferDev);
        image->isLastFrame = false;

        if (YUV420SP_SIZE(image->width, image->height) != image->jpegDecodeSize) {
            ERROR_LOG("Invalid decoded frame parameter, "
            "width %d, height %d, size %d, buffer %p",
            image->width, image->height,
            image->jpegDecodeSize, image->data.get());
        }
        else{
            decoder->FrameImageEnQueue(image);
        }
    }
    else {
        void *vdecOutBufferDev = acldvppGetPicDescData(output);
        ERROR_LOG("vdec decode frame failed, error code: %d",retCode);
        aclError ret0 = acldvppFree(vdecOutBufferDev);
        ERROR_LOG("release, error code: %d", ret0);
    }

    aclError ret = acldvppDestroyPicDesc(output);

    if (input != nullptr) {
        void *vdecInBufferDev = acldvppGetStreamDescData(input);
        if (vdecInBufferDev != nullptr) {
            // aclError ret = aclrtFree(vdecInBufferDev);
            aclError ret = acldvppFree(vdecInBufferDev);
        }
    }
}

int DecodeProcess::GetVideoIndex(AVFormatContext* avFormatContext) {
    if (avFormatContext == nullptr) { // verify input pointer
        return kInvalidVideoIndex;
    }

    // get video index in streams
    for (uint32_t i = 0; i < avFormatContext->nb_streams; i++) {
        if (avFormatContext->streams[i]->codecpar->codec_type
            == AVMEDIA_TYPE_VIDEO) { // check is media type is video
            return i;
        }
    }

    return kInvalidVideoIndex;
}

void DecodeProcess::SetDictForRtsp(AVDictionary*& avdic) {
    INFO_LOG("Set parameters for %s", streamName_.c_str());
    av_dict_set(&avdic, kRtspTransport.c_str(), rtspTransport_.c_str(), kNoFlag);
    av_dict_set(&avdic, kBufferSize.c_str(), kMaxBufferSize.c_str(), kNoFlag);
    av_dict_set(&avdic, kMaxDelayStr.c_str(), kMaxDelayValue.c_str(), kNoFlag);
    av_dict_set(&avdic, kTimeoutStr.c_str(), kTimeoutValue.c_str(), kNoFlag);
    av_dict_set(&avdic, kReorderQueueSize.c_str(),
                kReorderQueueSizeValue.c_str(), kNoFlag);
    av_dict_set(&avdic, kPktSize.c_str(), kPktSizeValue.c_str(), kNoFlag);
    //bandwidth
    // av_dict_set(&avdic, "MaxClients", "10", 0);
    // av_dict_set(&avdic, "MaxBandwidth", "100000", 0);
    INFO_LOG("Set parameters for %s end", streamName_.c_str());
}

bool DecodeProcess::OpenVideo(AVFormatContext*& avFormatContext) {
    bool ret = true;
    AVDictionary* avdic = nullptr;
    av_log_set_level(AV_LOG_DEBUG);
    INFO_LOG("Open video %s ...", streamName_.c_str());
    SetDictForRtsp(avdic);
    int openRet = avformat_open_input(&avFormatContext,
                                       streamName_.c_str(), nullptr,
                                       &avdic);
    if (openRet < 0) { // check open video result
        char buf_error[kErrorBufferSize];
        av_strerror(openRet, buf_error, kErrorBufferSize);
        ERROR_LOG("Could not open video:%s, return :%d, error info:%s",
                      streamName_.c_str(), openRet, buf_error);
        ret = false;
    }
    if (avdic != nullptr) { // free AVDictionary
        av_dict_free(&avdic);
    }
    return ret;
}

void DecodeProcess::GetVideoInfo() {
    avformat_network_init(); // init network
    AVFormatContext* avFormatContext = avformat_alloc_context();
    bool ret = OpenVideo(avFormatContext);
    if (ret == false) {
        ERROR_LOG("Open %s failed", streamName_.c_str());
        return;
    }

    if (avformat_find_stream_info(avFormatContext,NULL)<0) {
        ERROR_LOG("Get stream info of %s failed", streamName_.c_str());
        return;
    }

    int videoIndex = GetVideoIndex(avFormatContext);
    if (videoIndex == kInvalidVideoIndex) { // check video index is valid
        ERROR_LOG("Video index is %d, current media stream has no "
                        "video info:%s",
                        kInvalidVideoIndex, streamName_.c_str());

        avformat_close_input(&avFormatContext);
        return;
    }

    AVStream* inStream = avFormatContext->streams[videoIndex];
    frameWidth_ = inStream->codecpar->width;
    frameHeight_ = inStream->codecpar->height;
    if (inStream->avg_frame_rate.den == 0 || inStream->avg_frame_rate.num == 0 && inStream->avg_frame_rate.den == 1) {
        fps_ = inStream->r_frame_rate.num / inStream->r_frame_rate.den;
    }
    else {
        fps_ = inStream->avg_frame_rate.num / inStream->avg_frame_rate.den;
    }

    videoType_ = inStream->codecpar->codec_id;
    profile_ = inStream->codecpar->profile;
    avformat_close_input(&avFormatContext);
    INFO_LOG("Video %s, type %d, profile %d, width:%d, height:%d, fps:%d",
                 streamName_.c_str(), videoType_, profile_, frameWidth_, frameHeight_, fps_);
    return;
}

void DecodeProcess::FFmpegDecoder(){
    rtspTransport_.assign(kTcp.c_str());
    isStop_ = false;
    GetVideoInfo();
}

int DecodeProcess::GetVdecType() {
    //VDEC only support　H265 main level，264 baseline level，main level，high level
    if (videoType_ == AV_CODEC_ID_HEVC) {
        enType_ = 0;   //265视频
        streamFormat_ = H265_MAIN_LEVEL;         
    } else if (videoType_ == AV_CODEC_ID_H264) {
        switch(profile_) {
            case FF_PROFILE_H264_BASELINE:
                streamFormat_ = H264_BASELINE_LEVEL;
                break;
            case FF_PROFILE_H264_MAIN:
                streamFormat_ = H264_MAIN_LEVEL;
                break;
            case FF_PROFILE_H264_HIGH:
            case FF_PROFILE_H264_HIGH_10:
            case FF_PROFILE_H264_HIGH_10_INTRA:
            case FF_PROFILE_H264_MULTIVIEW_HIGH:
            case FF_PROFILE_H264_HIGH_422:
            case FF_PROFILE_H264_HIGH_422_INTRA:
            case FF_PROFILE_H264_STEREO_HIGH:
            case FF_PROFILE_H264_HIGH_444:
            case FF_PROFILE_H264_HIGH_444_PREDICTIVE:
            case FF_PROFILE_H264_HIGH_444_INTRA:
                streamFormat_ = H264_HIGH_LEVEL;
                break;
            default:
                INFO_LOG("Not support h264 profile %d, use as mp", profile_);
                streamFormat_ = H264_MAIN_LEVEL; 
                break;
        }
    } else {
        streamFormat_ = kInvalidTpye;
        ERROR_LOG("Not support stream, type %d,  profile %d", videoType_, profile_);
    }

    return streamFormat_;
}

void DecodeProcess::InitVideoStreamFilter(const AVBitStreamFilter*& videoFilter) {
    if (videoType_ == AV_CODEC_ID_H264) { // check video type is h264
        videoFilter = av_bsf_get_by_name("h264_mp4toannexb");
    }
    else { // the video type is h265
        videoFilter = av_bsf_get_by_name("hevc_mp4toannexb");
    }
}

bool DecodeProcess::InitVideoParams(int videoIndex, 
                    AVFormatContext* avFormatContext,
                    AVBSFContext*& bsfCtx) {
    const AVBitStreamFilter* videoFilter;
    InitVideoStreamFilter(videoFilter);
    if (videoFilter == nullptr) { // check video fileter is nullptr
        ERROR_LOG("Unkonw bitstream filter, videoFilter is nullptr!");
        return false;
    }

    // checke alloc bsf context result
    if (av_bsf_alloc(videoFilter, &bsfCtx) < 0) {
        ERROR_LOG("Fail to call av_bsf_alloc!");
        return false;
    }

    // check copy parameters result
    if (avcodec_parameters_copy(bsfCtx->par_in,
        avFormatContext->streams[videoIndex]->codecpar) < 0) {
        ERROR_LOG("Fail to call avcodec_parameters_copy!");
        return false;
    }

    bsfCtx->time_base_in = avFormatContext->streams[videoIndex]->time_base;

    // check initialize bsf contextreult
    if (av_bsf_init(bsfCtx) < 0) {
        ERROR_LOG("Fail to call av_bsf_init!");
        return false;
    }

    return true;
}

Result DecodeProcess::SetAclContext() {
    if (context_ == nullptr) {
        ERROR_LOG("Video decoder context is null");
        return FAILED;
    }
    
    aclError ret = aclrtSetCurrentContext(context_);
    if (ret != ACL_SUCCESS) {
        ERROR_LOG("Video decoder set context failed, error: %d", ret);
        return FAILED;
    }
    return SUCCESS;   
}

// static int ReadTimeoutCallback(void *read_frame_st)
// {
//     //获取开始时间
//     std::chrono::high_resolution_clock::time_point *start
//         = (std::chrono::high_resolution_clock::time_point *)read_frame_st;
//     //获取当前时间
//     auto now = std::chrono::high_resolution_clock::now();
//     //计算时间差 毫秒
//     float elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - *start).count();
//     //判断是否超时
//     if (elapsed > 1000)
//     {
//         ERROR_LOG("Read frame timeout");
//         return -1;
//     }

//     return 0;
// }

void DecodeProcess::Decode(void* callbackParam) {
    DecodeProcess* thisPtr =  (DecodeProcess*)callbackParam;
    INFO_LOG("Start ffmpeg decode video %s ...", thisPtr->streamName_.c_str());

    aclError aclRet = thisPtr->SetAclContext();
    if (aclRet != ACL_SUCCESS) {
        ERROR_LOG("Set frame decoder context failed, errorno:%d",
                        aclRet);
        return;
    }

    avformat_network_init(); // init network
    AVFormatContext* avFormatContext = avformat_alloc_context();

    // auto read_frame_st = std::chrono::high_resolution_clock::now();
    // avFormatContext->interrupt_callback.callback = ReadTimeoutCallback;
    // avFormatContext->interrupt_callback.opaque = &read_frame_st;

    // check open video result
    if (!thisPtr->OpenVideo(avFormatContext)) {
        return;
    }

    int videoIndex = thisPtr->GetVideoIndex(avFormatContext);
    if (videoIndex == kInvalidVideoIndex) { // check video index is valid
        ERROR_LOG("Rtsp %s index is -1", thisPtr->streamName_.c_str());
        return;
    }

    AVBSFContext* bsfCtx = nullptr;
    // check initialize video parameters result
    if (!thisPtr->InitVideoParams(videoIndex, avFormatContext, bsfCtx)) {
        return;
    }

    INFO_LOG("Start decode frame of video %s ...", thisPtr->streamName_.c_str());

    AVPacket avPacket;
    int processOk = true;
    int maxRetryCount = 3;  //最大重试次数
    int retryCount = 0;
    // loop to get every frame from video stream
    while (true) {
        INFO_LOG("[DecodeProcess::Decode] while loop -> av_read_frame");
        auto rec_ret = av_read_frame(avFormatContext, &avPacket);
        if (rec_ret < 0)
        {
            std::cerr << "Error reading frame." << std::endl;
            if (retryCount < maxRetryCount)
            {

                // av_bsf_free(&bsfCtx); // free AVBSFContext pointer
                // avformat_close_input(&avFormatContext); // close input video

                // avformat_network_init(); // init network
                // AVFormatContext* avFormatContext = avformat_alloc_context();

                // read_frame_st = std::chrono::high_resolution_clock::now();
                // avFormatContext->interrupt_callback.callback = ReadTimeoutCallback;
                // avFormatContext->interrupt_callback.opaque = &read_frame_st;

                // // check open video result
                // if (!thisPtr->OpenVideo(avFormatContext)) {
                //     return;
                // }

                // int videoIndex = thisPtr->GetVideoIndex(avFormatContext);
                // if (videoIndex == kInvalidVideoIndex) { // check video index is valid
                //     ERROR_LOG("Rtsp %s index is -1", thisPtr->streamName_.c_str());
                //     return;
                // }

                // AVBSFContext* bsfCtx = nullptr;
                // // check initialize video parameters result
                // if (!thisPtr->InitVideoParams(videoIndex, avFormatContext, bsfCtx)) {
                //     return;
                // }

                // INFO_LOG("Start decode frame of video %s ...", thisPtr->streamName_.c_str());
                


                //重连
                AVDictionary* avdic = nullptr;
                av_log_set_level(AV_LOG_DEBUG);
                INFO_LOG("Open video %s ...", thisPtr->streamName_.c_str());
                thisPtr->SetDictForRtsp(avdic);
                std::cout << "Retry..." << std::endl;
                usleep(1000*1000);  //等待1秒钟
                avformat_close_input(&avFormatContext);
                if (avformat_open_input(&avFormatContext,
                                       thisPtr->streamName_.c_str(), nullptr,
                                       &avdic) != 0)
                {
                    std::cerr << "Could not open input file." << std::endl;
                    return ;
                }
                if (avformat_find_stream_info(avFormatContext, NULL) < 0)
                {
                    std::cerr << "Failed to retrieve input stream information." << std::endl;
                    return ;
                }

                // av_dict_set(&options, "stimeout", "2000000", 0);
                // av_dict_set(&options, "max_delay", "500000", 0);
                // av_dict_set(&options, "rtsp_transport", "tcp", 0);
                INFO_LOG("Open video retry111111111111");
                // avformat_find_stream_info(avFormatContext, NULL);
                INFO_LOG("Open video retry222222222222222");

                av_dump_format(avFormatContext, 0, thisPtr->streamName_.c_str(), 0);
                INFO_LOG("Open video retry33333333333333");
                retryCount++;
                continue;
                INFO_LOG("Open video retry  %s ... success", thisPtr->streamName_.c_str());
            }
            else
            {
                break;
            }
        }
        retryCount = 0;
        if(processOk && !thisPtr->isStop_){
            if (avPacket.stream_index == videoIndex) { // check current stream is video
            // send video packet to ffmpeg
            INFO_LOG("[DecodeProcess::Decode] while loop -> av_bsf_send_packet");
            if (av_bsf_send_packet(bsfCtx, &avPacket)) {
                ERROR_LOG("Fail to call av_bsf_send_packet, channel id:%s",
                    thisPtr->streamName_.c_str());
            }

            // receive single frame from ffmpeg
            INFO_LOG("[DecodeProcess::Decode] while loop -> av_bsf_receive_packet");
            while ((av_bsf_receive_packet(bsfCtx, &avPacket) == 0) && !thisPtr->isStop_) {
                INFO_LOG("[DecodeProcess::Decode] while loop -> FrameDecodeCallback");
                int ret = thisPtr->FrameDecodeCallback(callbackParam, avPacket.data, avPacket.size);
                INFO_LOG("[DecodeProcess::Decode] while loop -> FrameDecodeCallback done, ret: %d", ret);
                if (ret != 0) {
                    processOk = false;
                    break;
                }
            }
        }
        av_packet_unref(&avPacket);

        }else{
            printf("processOk false or is stop-----------------------------\n");
            exit(0);
        }

    }
    av_bsf_free(&bsfCtx); // free AVBSFContext pointer
    avformat_close_input(&avFormatContext); // close input video

    shared_ptr<PicDesc> image = make_shared<PicDesc>();
    image->isLastFrame = true;
    thisPtr->FrameImageEnQueue(image);
    INFO_LOG("Ffmpeg decoder %s finished", thisPtr->streamName_.c_str());
}

Result DecodeProcess::VdecDecoder(){
    // create threadId
    pthread_create(&threadId_, nullptr, ThreadFunc, context_);
    (void)aclrtSubscribeReport(static_cast<uint64_t>(threadId_), stream_);
    vdecChannelDesc_ = aclvdecCreateChannelDesc();
    // channelId: 0-15
    aclError ret = aclvdecSetChannelDescChannelId(vdecChannelDesc_, 10);
    ret = aclvdecSetChannelDescThreadId(vdecChannelDesc_, threadId_);
    //Sets the callback function
    ret = aclvdecSetChannelDescCallback(vdecChannelDesc_, callback);
	//The H265_MAIN_LEVEL video encoding protocol is used in the example
    ret = aclvdecSetChannelDescEnType(vdecChannelDesc_, static_cast<acldvppStreamFormat>(enType_));
	//PIXEL_FORMAT_YVU_SEMIPLANAR_420 
    ret = aclvdecSetChannelDescOutPicFormat(vdecChannelDesc_, static_cast<acldvppPixelFormat>(format_));
    ret = aclvdecCreateChannel(vdecChannelDesc_);
    // Create input video stream description information, set the properties of the stream information
    streamInputDesc_ = acldvppCreateStreamDesc();
    return SUCCESS;
}

Result DecodeProcess::InitResource()
{
    if (context_ == nullptr) {
        aclError aclRet = aclrtGetCurrentContext(&context_);
        if ((aclRet != ACL_SUCCESS) || (context_ == nullptr)) {
            ERROR_LOG("Get current acl context error:%d", aclRet);
            return FAILED;
        }
    }

    //intialize ffmpeg decoder
    FFmpegDecoder();
    //verify video type
    if (kInvalidTpye == GetVdecType()) {      
        ERROR_LOG("Video %s type is invalid", streamName_.c_str());
    }

    Result ret = VdecDecoder();
    if (ret != SUCCESS) {
        ERROR_LOG("Vdec init resource failed");
    }
    INFO_LOG("Vdec init resource success");

    //Get video fps, if no fps, use 1 as default
    if (fps_ == 0) {
        fps_ = kDefaultFps;
        ERROR_LOG("Video %s fps is 0, change to %d",
                       streamName_.c_str(), fps_);
    }
    //Call the frame interval time(us)
    fpsInterval_ = kUsec / fps_;
    INFO_LOG("ffmped init resource success");

    decodeThread_ = thread(Decode, (void*)this);
    decodeThread_.detach();
    return SUCCESS;
}

Result DecodeProcess::ReadFrame(PicDesc& image){
    shared_ptr<PicDesc> frame = FrameImageOutQueue(false);        //segmentation fault
    if (frame == nullptr) {
        ERROR_LOG("No frame image to read abnormally");
        return FAILED;
    }
    if(frame->isLastFrame) {
        INFO_LOG("No frame to read anymore");
        return FAILED;
    }
    image.width = frame->width;
    image.height = frame->height;
    INFO_LOG("[DecodeProcess::ReadFrame]");
    image.jpegDecodeSize = frame->jpegDecodeSize;
    image.data = frame->data;
    return SUCCESS;
}

shared_ptr<PicDesc> DecodeProcess::FrameImageOutQueue(bool noWait) {
    shared_ptr<PicDesc> image = frameImageQueue_.Pop();

    if (noWait || (image != nullptr)) return image;

    for (int count = 0; count < kQueueOpRetryTimes - 1; count++) {
        INFO_LOG("[DecodeProcess::FrameImageOutQueue] OutQueue retry %d times", count);
        usleep(kDecodeQueueOpWait);
        image = frameImageQueue_.Pop();
        if (image != nullptr)
            return image;
    }

    return nullptr;
}

Result DecodeProcess::DestroyResource(){
    // acldvppFree(inBufferDev_);
    aclError ret;
    if(vdecChannelDesc_!=nullptr){
        ret = aclvdecDestroyChannel(vdecChannelDesc_);
        aclvdecDestroyChannelDesc(vdecChannelDesc_);
        vdecChannelDesc_ = nullptr;
    }
    (void)aclrtUnSubscribeReport(static_cast<uint64_t>(threadId_), stream_);
    
    if(streamInputDesc_!=nullptr){
        ret = acldvppDestroyStreamDesc(streamInputDesc_);
    }
    g_RunFlag = false;
    void *res = nullptr;
    pthread_join(threadId_, &res);

    do {
        shared_ptr<PicDesc> frame = FrameImageOutQueue(true);
        if (frame == nullptr) {
            break;
        }

        if (frame->data != nullptr) {
            acldvppFree(frame->data.get());
            frame->data = nullptr;
        }
    }while(1);
    return SUCCESS;
}
