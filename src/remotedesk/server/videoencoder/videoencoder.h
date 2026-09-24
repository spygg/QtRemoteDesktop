#ifndef VIDEOENCODER_H
#define VIDEOENCODER_H

#include <QByteArray>
#include <QImage>
#include <QMutex>
#include <QObject>
#include <QQueue>
#include <QString>
#include <QThread>
#include <QWaitCondition>
#include <atomic>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/avutil.h>
#include <libavutil/imgutils.h>
#include <libavutil/pixdesc.h>
#include <libswscale/swscale.h>
}

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

class MppEncoder; // RK3588 MPP 硬件编码器（USE_MPP 时启用，前向声明保持头文件跨平台）

class VideoEncoder : public QObject {
    Q_OBJECT
public:
    explicit VideoEncoder(QObject* parent = nullptr);
    ~VideoEncoder();

    bool initialize(CodecType type, int srcW, int srcH, int encW, int encH, int fps, int bitrate,
                    HwEncodeMode hwMode = HwEncodeMode::Auto);
    void encode(const QImage& frame);
    void shutdown();

    // 探测当前系统是否可对指定编码使用硬件编码（用于前端 auto 模式选路/模式通知）
    static bool isHwAcceleratedAvailable(CodecType type = CodecType::H264);
    // 当前可用的硬件编码器名（如 "h264_nvenc" / "hevc_rkmpp"），无则空串
    static QString hwEncoderName(CodecType type = CodecType::H264);
    // 当前实际使用的编码器描述（如 "H.264 (h264_nvenc)" 或软编 "h264"），供 mode 通知/日志
    QString activeEncoderName() const;

    // WebRTC：浏览器请求关键帧（PLI）时强制下一帧为 IDR
    void requestKeyframe();
    // WebRTC：浏览器码率反馈（REMB）时调整编码码率
    void setBitrate(int bitrate);
    // 当前目标码率（编码线程最新应用值）
    int currentBitrate() const;

signals:
    void encodedFrame(const QByteArray& data, bool keyframe, qint64 timestamp);
    void encoderReady();   // 编码器（重）初始化完成：请求捕获端强制推帧（静止桌面首帧兜底）
    void codecConfigChanged(const QByteArray& extradata); // 用于发送 H.264 SPS/PPS、HEVC VPS/SPS/PPS 等
    // 编码过载状态变化（true=持续过载，false=恢复），供上层自适应降质/恢复
    void encoderOverload(bool overloaded);

private slots:
    void encodingLoop();

private:

    QThread encoderThread_;
    QMutex mutex_;
    QWaitCondition condition_;
    QQueue<QImage> frameQueue_;
    static constexpr int kMaxFrameQueueSize = 10; // 队列上限，防止 OOM
    std::atomic<bool> abort_{false};

    AVCodecContext* codecCtx_ = nullptr;
    AVFrame* frame_ = nullptr;
    SwsContext* swsCtx_ = nullptr;
    AVPixelFormat pixFmt_ = AV_PIX_FMT_YUV420P;
    QString hwName_;
    int64_t frameCount_ = 0;
    qint64 startTime_ = 0;
    int fps_ = 30;

    CodecType currentCodec_ = CodecType::H264;
    HwEncodeMode hwMode_ = HwEncodeMode::Auto;
    QString codecName_;
    std::atomic<bool> forceKeyframe_{ false };
    std::atomic<int> pendingBitrate_{ 0 };
    // 编码线程已应用的码率，供主线程无锁读取（避免直读 codecCtx_->bit_rate 造成数据竞争）
    std::atomic<int> appliedBitrate_{ 0 };
    // RK3588 MPP 硬件编码器（USE_MPP 时启用；MPP 路径不经过 FFmpeg sws/编码器）
    MppEncoder* mpp_ = nullptr;

    // 编码耗时 EMA 监控（检测过载）
    double encodeEmaMs_ = 0;
    qint64 lastOverloadLogMs_ = 0;
    bool overloaded_ = false;
    int mppFailCount_ = 0;   // MPP 编码连续失败计数（超出则回退软编）
};

#endif // VIDEOENCODER_H
