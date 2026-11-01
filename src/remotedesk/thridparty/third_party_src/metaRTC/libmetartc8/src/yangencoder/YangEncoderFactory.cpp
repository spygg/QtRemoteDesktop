//
// Copyright (c) 2019-2026 yanggaofeng
//
#include <yangencoder/YangEncoderFactory.h>

#include <yangencoder/YangAudioEncoderOpus.h>


#include <yangencoder/YangGpuEncoderFactory.h>

#include <yangencoder/YangH265EncoderSoft.h>

#include <yangencoder/YangVideoEncoderMac.h>
#include <yangencoder/YangEncoderMediacodec.h>

#if Yang_Enable_Openh264
#include <yangencoder/YangOpenH264Encoder.h>
#else
#include <yangencoder/YangH264EncoderSoft.h>
#endif

YangEncoderFactory::YangEncoderFactory() {

}

YangEncoderFactory::~YangEncoderFactory() {

}


YangAudioEncoder* YangEncoderFactory::createAudioEncoder(YangAudioCodec acodec,
        YangAudioInfo *audioInfo) {

    return new YangAudioEncoderOpus();
}
YangAudioEncoder* YangEncoderFactory::createAudioEncoder(
        YangAudioInfo *audioInfo) {

    YangAudioCodec acodec=(YangAudioCodec)audioInfo->audioEncoderType;
    return createAudioEncoder(acodec, audioInfo);
}

YangVideoEncoder* YangEncoderFactory::createVideoEncoder(YangVideoCodec vcodec,YangVideoInfo *videoInfo) {

    if(videoInfo->videoEncHwType==0){
        if (vcodec == Yang_VED_H264){
#if Yang_Enable_Openh264
            return new YangOpenH264Encoder();
#else
            return  new YangH264EncoderSoft();
#endif
        }else if (vcodec == Yang_VED_H265){
            return new YangH265EncoderSoft();
        }
    }else{
#if Yang_OS_ANDROID
        return new YangEncoderMediacodec();
#elif Yang_OS_WIN
        #if Yang_Enable_GPU_Encoding
         YangGpuEncoderFactory gf;
         return gf.createGpuEncoder();
        #endif
#endif
        // Yang_OS_APPLE 故意不接 YangVideoEncoderMac：它依赖的 yang_create_ios_encoder
        // 实现在 src/yangmac/YangEncoderMac.mm 里，而那份 ObjC 源码只被上游 iOS 的
        // Xcode 工程编译，本仓库的 CMake 工程从来没编过它（.mm 不在任何 SOURCES 列表）。
        // 引用它会在链接期留下 undefined symbol。macOS 的硬编由我们自己的 FFmpeg
        // （--enable-videotoolbox → h264_videotoolbox）提供，这条 metaRTC 自建编码器
        // 的路径本项目不走（推流用的是预编码帧），所以与 Linux 一样返回 NULL。
    }
    return NULL;
}

YangVideoEncoder* YangEncoderFactory::createVideoEncoder(YangVideoInfo *videoInfo) {
    YangVideoCodec vcodec = Yang_VED_H264;
    if (videoInfo->videoEncoderType == 0)
        vcodec = Yang_VED_H264;
    if (videoInfo->videoEncoderType == 1)
        vcodec = Yang_VED_H265;

    return createVideoEncoder(vcodec, videoInfo);
}
