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

bool VideoEncoder::isHwAcceleratedAvailable()
{
    return findHwEncoder() != nullptr;
}

bool VideoEncoder::initialize(CodecType type, int srcW, int srcH, int encW, int encH, int fps, int bitrate)
{
    // 支持 shutdown() 后重新初始化（缩放档位改变时按新尺寸重建编码器）
    abort_ = false;
    frameCount_ = 0;
    startTime_ = 0;
    pendingBitrate_.store(0);
    forceKeyframe_.store(false);
    encodeEmaMs_ = 0;
    lastOverloadLogMs_ = 0;
    overloaded_ = false;
    {
        QMutexLocker locker(&mutex_);
        frameQueue_.clear();
    }

#if LIBAVCODEC_VERSION_INT < AV_VERSION_INT(58, 0, 0)
    avcodec_register_all();
#endif
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
    codecCtx_->width = encW;
    codecCtx_->height = encH;
    codecCtx_->time_base = { 1, fps };
    codecCtx_->framerate = { fps, 1 };
    codecCtx_->bit_rate = bitrate;
    codecCtx_->gop_size = fps;
    codecCtx_->max_b_frames = 0;
    codecCtx_->pix_fmt = pixFmt_;
    codecCtx_->thread_count = qMax(1, qMin(QThread::idealThreadCount(), 4));
    appliedBitrate_.store(bitrate);

    AVDictionary* opts = nullptr;

    switch (type) {
    case CodecType::H264:
        if (hwName_.isEmpty() && codec->name &&
            (qstrcmp(codec->name, "libopenh264") == 0 || qstrcmp(codec->name, "h264_openh264") == 0)) {
            // libopenh264（FFmpeg 3.4.8 封装）：减少实时桌面的 CPU 占用
            av_dict_set(&opts, "allow_skip_frames", "1", 0); // 码率超限时允许跳帧，避免积压
            av_dict_set(&opts, "loopfilter", "0", 0);        // 禁用环内滤波，省 CPU（桌面画面可接受）
        } else if (hwName_.isEmpty()) {
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
    if (av_frame_get_buffer(frame_, 0) < 0) {
        qCritical() << "Failed to allocate frame buffer";
        av_frame_free(&frame_);
        avcodec_free_context(&codecCtx_);
        codecCtx_ = nullptr;
        return false;
    }

    // 缩放移入编码线程：源为完整捕获帧，目标为编码分辨率（按缩放档位）
    // AV_PIX_FMT_RGB32 在小端系统上即 BGRA，与 QImage::Format_RGB32 内存布局一致
    swsCtx_ = sws_getContext(srcW, srcH, AV_PIX_FMT_RGB32,
        encW, encH, codecCtx_->pix_fmt,
        SWS_FAST_BILINEAR, nullptr, nullptr, nullptr);
    if (!swsCtx_) {
        qCritical() << "sws_getContext failed";
        av_frame_free(&frame_);
        avcodec_free_context(&codecCtx_);
        codecCtx_ = nullptr;
        return false;
    }

    startTime_ = QDateTime::currentMSecsSinceEpoch();
    encoderThread_.start(QThread::HighPriority);
    qInfo() << codecName_ << "encoder initialized" << encW << "x" << encH
            << "@" << fps << "fps (sws " << srcW << "x" << srcH << " -> " << encW << "x" << encH << ")";
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

    // 统一输入到 sws：X11 捕获直接给 RGB32（小端=BGRA），其他平台给 RGB888。
    // 转换放到编码线程（原本空闲），把主线程从全帧 convertToFormat 中解放。
    if (image.format() != QImage::Format_RGB32 && image.format() != QImage::Format_ARGB32) {
        image = image.convertToFormat(QImage::Format_RGB32);
    }

    const uint8_t* srcData[1] = { image.bits() };
    int srcLinesize[1] = { static_cast<int>(image.bytesPerLine()) };
    sws_scale(swsCtx_, srcData, srcLinesize, 0, image.height(),
        frame_->data, frame_->linesize);

        // 应用 WebRTC 反馈：REMB 码率调整 / PLI 强制关键帧
        int br = pendingBitrate_.exchange(0);
        if (br > 0 && codecCtx_->bit_rate != br) {
            codecCtx_->bit_rate = br;
            appliedBitrate_.store(br);
        }
        if (forceKeyframe_.exchange(false))
            codecCtx_->gop_size = 1; // 下一帧强制为 IDR

        frame_->pts = frameCount_++;

        // 编码耗时监控（EMA）：
        // 平均耗时超过帧间隔的 80% 视为过载，每 5 秒最多告警一次，便于定位单板 CPU 压力
        qint64 encodeStart = QDateTime::currentMSecsSinceEpoch();
        int ret = avcodec_send_frame(codecCtx_, frame_);
        if (ret < 0) {
            // 失败路径也必须恢复 GOP：否则强制关键帧后 gop_size 一直为 1，
            // 导致后续每一帧都被强制为 IDR（码率暴增、CPU 过载）
            if (codecCtx_->gop_size == 1)
                codecCtx_->gop_size = fps_;
            continue;
        }

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

        qint64 encodeMs = QDateTime::currentMSecsSinceEpoch() - encodeStart;
        encodeEmaMs_ = (encodeEmaMs_ == 0) ? encodeMs : (encodeEmaMs_ * 0.8 + encodeMs * 0.2);
        double frameIntervalMs = fps_ > 0 ? 1000.0 / fps_ : 33.0;
        // 冷启动前几帧含编码器预热（首帧必然慢），跳过过载判定避免启动时误降码率
        if (frameCount_ <= 5) {
            if (codecCtx_->gop_size == 1)
                codecCtx_->gop_size = fps_;
            continue;
        }
        bool overloadedNow = encodeEmaMs_ > frameIntervalMs * 0.8;
        // 过载状态变化时通知上层（带滞回：<50% 才算恢复，避免频繁抖动）
        if (overloadedNow && !overloaded_) {
            overloaded_ = true;
            emit encoderOverload(true);
        } else if (!overloadedNow && overloaded_ && encodeEmaMs_ < frameIntervalMs * 0.5) {
            overloaded_ = false;
            emit encoderOverload(false);
        }
        if (overloadedNow) {
            qint64 nowMs = QDateTime::currentMSecsSinceEpoch();
            if (nowMs - lastOverloadLogMs_ > 5000) {
                lastOverloadLogMs_ = nowMs;
                qWarning() << "VideoEncoder: overloaded, encode EMA"
                           << QString::number(encodeEmaMs_, 'f', 1) << "ms / frame interval"
                           << QString::number(frameIntervalMs, 'f', 1) << "ms";
            }
        }

        // 恢复正常 GOP 间隔
        if (codecCtx_->gop_size == 1)
            codecCtx_->gop_size = fps_;
    }
}

void VideoEncoder::requestKeyframe()
{
    forceKeyframe_.store(true);
}

void VideoEncoder::setBitrate(int bitrate)
{
    if (bitrate > 0)
        pendingBitrate_.store(bitrate);
}

int VideoEncoder::currentBitrate() const
{
    return appliedBitrate_.load();
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
}
