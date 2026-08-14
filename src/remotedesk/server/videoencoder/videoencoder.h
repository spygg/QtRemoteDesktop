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
    VP8,
    VP9,
    AV1,
    MPEG4,
    MJPEG
};

class VideoEncoder : public QObject {
    Q_OBJECT
public:
    explicit VideoEncoder(QObject* parent = nullptr);
    ~VideoEncoder();

    bool initialize(CodecType type, int srcW, int srcH, int encW, int encH, int fps, int bitrate);
    void encode(const QImage& frame);
    void shutdown();

    // 探测当前系统是否可用 H.264 硬件编码（用于前端 auto 模式选路）
    static bool isHwAcceleratedAvailable();

    // WebRTC：浏览器请求关键帧（PLI）时强制下一帧为 IDR
    void requestKeyframe();
    // WebRTC：浏览器码率反馈（REMB）时调整编码码率
    void setBitrate(int bitrate);
    // 当前目标码率（编码线程最新应用值）
    int currentBitrate() const;

signals:
    void encodedFrame(const QByteArray& data, bool isKeyframe, qint64 timestamp);
    void codecConfigChanged(const QByteArray& extradata); // 用于发送 H.264 SPS/PPS 等
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
    QString codecName_;
    std::atomic<bool> forceKeyframe_{ false };
    std::atomic<int> pendingBitrate_{ 0 };
    // 编码线程已应用的码率，供主线程无锁读取（避免直读 codecCtx_->bit_rate 造成数据竞争）
    std::atomic<int> appliedBitrate_{ 0 };

    // 编码耗时 EMA 监控（检测过载）
    double encodeEmaMs_ = 0;
    qint64 lastOverloadLogMs_ = 0;
    bool overloaded_ = false;
};

#endif // VIDEOENCODER_H
