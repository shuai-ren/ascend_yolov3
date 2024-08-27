# Copyright (c) Huawei Technologies Co., Ltd. 2019. All rights reserved.

# CMake lowest version requirement
cmake_minimum_required(VERSION 3.5.1)

# project information
project(ACL_DVPP_YOLOV3)

# Compile options
add_compile_options(-std=c++11)

add_definitions(-DENABLE_DVPP_INTERFACE)

# Specify target generation path
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY  "../../out")
set(CMAKE_CXX_FLAGS_DEBUG "-fPIC -O0 -g -Wall")
set(CMAKE_CXX_FLAGS_RELEASE "-fPIC -O2 -Wall")

if (NOT DEFINED ENV{INSTALL_DIR})
    MESSAGE(FATAL_ERROR "Not Defined INSTALL_DIR")
endif()

if (NOT DEFINED ENV{THIRDPART_PATH})
    MESSAGE(FATAL_ERROR "Not Defined THIRDPART_PATH")
endif()

if (NOT DEFINED ENV{CPU_ARCH})
    MESSAGE(FATAL_ERROR "Not Defined CPU_ARCH")
endif()

set(INC_PATH $ENV{DDK_PATH})

set(LIB_PATH $ENV{NPU_HOST_LIB})

if (NOT DEFINED ENV{NPU_HOST_LIB})
    set(LIB_PATH "/usr/local/Ascend/acllib/lib64/stub/")
    message(STATUS "set default LIB_PATH: ${LIB_PATH}")
else ()
    message(STATUS "env LIB_PATH: ${LIB_PATH}")
endif()

add_definitions(-DENABLE_DVPP_INTERFACE)
list(APPEND COMMON_DEPEND_LIB avcodec avformat avdevice avutil swresample avfilter swscale)
if ($ENV{CPU_ARCH} MATCHES "aarch64")
    if(EXISTS "$ENV{INSTALL_DIR}/driver/libmedia_mini.so")
        list(APPEND COMMON_DEPEND_LIB media_mini slog ascend_hal c_sec mmpa)
        add_definitions(-DENABLE_BOARD_CAMARE)
        message(STATUS "arch: arm")
    endif()
endif()

# Header path
include_directories(
    $ENV{INSTALL_DIR}/runtime/include/
	$ENV{THIRDPART_PATH}/include/
    $ENV{THIRDPART_PATH}/include/acllite
    /usr/local/Ascend/thirdpart/aarch64/include/opencv4
    #$ENV{INSTALL_DIR}/acllib/include/
    ${INC_PATH}/acllib/include/
    ../inc/
)

# add host lib path
link_directories(
    ${LIB_PATH}
    $ENV{HOME}/ascend_ddk/arm/lib/
    ${INC_PATH}/atc/lib64
    $ENV{HOME}/Ascend

    $ENV{INSTALL_DIR}/runtime/lib64/stub
    $ENV{THIRDPART_PATH}/lib/
    $ENV{THIRDPART_PATH}/lib64/
    $ENV{INSTALL_DIR}/driver
)

add_executable(main
        utils.cpp
        dvpp_process.cpp
        model_process.cpp
        sample_process.cpp
        decode.cpp
        main.cpp)

if (${CMAKE_HOST_SYSTEM_NAME} MATCHES "Windows")
    target_link_libraries(main
        libascendcl libacl_cblas libacl_dvpp opencv_core opencv_imgproc opencv_imgcodecs opencv_videoio pthread)
else()
    target_link_libraries(main
        ascendcl acl_cblas acl_dvpp stdc++ ${COMMON_DEPEND_LIB} opencv_core opencv_imgproc opencv_imgcodecs opencv_videoio pthread)
endif()

install(TARGETS main DESTINATION ${CMAKE_RUNTIME_OUTPUT_DIRECTORY})
