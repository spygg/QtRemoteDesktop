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

    bool initialize(CodecType type, int width, int height, int fps, int bitrate);
    void encode(const QImage& frame);
    void shutdown();

signals:
    void encodedFrame(const QByteArray& data, bool isKeyframe, qint64 timestamp);
    void codecConfigChanged(const QByteArray& extradata); // 用于发送 H.264 SPS/PPS 等

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
    AVBufferRef* hwDeviceCtx_ = nullptr;
    QString hwName_;
    int64_t frameCount_ = 0;
    qint64 startTime_ = 0;
    int fps_ = 30;

    CodecType currentCodec_;
    QString codecName_;
};

#endif // VIDEOENCODER_H
