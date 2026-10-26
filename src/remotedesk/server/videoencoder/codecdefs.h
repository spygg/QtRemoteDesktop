#ifndef CODECDEFS_H
#define CODECDEFS_H

// 编码协议 / 硬件编码开关的枚举定义。
//
// 单独拆成一个**不依赖任何 FFmpeg 头文件**的小头：RDPServer 的配置读写
// （codec/hw_encode 字段）在"没有 FFmpeg、纯 JPEG 回退"的构建里同样需要这两个
// 枚举。以前它们写在 videoencoder.h 里，而 rdpserver.h 又无条件
// `#include "videoencoder.h"`，于是无 FFmpeg 构建会在
// `<libavcodec/avcodec.h> No such file` 处直接编译失败——尽管 CMake 打印的是
// "FFmpeg not found (optional) — JPEG fallback mode"，说明该模式本应可用。

enum class CodecType {
    H264,
    HEVC,   // H.265
    VP8,
    VP9,
    AV1,
    MPEG4,
    MJPEG
};

// 硬件编码开关：Auto=优先硬编、open 失败自动回退软编；On=强制硬编（不可用则告警回退软编，保证画面）；Off=禁用硬编
enum class HwEncodeMode { Auto, On, Off };

#endif // CODECDEFS_H
