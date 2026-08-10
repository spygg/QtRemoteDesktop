cmake_minimum_required(VERSION 3.5)
project(libmetartc8 C CXX)

set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(CMAKE_CXX_COMPILER_ID MATCHES "GNU")
    set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -O2 -std=gnu11")
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -O2 -std=gnu++11")
endif()

add_definitions(-D__STDC_FORMAT_MACROS)
add_definitions(-D_TIMESPEC_DEFINED)
if(WIN32)
    add_definitions(-D_WIN32_WINNT=0x0601)
endif()

set(HOME_BASE "${PROJECT_SOURCE_DIR}/..")

# metaRTC 自带 ffmpeg 头为裁剪版（缺 AVHWDeviceType 等），改用安装的完整 FFmpeg 头
set(FFMPEG_USER_INCLUDE "${BUILD_OUTPUT_DIR}/ffmpeg_install/include")

include_directories(
    ${HOME_BASE}/include
    ${HOME_BASE}/thirdparty/include
    ${HOME_BASE}/thirdparty/user_include
    ${FFMPEG_USER_INCLUDE}
    ${PROJECT_SOURCE_DIR}/src
)

set(SOURCES
    src/yangaudiodev/YangAudioCapture.cpp
    src/yangaudiodev/YangAudioCaptureData.cpp
    src/yangaudiodev/YangAudioCaptureHandle.cpp
    src/yangaudiodev/YangAudioPlay.cpp
    src/yangaudiodev/YangAudioRenderData.cpp
    src/yangcapture/YangCaptureFactory.cpp
    src/yangcapture/YangScreenShare.cpp
    src/yangcapture/YangVideoCapture.cpp
    src/yangcapture/YangVideoCaptureHandle.cpp
    src/yangencoder/YangAudioEncoder.cpp
    src/yangencoder/YangAudioEncoderHandle.cpp
    src/yangencoder/YangAudioEncoderHandleCb.cpp
    src/yangencoder/YangAudioEncoderOpus.cpp
    src/yangencoder/YangEncoderFactory.cpp
    src/yangencoder/YangEncoderMediacodec.cpp
    src/yangencoder/YangH264EncoderSoft.cpp
    src/yangencoder/YangH265EncoderSoft.cpp
    src/yangencoder/YangOpenH264Encoder.cpp
    src/yangencoder/YangVideoEncoder.cpp
    src/yangencoder/YangVideoEncoderHandle.cpp
    src/yangencoder/YangVideoEncoderMac.cpp
    src/yangdecoder/YangAudioDecoder.cpp
    src/yangdecoder/YangAudioDecoderHandle.cpp
    src/yangdecoder/YangAudioDecoderHandles.cpp
    src/yangdecoder/YangAudioDecoderOpus.cpp
    src/yangdecoder/YangDecoderFactory.cpp
    src/yangdecoder/YangDecoderMediacodec.cpp
    src/yangdecoder/YangH264DecoderSoft.cpp
    src/yangdecoder/YangHeaderParseFfmpeg.cpp
    src/yangdecoder/YangVideoDecoderFfmpeg.cpp
    src/yangdecoder/YangVideoDecoderHandle.cpp
    src/yangdecoder/YangVideoDecoderHandles.cpp
    src/yangdecoder/YangVideoDecoderMac.cpp
    src/yangutil/YangAvinfo.cpp
    src/yangutil/YangLoadLib.cpp
    src/yangutil/YangString.cpp
    src/yangutil/YangSysMessageHandle.cpp
    src/yangutil/YangThread.cpp
    src/yangutil/YangTimer.cpp
    src/yangutil/buffer/YangAudioBuffer.cpp
    src/yangutil/buffer/YangAudioEncoderBuffer.cpp
    src/yangutil/buffer/YangAudioPlayBuffer.cpp
    src/yangutil/buffer/YangMediaBuffer.cpp
    src/yangutil/buffer/YangVideoBuffer.cpp
    src/yangutil/buffer/YangVideoDecoderBuffer.cpp
    src/yangutil/buffer/YangVideoEncoderBuffer.cpp
    src/yangavutil/YangWave.cpp
    src/yangavutil/YangYuvConvert.cpp
    src/yangstream/YangStreamManager.cpp
    src/yangstream/YangSynBuffer.cpp
    src/yangstream/YangSynBufferManager.cpp
    src/yangrtc/YangPeerConnection8.cpp
)

if(WIN32)
    list(APPEND SOURCES
        src/yangcapture/win/YangDXGIManager.cpp
        src/yangcapture/win/YangVideoCaptureWindows.cpp
        src/yangcapture/win/YangWinVideoCaptureHandle.cpp
    )
elseif(UNIX AND NOT APPLE AND NOT ANDROID)
    list(APPEND SOURCES
        src/yangcapture/linux/YangVideoCaptureLinux.cpp
        src/yangaudiodev/linux/YangAudioAecLinux.cpp
        src/yangaudiodev/linux/YangAudioCaptureLinux.cpp
        src/yangaudiodev/linux/YangAudioPlayLinux.cpp
    )
endif()

add_library(metartc8 STATIC ${SOURCES})

# libmetartc8 依赖 util + core + openh264（Wels API 静态链接，FFmpeg 系运行时加载）
target_link_libraries(metartc8 PUBLIC yangutil8 metartccore8 openh264)
