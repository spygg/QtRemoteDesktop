#include "videoencoder.h"
#include <QDateTime>
#include <QDebug>

VideoEncoder::VideoEncoder(QObject*)
    : QObject(nullptr)
{
    moveToThread(&encoderThread_);
    connect(&encoderThread_, &QThread::started, this, &VideoEncoder::encodingLoop);
}

VideoEncoder::~VideoEncoder()
{
    shutdown();
}

static const char* findHwEncoder()
{
    const char* candidates[] = {
#ifdef Q_OS_WIN
        "h264_nvenc", "h264_amf",
#elif defined(Q_OS_LINUX)
        "h264_nvenc",
#elif defined(Q_OS_MACOS)
        "h264_videotoolbox",
#endif
        nullptr
    };
    for (int i = 0; candidates[i]; ++i) {
        const AVCodec* c = avcodec_find_encoder_by_name(candidates[i]);
        if (!c) continue;
        for (const AVPixelFormat* p = c->pix_fmts; p && *p != AV_PIX_FMT_NONE; ++p) {
            if (*p == AV_PIX_FMT_NV12 || *p == AV_PIX_FMT_YUV420P) {
                return candidates[i];
            }
        }
    }
    return nullptr;
}

static AVPixelFormat encoderPixFmt(const AVCodec* codec, const char* hwName)
{
    if (hwName) {
        for (const AVPixelFormat* p = codec->pix_fmts; p && *p != AV_PIX_FMT_NONE; ++p) {
            if (*p == AV_PIX_FMT_NV12) return AV_PIX_FMT_NV12;
            if (*p == AV_PIX_FMT_YUV420P) return AV_PIX_FMT_YUV420P;
        }
    }
    return AV_PIX_FMT_YUV420P;
}

bool VideoEncoder::initialize(CodecType type, int width, int height, int fps, int bitrate)
{
    currentCodec_ = type;
    fps_ = fps;

    AVCodecID codecId;
    switch (type) {
    case CodecType::H264: codecId = AV_CODEC_ID_H264; break;
    case CodecType::VP8:  codecId = AV_CODEC_ID_VP8;  break;
    case CodecType::VP9:  codecId = AV_CODEC_ID_VP9;  break;
    case CodecType::AV1:  codecId = AV_CODEC_ID_AV1;  break;
    case CodecType::MPEG4:codecId = AV_CODEC_ID_MPEG4;break;
    case CodecType::MJPEG:codecId = AV_CODEC_ID_MJPEG;break;
    default:
        qCritical() << "Unsupported codec type";
        return false;
    }

    const AVCodec* codec = nullptr;
    pixFmt_ = AV_PIX_FMT_YUV420P;
    codecName_ = avcodec_get_name(codecId);

    if (type == CodecType::H264) {
        const char* hwName = findHwEncoder();
        if (hwName) {
            codec = avcodec_find_encoder_by_name(hwName);
            if (codec) {
                pixFmt_ = encoderPixFmt(codec, hwName);
                hwName_ = QString::fromUtf8(hwName);
                codecName_ = QString("H.264 (%1)").arg(hwName_);
                qInfo() << "Using HW encoder:" << hwName_ << "pix_fmt:" << av_get_pix_fmt_name(pixFmt_);
            }
        }
    }

    if (!codec) {
        codec = avcodec_find_encoder(codecId);
        if (!codec) {
            qCritical() << "Encoder" << codecName_ << "not found";
            return false;
        }
        qInfo() << "Using software encoder:" << codecName_;
    }

    codecCtx_ = avcodec_alloc_context3(codec);
    codecCtx_->width = width;
    codecCtx_->height = height;
    codecCtx_->time_base = { 1, fps };
    codecCtx_->framerate = { fps, 1 };
    codecCtx_->bit_rate = bitrate;
    codecCtx_->gop_size = fps;
    codecCtx_->max_b_frames = 0;
    codecCtx_->pix_fmt = pixFmt_;
    codecCtx_->thread_count = 2;

    AVDictionary* opts = nullptr;

    switch (type) {
    case CodecType::H264:
        if (hwName_.isEmpty()) {
            av_dict_set(&opts, "preset", "ultrafast", 0);
            av_dict_set(&opts, "tune", "zerolatency", 0);
        } else {
            av_dict_set(&opts, "preset", "p1", 0);
            av_dict_set(&opts, "tune", "ll", 0);
        }
        av_dict_set(&opts, "profile", "baseline", 0);
        break;
    case CodecType::VP8:
        av_dict_set(&opts, "deadline", "realtime", 0);
        av_dict_set(&opts, "error_resilient", "1", 0);
        break;
    case CodecType::VP9:
        av_dict_set(&opts, "deadline", "realtime", 0);
        av_dict_set(&opts, "cpu-used", "5", 0);
        break;
    case CodecType::AV1:
        av_dict_set(&opts, "usage", "realtime", 0);
        av_dict_set(&opts, "cpu-used", "6", 0);
        break;
    case CodecType::MPEG4:
        av_dict_set(&opts, "qmin", "2", 0);
        av_dict_set(&opts, "qmax", "31", 0);
        break;
    case CodecType::MJPEG:
        av_dict_set(&opts, "q", "5", 0);
        break;
    }

    int ret = avcodec_open2(codecCtx_, codec, &opts);
    av_dict_free(&opts);
    if (ret < 0) {
        qCritical() << "Failed to open codec" << codecName_ << "error:" << ret;
        avcodec_free_context(&codecCtx_);
        codecCtx_ = nullptr;
        return false;
    }

    if (codecCtx_->extradata && codecCtx_->extradata_size > 0) {
        QByteArray extra(reinterpret_cast<char*>(codecCtx_->extradata), codecCtx_->extradata_size);
        emit codecConfigChanged(extra);
    }

    frame_ = av_frame_alloc();
    frame_->format = codecCtx_->pix_fmt;
    frame_->width = codecCtx_->width;
    frame_->height = codecCtx_->height;
    av_frame_get_buffer(frame_, 0);

    swsCtx_ = sws_getContext(width, height, AV_PIX_FMT_RGB24,
        width, height, codecCtx_->pix_fmt,
        SWS_BILINEAR, nullptr, nullptr, nullptr);
    if (!swsCtx_) {
        qCritical() << "sws_getContext failed";
        av_frame_free(&frame_);
        avcodec_free_context(&codecCtx_);
        codecCtx_ = nullptr;
        return false;
    }

    startTime_ = QDateTime::currentMSecsSinceEpoch();
    encoderThread_.start(QThread::HighPriority);
    qInfo() << codecName_ << "encoder initialized" << width << "x" << height << "@" << fps << "fps";
    return true;
}

void VideoEncoder::encode(const QImage& frame)
{
    QMutexLocker locker(&mutex_);
    if (frameQueue_.size() >= kMaxFrameQueueSize)
        frameQueue_.dequeue();
    frameQueue_.enqueue(frame);
    condition_.wakeOne();
}

void VideoEncoder::encodingLoop()
{
    while (!abort_) {
        QImage image;
        {
            QMutexLocker locker(&mutex_);
            while (frameQueue_.isEmpty() && !abort_)
                condition_.wait(&mutex_);
            if (abort_)
                break;
            image = frameQueue_.dequeue();
        }

        const uint8_t* srcData[1] = { image.bits() };
        int srcLinesize[1] = { static_cast<int>(image.bytesPerLine()) };
        sws_scale(swsCtx_, srcData, srcLinesize, 0, image.height(),
            frame_->data, frame_->linesize);

        frame_->pts = frameCount_++;

        int ret = avcodec_send_frame(codecCtx_, frame_);
        if (ret < 0)
            continue;

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

void VideoEncoder::shutdown()
{
    {
        QMutexLocker locker(&mutex_);
        abort_ = true;
        condition_.wakeAll();
    }

    if (encoderThread_.isRunning()) {
        encoderThread_.quit();
        if (!encoderThread_.wait(3000)) {
            qWarning() << "Encoder thread did not stop within 3s, terminating...";
            encoderThread_.terminate();
            encoderThread_.wait();
        }
    }

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
    if (hwDeviceCtx_) {
        av_buffer_unref(&hwDeviceCtx_);
        hwDeviceCtx_ = nullptr;
    }
}
