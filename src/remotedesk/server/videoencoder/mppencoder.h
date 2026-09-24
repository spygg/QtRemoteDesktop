#ifndef MPPENCODER_H
#define MPPENCODER_H

#include <QByteArray>
#include <QImage>
#include <QString>
#include <atomic>

// RK3588 (Rockchip MPP) 硬件编码器封装。
// 仅在工程自编译的 librockchip-mpp 存在（USE_MPP，Linux/ARM 平台）时启用；
// 其他平台所有方法返回 false/空实现，不引入任何 MPP 符号依赖，保证跨平台编译。
// MPP 直连编码：软件帧（RGB32 → NV12）直接喂 MPP 硬件，H264/HEVC 均支持。
class MppEncoder {
public:
    MppEncoder();
    ~MppEncoder();

    // Rockchip MPP 平台探测（/dev/mpp_service 存在即认为可用）
    static bool isSupported();

    // codec: 0=H264, 1=HEVC（与 videoencoder.h CodecType 对齐）；forceHw=true 失败即失败
    bool initialize(int codec, int width, int height, int fps, int bitrate, bool forceHw);
    void shutdown();
    bool isActive() const { return active_; }

    // 编码一帧 RGB32 图像，输出 annexb ES 流（关键帧含 SPS/PPS，HEVC 含 VPS/SPS/PPS）
    bool encode(const QImage& frame, QByteArray& out, bool& keyframe);

    void setBitrate(int bitrate);
    // 强制下一帧为 IDR（MPP 无 force-IDR API，重建编码会话实现，仅 PLI 等低频触发）
    void requestKeyframe();

    const QString& name() const { return name_; }      // 如 "H.265 (hevc_rkmpp)"
    const QString& hwName() const { return hwName_; }  // 如 "hevc_rkmpp"

private:
    bool setupEncoder();   // mpp_create/init/cfg/header（可重复调用重建会话）
    void teardown();       // 释放会话与缓冲

    void* ctx_ = nullptr;    // MppCtx
    void* mpi_ = nullptr;    // MppApi*
    void* cfg_ = nullptr;    // MppEncCfg
    void* sws_ = nullptr;    // SwsContext*（RGB32 -> NV12）
    void* buffer_ = nullptr; // MppBuffer（NV12 帧缓冲，循环使用）
    int codec_ = 0;          // 0=H264 1=HEVC
    int width_ = 0, height_ = 0;
    int horStride_ = 0, verStride_ = 0;  // 16 字节对齐
    int fps_ = 30, bitrate_ = 2000000;
    bool active_ = false;
    QString name_, hwName_;
    std::atomic<int> pendingBitrate_{ 0 };
    std::atomic<bool> keyframeReq_{ false };
    qint64 frameCount_ = 0;
};

#endif // MPPENCODER_H
