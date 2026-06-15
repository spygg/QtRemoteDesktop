#include "videoencoder.h"
#include <QDateTime>
#include <QDebug>

VideoEncoder::VideoEncoder(QObject*)
    : QObject(nullptr)
{
    moveToThread(&encoderThread_);
    connect(&encoderThread_, &QThread::started, this, &VideoEncoder::encodingLoop);
    // connect(&encoderThread_, &QThread::finished, this, &QObject::deleteLater);
}

VideoEncoder::~VideoEncoder()
{
    shutdown();
}

bool VideoEncoder::initialize(CodecType type, int width, int height, int fps, int bitrate)
{
    currentCodec_ = type;
    fps_ = fps;

    // 根据类型选择编码器 ID
    AVCodecID codecId;
    switch (type) {
    case CodecType::H264:
        codecId = AV_CODEC_ID_H264;
        codecName_ = "H.264";
        break;
    case CodecType::VP8:
        codecId = AV_CODEC_ID_VP8;
        codecName_ = "VP8";
        break;
    case CodecType::VP9:
        codecId = AV_CODEC_ID_VP9;
        codecName_ = "VP9";
        break;
    case CodecType::AV1:
        codecId = AV_CODEC_ID_AV1;
        codecName_ = "AV1";
        break;
    case CodecType::MPEG4:
        codecId = AV_CODEC_ID_MPEG4;
        codecName_ = "MPEG4";
        break;
    case CodecType::MJPEG:
        codecId = AV_CODEC_ID_MJPEG;
        codecName_ = "MJPEG";
        break;
    default:
        qCritical() << "Unsupported codec type";
        return false;
    }

    const AVCodec* codec = avcodec_find_encoder(codecId);
    if (!codec) {
        qCritical() << "Encoder" << codecName_ << "not found!";
        return false;
    }

    codecCtx_ = avcodec_alloc_context3(codec);
    codecCtx_->width = width;
    codecCtx_->height = height;
    codecCtx_->time_base = { 1, fps };
    codecCtx_->framerate = { fps, 1 };
    codecCtx_->bit_rate = bitrate;
    codecCtx_->gop_size = fps; // 1秒一个关键帧
    codecCtx_->max_b_frames = 0; // 无B帧，低延迟
    codecCtx_->pix_fmt = AV_PIX_FMT_YUV420P;
    codecCtx_->thread_count = 2;

    AVDictionary* opts = nullptr;

    // 编码器特定参数
    switch (type) {
    case CodecType::H264:
        av_dict_set(&opts, "preset", "ultrafast", 0);
        av_dict_set(&opts, "tune", "zerolatency", 0);
        av_dict_set(&opts, "profile", "baseline", 0);
        av_dict_set(&opts, "level", "31", 0);
        break;
    case CodecType::VP8:
        av_dict_set(&opts, "deadline", "realtime", 0);
        av_dict_set(&opts, "error_resilient", "1", 0);
        break;
    case CodecType::VP9:
        av_dict_set(&opts, "deadline", "realtime", 0);
        av_dict_set(&opts, "cpu-used", "5", 0); // 速度与质量权衡
        break;
    case CodecType::AV1:
        // AV1 实时编码较慢，此处简化，可选用 libaom-av1 并设置 speed
        av_dict_set(&opts, "usage", "realtime", 0);
        av_dict_set(&opts, "cpu-used", "6", 0);
        break;
    case CodecType::MPEG4:
        av_dict_set(&opts, "qmin", "2", 0);
        av_dict_set(&opts, "qmax", "31", 0);
        av_dict_set(&opts, "mbd", "rd", 0);
        av_dict_set(&opts, "mpeg_quant", "1", 0);
        break;
    case CodecType::MJPEG:
        av_dict_set(&opts, "q", "5", 0); // 质量 1-31，越小越好
        break;
    }

    int ret = avcodec_open2(codecCtx_, codec, &opts);
    av_dict_free(&opts);
    if (ret < 0) {
        qCritical() << "Failed to open codec" << codecName_;
        return false;
    }

    // 如果编码器有 extradata（如 H.264 的 SPS/PPS），发送给前端
    if (codecCtx_->extradata_size > 0) {
        QByteArray extra(reinterpret_cast<char*>(codecCtx_->extradata), codecCtx_->extradata_size);
        emit codecConfigChanged(extra);
    }

    // 分配帧
    frame_ = av_frame_alloc();
    frame_->format = codecCtx_->pix_fmt;
    frame_->width = codecCtx_->width;
    frame_->height = codecCtx_->height;
    av_frame_get_buffer(frame_, 0);

    // 初始化图像转换上下文 (RGB24 -> YUV420P)
    swsCtx_ = sws_getContext(width, height, AV_PIX_FMT_RGB24,
        width, height, AV_PIX_FMT_YUV420P,
        SWS_BILINEAR, nullptr, nullptr, nullptr);

    startTime_ = QDateTime::currentMSecsSinceEpoch();
    encoderThread_.start(QThread::HighPriority);
    qInfo() << codecName_ << "encoder initialized successfully";
    return true;
}

void VideoEncoder::encode(const QImage& frame)
{
    QMutexLocker locker(&mutex_);
    // 队列满时丢弃最旧帧，防止内存无限增长
    if (frameQueue_.size() >= kMaxFrameQueueSize) {
        frameQueue_.dequeue();
    }
    frameQueue_.enqueue(frame.copy()); // 深拷贝
    condition_.wakeOne();
}

void VideoEncoder::encodingLoop()
{
    while (!abort_) {
        QImage image;
        {
            QMutexLocker locker(&mutex_);
            while (frameQueue_.isEmpty() && !abort_) {
                condition_.wait(&mutex_);
            }
            if (abort_)
                break;
            image = frameQueue_.dequeue();
        }

        // 转换 RGB -> YUV
        const uint8_t* srcData[1] = { image.bits() };
        int srcLinesize[1] = { static_cast<int>(image.bytesPerLine()) };
        sws_scale(swsCtx_, srcData, srcLinesize, 0, image.height(),
            frame_->data, frame_->linesize);

        frame_->pts = frameCount_++;

        // 发送帧到编码器
        int ret = avcodec_send_frame(codecCtx_, frame_);
        if (ret < 0)
            continue;

        // 接收编码后的包
        AVPacket* packet = av_packet_alloc();
        while (ret >= 0) {
            ret = avcodec_receive_packet(codecCtx_, packet);
            if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF)
                break;
            if (ret < 0)
                break;

            bool isKeyframe = (packet->flags & AV_PKT_FLAG_KEY) != 0;
            QByteArray data(reinterpret_cast<char*>(packet->data), packet->size);
            qint64 timestamp = QDateTime::currentMSecsSinceEpoch() - startTime_;

            emit encodedFrame(data, isKeyframe, timestamp);

            av_packet_unref(packet);
        }
        av_packet_free(&packet);
    }
}
// void VideoEncoder::encodingLoop()
// {
//     while (!abort_) {
//         QImage image;
//         {
//             QMutexLocker locker(&mutex_);
//             while (frameQueue_.isEmpty() && !abort_) {
//                 condition_.wait(&mutex_);
//             }
//             if (abort_)
//                 break;
//             image = frameQueue_.dequeue();
//         }

//         // 转换 RGB -> YUV
//         const uint8_t* srcData[1] = { image.bits() };
//         int srcLinesize[1] = { static_cast<int>(image.bytesPerLine()) };
//         sws_scale(swsCtx_, srcData, srcLinesize, 0, image.height(),
//             frame_->data, frame_->linesize);

//         // 设置时间戳（毫秒）
//         frame_->pts = frameCount_++;

//         // 发送帧到编码器
//         int ret = avcodec_send_frame(codecCtx_, frame_);
//         if (ret < 0)
//             continue;

//         // 接收编码后的包
//         AVPacket* packet = av_packet_alloc();
//         while (ret >= 0) {
//             ret = avcodec_receive_packet(codecCtx_, packet);
//             if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF)
//                 break;
//             if (ret < 0)
//                 break;

//             bool isKeyframe = (packet->flags & AV_PKT_FLAG_KEY) != 0;
//             QByteArray data(reinterpret_cast<char*>(packet->data), packet->size);
//             qint64 timestamp = QDateTime::currentMSecsSinceEpoch() - startTime_;

//             emit encodedFrame(data, isKeyframe, timestamp);

//             av_packet_unref(packet);
//         }
//         av_packet_free(&packet);
//     }
// }

void VideoEncoder::shutdown()
{
    {
        QMutexLocker locker(&mutex_);
        abort_ = true;
        condition_.wakeAll();
    }

    if (encoderThread_.isRunning()) {
        encoderThread_.quit();
        // 等待线程结束，设置超时防止死锁（例如 3 秒）
        if (!encoderThread_.wait(3000)) {
            qWarning() << "Encoder thread did not stop within 3 seconds, terminating...";
            encoderThread_.terminate(); // 不推荐，但作为最后手段
            encoderThread_.wait(); // 等待终止完成
        }
    }

    // 释放 FFmpeg 资源
    if (swsCtx_) {
        sws_freeContext(swsCtx_);
        swsCtx_ = nullptr;
    }
    if (frame_) {
        av_frame_free(&frame_);
        frame_ = nullptr;
    }
    if (codecCtx_) {
        avcodec_free_context(&codecCtx_);
        codecCtx_ = nullptr;
    }
}
