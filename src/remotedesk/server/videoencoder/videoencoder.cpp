#include "videoencoder.h"
#include "mppencoder.h"
#include <QDateTime>
#include <cstring>
#include <QDebug>
#include <QFile>
#ifdef Q_OS_LINUX
#include <QDir>
#endif

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

// 硬件编码器平台可用性粗筛（编码器存在 != 运行时可用；避免假阳性导致每次初始化
// 都尝试 open 一个必然失败的编码器，从而产生无谓的延迟与告警日志）
static bool hwPlatformAvailable(const char* encName)
{
#ifdef Q_OS_LINUX
    if (strstr(encName, "vaapi")) {
        // VAAPI 需要 DRM 渲染节点；无 /dev/dri 的板子直接跳过
        QDir dri("/dev/dri");
        if (!dri.exists())
            return false;
        if (dri.entryList(QStringList() << "renderD*", QDir::System).isEmpty())
            return false;
    } else if (strstr(encName, "nvenc")) {
        // NVIDIA NVENC 需要驱动设备节点；无 NVIDIA 的机器（如 RK3588）直接跳过
        if (!QFile::exists("/dev/nvidiactl"))
            return false;
    }
    // rkmpp：FFmpeg 编译带 rkmpp 即有编码器，可用性由 avcodec_open2 验证并回退
#endif
    return true;
}

// 按编码类型 + 平台返回硬件编码器候选链（优先板载/通用方案，最后才是厂商专有）
static const char* findHwEncoder(CodecType type, HwEncodeMode mode)
{
    if (mode == HwEncodeMode::Off)
        return nullptr;
    const char* candidates[4] = { nullptr, nullptr, nullptr, nullptr };
    switch (type) {
    case CodecType::H264:
#ifdef Q_OS_WIN
        candidates[0] = "h264_nvenc"; candidates[1] = "h264_amf";
#elif defined(Q_OS_LINUX)
        candidates[0] = "h264_rkmpp"; candidates[1] = "h264_vaapi"; candidates[2] = "h264_nvenc";
#elif defined(Q_OS_MACOS)
        candidates[0] = "h264_videotoolbox";
#endif
        break;
    case CodecType::HEVC:
#ifdef Q_OS_WIN
        candidates[0] = "hevc_nvenc"; candidates[1] = "hevc_amf";
#elif defined(Q_OS_LINUX)
        candidates[0] = "hevc_rkmpp"; candidates[1] = "hevc_vaapi"; candidates[2] = "hevc_nvenc";
#elif defined(Q_OS_MACOS)
        candidates[0] = "hevc_videotoolbox";
#endif
        break;
    case CodecType::VP8:
#ifdef Q_OS_LINUX
        candidates[0] = "vp8_vaapi";
#endif
        break;
    case CodecType::VP9:
#ifdef Q_OS_LINUX
        candidates[0] = "vp9_vaapi";
#endif
        break;
    case CodecType::AV1:
#ifdef Q_OS_WIN
        candidates[0] = "av1_nvenc"; candidates[1] = "av1_amf";
#elif defined(Q_OS_LINUX)
        candidates[0] = "av1_vaapi"; candidates[1] = "av1_nvenc";
#endif
        break;
    default:
        break; // MPEG4/MJPEG 无硬件编码
    }
    for (int i = 0; i < 4 && candidates[i]; ++i) {
        const AVCodec* c = avcodec_find_encoder_by_name(candidates[i]);
        if (!c)
            continue;
        if (!hwPlatformAvailable(candidates[i]))
            continue;
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

// 硬件编码器私有选项：按编码器名精确匹配，避免给不认识的编码器传不存在的选项
// 导致 avcodec_open2 失败（如 h264_amf 无 preset/tune、rkmpp/vaapi/videotoolbox 无 preset）
static void applyHwOpts(const QString& hwName, AVDictionary** opts)
{
    if (hwName.contains("nvenc")) {
        av_dict_set(opts, "preset", "p1", 0);
        av_dict_set(opts, "tune", "ll", 0);
    } else if (hwName.contains("amf")) {
        // h264_amf/hevc_amf/av1_amf：无 preset/tune；usage=transcoding 走低延迟实时档
        av_dict_set(opts, "usage", "transcoding", 0);
    }
    // videotoolbox / vaapi / rkmpp：默认参数即可，不额外设置
}

bool VideoEncoder::isHwAcceleratedAvailable(CodecType type)
{
    // RK3588 MPP：H264/HEVC 直连硬编（非 avcodec 编码器，单独探测）
    if ((type == CodecType::H264 || type == CodecType::HEVC) && MppEncoder::isSupported())
        return true;
    return findHwEncoder(type, HwEncodeMode::Auto) != nullptr;
}

QString VideoEncoder::hwEncoderName(CodecType type)
{
    if ((type == CodecType::H264 || type == CodecType::HEVC) && MppEncoder::isSupported())
        return type == CodecType::HEVC ? QStringLiteral("hevc_rkmpp") : QStringLiteral("h264_rkmpp");
    const char* n = findHwEncoder(type, HwEncodeMode::Auto);
    return n ? QString::fromUtf8(n) : QString();
}

QString VideoEncoder::activeEncoderName() const
{
    return codecName_;
}

bool VideoEncoder::initialize(CodecType type, int srcW, int srcH, int encW, int encH, int fps, int bitrate,
                              HwEncodeMode hwMode)
{
    // 支持 shutdown() 后重新初始化（缩放/帧率/画质/编码协议/硬件开关改变时重建编码器）
    // 先释放旧 MPP 实例：hwMode=Off 回退软编时必须清掉残留的 MPP（否则 encodingLoop
    // 仍会走 isActive() 的 MPP 路径，软编永不生效）
    if (mpp_) {
        delete mpp_;
        mpp_ = nullptr;
    }
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
    hwMode_ = hwMode;

    // ---- RK3588 MPP 硬编：H264/HEVC + 硬件开关非 Off 时优先走 MPP 直连 ----
    // MPP 不是 avcodec 编码器（无编码器名可探测），需单独初始化；其他平台 isSupported()=false 自动跳过
    if ((type == CodecType::H264 || type == CodecType::HEVC) && hwMode != HwEncodeMode::Off) {
        mpp_ = new MppEncoder();
        if (mpp_->initialize(static_cast<int>(type), encW, encH, fps, bitrate,
                             hwMode == HwEncodeMode::On)) {
            codecName_ = mpp_->name();
            hwName_ = mpp_->hwName();
            qInfo() << "Using MPP encoder:" << hwName_ << encW << "x" << encH << "@" << fps << "fps";
            startTime_ = QDateTime::currentMSecsSinceEpoch();
            encoderThread_.start(QThread::HighPriority);
            emit encoderReady();
            return true;
        }
        delete mpp_;
        mpp_ = nullptr;
        if (hwMode == HwEncodeMode::On) {
            // 强制硬编但 MPP 不可用：仍回退 FFmpeg 软编保证画面可用
            // （本设备 MPP 1.1.0 初始化成功但 encode 恒 null，强制 on 若直接失败
            //  会让 video 模式整段停用导致黑屏，回退软编更稳妥）
            qWarning() << "MPP encoder unavailable, hw_encode=on falling back to software encoder";
        } else {
            qWarning() << "MPP encoder init failed, falling back to FFmpeg encoder";
        }
    }

    AVCodecID codecId;
    switch (type) {
    case CodecType::H264: codecId = AV_CODEC_ID_H264; break;
    case CodecType::HEVC: codecId = AV_CODEC_ID_HEVC; break;
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
    codecName_ = QString::fromUtf8(avcodec_get_name(codecId));

    // 支持硬件编码的编码类型才探测（MPEG4/MJPEG 直接软编）
    const char* hwName = nullptr;
    switch (type) {
    case CodecType::H264: case CodecType::HEVC:
    case CodecType::VP8:  case CodecType::VP9: case CodecType::AV1:
        hwName = findHwEncoder(type, hwMode);
        break;
    default:
        break;
    }
    if (hwName) {
        codec = avcodec_find_encoder_by_name(hwName);
        if (codec) {
            pixFmt_ = encoderPixFmt(codec, hwName);
            hwName_ = QString::fromUtf8(hwName);
            codecName_ = QString("%1 (%2)").arg(QString::fromUtf8(avcodec_get_name(codecId))).arg(hwName_);
            qInfo() << "Using HW encoder:" << hwName_ << "pix_fmt:" << av_get_pix_fmt_name(pixFmt_);
        }
    }

    if (!codec) {
        codec = avcodec_find_encoder(codecId);
        if (!codec) {
            qCritical() << "Encoder" << codecName_ << "not found";
            return false;
        }
        hwName_.clear();
        qInfo() << "Using software encoder:" << codecName_;
    }

    // openCodec：分配 ctx + 设置参数 + open；失败返回 false（由调用方决定回退或放弃）
    auto openCodec = [&](const AVCodec* c) -> bool {
        AVCodecContext* ctx = avcodec_alloc_context3(c);
        if (!ctx)
            return false;
        ctx->width = encW;
        ctx->height = encH;
        ctx->time_base = { 1, fps };
        ctx->framerate = { fps, 1 };
        ctx->bit_rate = bitrate;
        ctx->gop_size = fps;
        ctx->max_b_frames = 0;
        ctx->pix_fmt = pixFmt_;
        // 硬件编码器内部自带加速与缓冲管理，线程数固定 1；多线程仅对软编有效
        ctx->thread_count = hwName_.isEmpty() ? qMax(1, qMin(QThread::idealThreadCount(), 4)) : 1;

        AVDictionary* opts = nullptr;

        switch (type) {
        case CodecType::H264:
            if (!hwName_.isEmpty()) {
                applyHwOpts(hwName_, &opts);
            } else if (c->name &&
                (qstrcmp(c->name, "libopenh264") == 0 || qstrcmp(c->name, "h264_openh264") == 0)) {
                // libopenh264（FFmpeg 3.4.8 封装）：减少实时桌面的 CPU 占用
                av_dict_set(&opts, "allow_skip_frames", "1", 0); // 码率超限时允许跳帧，避免积压
                av_dict_set(&opts, "loopfilter", "0", 0);        // 禁用环内滤波，省 CPU（桌面画面可接受）
            } else {
                av_dict_set(&opts, "preset", "ultrafast", 0);
                av_dict_set(&opts, "tune", "zerolatency", 0);
            }
            av_dict_set(&opts, "profile", "baseline", 0);
            break;
        case CodecType::HEVC:
            if (!hwName_.isEmpty()) {
                applyHwOpts(hwName_, &opts);
            } else {
                // libx265 等软编：ultrafast + zerolatency 保证实时桌面延迟
                av_dict_set(&opts, "preset", "ultrafast", 0);
                av_dict_set(&opts, "tune", "zerolatency", 0);
            }
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

        int ret = avcodec_open2(ctx, c, &opts);
        av_dict_free(&opts);
        if (ret < 0) {
            avcodec_free_context(&ctx);
            qWarning() << "Failed to open encoder" << codecName_ << "error:" << ret;
            return false;
        }
        codecCtx_ = ctx;
        return true;
    };

    if (!openCodec(codec)) {
        if (!hwName_.isEmpty()) {
            // 探测到硬件编码器但 open 失败（无驱动/设备/选项不兼容）→ 回退软编，保证画面不中断
            qWarning() << "HW encoder open failed, falling back to software encoder";
            codec = avcodec_find_encoder(codecId);
            if (!codec) {
                qCritical() << "Software encoder" << codecName_ << "not found";
                return false;
            }
            hwName_.clear();
            codecName_ = QString::fromUtf8(avcodec_get_name(codecId));
            pixFmt_ = AV_PIX_FMT_YUV420P;
            qInfo() << "Using software encoder:" << codecName_;
            if (!openCodec(codec))
                return false;
        } else {
            return false;
        }
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
    emit encoderReady();
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

    // ---- MPP 硬编路径（RK3588）：不经过 FFmpeg sws/编码器，MPP 内部自带 RGB32->NV12 ----
    if (mpp_ && mpp_->isActive()) {
        int br = pendingBitrate_.exchange(0);
        if (br > 0) {
            mpp_->setBitrate(br);
            appliedBitrate_.store(br);
        }
        if (forceKeyframe_.exchange(false))
            mpp_->requestKeyframe();

        qint64 encodeStart = QDateTime::currentMSecsSinceEpoch();
        QByteArray out;
        bool key = false;
        if (!mpp_->encode(image, out, key)) {
            // 连续失败阈值后认为 MPP 编码器实际不可用（如设备固件/库版本不兼容，
            // 初始化成功但 encode 始终 null），自动重建为 FFmpeg 软编，避免永久黑屏
            if (++mppFailCount_ >= 3) {
                qWarning() << "MPP encoder failing continuously (" << mppFailCount_
                           << "), falling back to software encoder";
                initialize(currentCodec_, image.width(), image.height(),
                           image.width(), image.height(), fps_, (pendingBitrate_.load() > 0 ? pendingBitrate_.load() : 1000000), HwEncodeMode::Off);
                mppFailCount_ = 0;
            }
            continue;
        }
        mppFailCount_ = 0;
        qint64 timestamp = QDateTime::currentMSecsSinceEpoch() - startTime_;
        emit encodedFrame(out, key, timestamp);

        // 过载监控（与 FFmpeg 路径同一套 EMA 逻辑；MPP 编码很快，通常远低于帧间隔）
        qint64 encodeMs = QDateTime::currentMSecsSinceEpoch() - encodeStart;
        encodeEmaMs_ = (encodeEmaMs_ == 0) ? encodeMs : (encodeEmaMs_ * 0.8 + encodeMs * 0.2);
        double frameIntervalMs = fps_ > 0 ? 1000.0 / fps_ : 33.0;
        if (frameCount_ > 5) {
            bool overloadedNow = encodeEmaMs_ > frameIntervalMs * 0.8;
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
        }
        continue;
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

    if (mpp_) {
        mpp_->shutdown();
        delete mpp_;
        mpp_ = nullptr;
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
