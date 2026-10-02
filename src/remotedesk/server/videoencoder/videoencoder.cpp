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

// 进程级标志：MPP 一旦被证明实际不可用（initialize 成功但 encode 恒返回 null），
// 本进程后续一律不再尝试 MPP。
//
// 原因：RK3588 上 MPP 常出现"初始化/探测成功、实际编码恒 null"的情况
// （实测 mpp_venc_kcfg_init failed -1、encode returned null packet 上百次）。
// 若每次重建编码器都重新走一遍必然失败的 MPP 路径，就会与软编无限交替
// （日志表现为 "Using MPP encoder" / "Using software encoder" 反复切换），
// 每次切换都要 shutdown+initialize（含线程停止/启动），画面长时间中断且 CPU 空转。
static std::atomic<bool> g_mppDisabled{ false };

bool VideoEncoder::isMppDisabled()
{
    return g_mppDisabled.load();
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

// 释放 FFmpeg 编码链路资源（幂等，指针置空）。
// 独立出来的原因：initialize() 会被重复调用（上层 reinit 未必先 shutdown），
// 必须能安全地把上一轮的上下文清干净，否则每次重建都泄漏一套编码器。
static void releaseAv(AVCodecContext** ctx, AVFrame** frame, SwsContext** sws)
{
    if (sws && *sws) {
        sws_freeContext(*sws);
        *sws = nullptr;
    }
    if (frame && *frame) {
        av_frame_free(frame);
        *frame = nullptr;
    }
    if (ctx && *ctx) {
        avcodec_free_context(ctx);
        *ctx = nullptr;
    }
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
    // 释放上一轮 FFmpeg 资源：initialize 允许重复调用（上层 reinit 未必先 shutdown），
    // 若不先释放就直接覆盖指针，会泄漏整套编码器上下文（硬编时还含 GPU/驱动资源）。
    releaseAv(&codecCtx_, &frame_, &swsCtx_);

    abort_ = false;
    hasIdr_.store(false);
    frameCount_ = 0;
    startTime_ = 0;
    pendingBitrate_.store(0);
    appliedBitrate_.store(0);   // [H18] 新编码器按参数码率打开，旧 applied 值残留会让 encode() 误判已生效
    forceKeyframe_.store(false);
    mppBroken_.store(false);
    mppFailCount_ = 0;          // [H18] 上一轮累计的失败计数不清零，新实例再失败 1 次即被永久禁用
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
    // 记录本轮上下文对应的源尺寸：后续投入的帧尺寸必须一致，
    // 否则 sws_scale/MPP 会按旧尺寸解释新帧（越界读 / 画面错乱）。
    swsSrcW_ = srcW;
    swsSrcH_ = srcH;
    sizeMismatch_.store(false);

    // ---- RK3588 MPP 硬编：H264/HEVC + 硬件开关非 Off 时优先走 MPP 直连 ----
    // MPP 不是 avcodec 编码器（无编码器名可探测），需单独初始化；其他平台 isSupported()=false 自动跳过
    if ((type == CodecType::H264 || type == CodecType::HEVC) && hwMode != HwEncodeMode::Off
        && !g_mppDisabled.load()) {
        mpp_ = new MppEncoder();
        // 源尺寸 srcW×srcH 与编码尺寸 encW×encH 分开传：MPP 内部 sws 需要按
        // 源尺寸解释输入帧，只传编码尺寸会导致画面裁切+压扁。
        if (mpp_->initialize(static_cast<int>(type), srcW, srcH, encW, encH, fps, bitrate,
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
        // MPP 初始化失败（如 mpp_venc_kcfg_init 失败）：置全局禁用标志，
        // 本进程后续重建不再尝试。重复 init/free mpp 库在部分固件上会崩
        // （曾见连续重建时 Segmentation fault）。
        g_mppDisabled.store(true);
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
        // 关键帧间隔取半秒到一秒之间：太短浪费码率，太长则画面突变后
        // 要等待整个 GOP 才能恢复清晰。桌面远程建议 ~0.5s 一个 IDR。
        ctx->gop_size = qMax(1, fps / 2);
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
                // libopenh264（FFmpeg 3.4.8 封装）：
                // 注意：openh264 硬约束——禁用跳帧(bEnableFrameSkip=0)会导致
                // RC_QUALITY/BITRATE/TIMESTAMP 模式全部失效（日志明确警告
                // "bitrate can't be controlled without enabling skip frame"），
                // 表现为刷新后画面停滞。必须保持跳帧允许；码率充足(高档~25M)时
                // 编码器实际不会跳帧，画质不受影响，仅低码率时才触发跳帧保流畅。
                av_dict_set(&opts, "allow_skip_frames", "1", 0);
                // 高码率（高质量档）时开启环内滤波消除块状感，否则禁用以省 CPU。
                // 100 万像素约需 0.05 bits/px 落点区分档位，此处按 bitrate 判定更直观。
                if (bitrate >= 15000000)
                    av_dict_set(&opts, "loopfilter", "1", 0);
                else
                    av_dict_set(&opts, "loopfilter", "0", 0);
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
    // 必须深拷贝再入队：X11 全量抓取路径把持久缓冲 fullFrame_ 浅共享给上层，
    // 而 Damage 区域抓取路径会在同一缓冲上 memcpy 原地改写。若这里只存 QImage 引用，
    // 编码线程出队时可能读到被捕获线程并发改写的半新半旧帧 → 画面撕裂/花屏。
    frameQueue_.enqueue(frame.copy());
    condition_.wakeOne();
}

void VideoEncoder::encodingLoop()
{
    // [P2] AVPacket 跨帧复用：旧代码每帧 av_packet_alloc/av_packet_free 各一次
    //（30fps = 每秒 60 次堆操作），packet 生命周期只在 receive 循环内，复用安全。
    AVPacket* packet = av_packet_alloc();
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

    // 尺寸一致性校验：sws 上下文/MPP 都是按 initialize() 时的源尺寸建立的。
    // 分辨率切换、输出切换或显示器热插拔后若上层未重建编码器，sws_scale 会按旧宽度
    // 解释新帧、却用新 bytesPerLine 作行距 → 降分辨率时堆越界读（可致随机崩溃）。
    // 这里直接丢帧并置位，由上层（onFrameCaptured）触发重建。
    if (image.width() != swsSrcW_ || image.height() != swsSrcH_) {
        if (!sizeMismatch_.exchange(true))
            qWarning() << "VideoEncoder: frame size" << image.width() << "x" << image.height()
                       << "!= context" << swsSrcW_ << "x" << swsSrcH_
                       << "- dropping frames until reinit";
        continue;
    }

    // ---- MPP 硬编路径（RK3588）：不经过 FFmpeg sws/编码器，MPP 内部自带 RGB32->NV12 ----
    //
    // 重要：只要 mpp_ 存在，本帧就必须完全走这条路 —— 绝不能落到下面的 FFmpeg 分支。
    // 因为 MPP 初始化成功时根本不会创建 swsCtx_/frame_/codecCtx_（见 initialize 的
    // MPP 成功分支 early return），一旦走到 sws_scale(swsCtx_) 就是空指针崩溃。
    // MPP 失效（mppBroken_）或已 teardown（isActive()==false）时直接跳过本帧，
    // 等待上层重建编码器。
    if (mpp_) {
        if (!mppBroken_.load() && mpp_->isActive()) {
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
            if (mpp_->encode(image, out, key)) {
                mppFailCount_ = 0;
                // [B18] MPP 分支同样要置 hasIdr_：上层 pumpKeyframe 依赖它判定
                // "编码器从未产出过 IDR"。旧代码只在 FFmpeg 分支置位，MPP 路径
                // hasIdr() 永假 → 每 4s 整套重建编码器（RK3588 周期性卡顿，
                // MPP 反复 init/free 有固件崩溃风险）。
                if (key)
                    hasIdr_.store(true);
                qint64 timestamp = QDateTime::currentMSecsSinceEpoch() - startTime_;
                emit encodedFrame(out, key, timestamp);
                updateOverloadState(encodeStart);
                continue;
            }

            // MPP 编码失败：累计到阈值判定本机 MPP 实际不可用。
            // 此前这里直接调用 initialize() 回退软编，是严重错误 —— 本函数运行在
            // encoderThread_ 自身（由 QThread::started 触发），而 initialize() 末尾会
            // encoderThread_.start()，等于让线程重新启动自己（Qt 明确禁止），
            // 且重新入参丢了缩放档位与用户码率（pendingBitrate_ 刚被 exchange(0) 清零，
            // 只能取硬编码兜底值）。正确做法是置标志 + 通知主线程重建。
            if (++mppFailCount_ >= 3) {
                g_mppDisabled.store(true);   // 本进程不再尝试 MPP，避免与软编无限交替
                mppBroken_.store(true);
                mppFailCount_ = 0;
                qWarning() << "MPP encoder failing continuously, disabling it for this process"
                           << "and requesting software reinit";
                emit reinitRequired();
            }
        }
        continue;
    }

    // MPP 之外的路径必须保证 FFmpeg 资源就绪，否则跳过本帧而不是崩溃
    if (!codecCtx_ || !frame_ || !swsCtx_)
        continue;

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
        if (forceKeyframe_.exchange(false)) {
            qWarning() << "VideoEncoder: FORCE-KF -> flush (frameCount=" << frameCount_ << ")";
            // libx264 运行期改 gop_size / 设 pict_type 均不可靠（旧版 ffmpeg 不认 frame->pict_type，
            // 实测 frame=2/3/4 强制后仍输出 P 帧），用 avcodec_flush_buffers 重置编码器内部
            // GOP 帧计数器，下一帧必为 IDR（含 SPS/PPS，MSE 可衔接解码）。
            avcodec_flush_buffers(codecCtx_);
            codecCtx_->gop_size = 1; // 双保险：部分编码器运行时读取
            frame_->pict_type = AV_PICTURE_TYPE_I; // 双保险：新版 ffmpeg 认
        } else {
            frame_->pict_type = AV_PICTURE_TYPE_NONE; // 防残留：非强制帧不继承上一帧的 I 标记
        }

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

        AVPacket* pkt = packet; // [P2] 复用循环外分配的 packet
        while (ret >= 0) {
            ret = avcodec_receive_packet(codecCtx_, pkt);
            if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF)
                break;
            if (ret < 0)
                break;

            bool isKeyframe = (pkt->flags & AV_PKT_FLAG_KEY) != 0;
            if (isKeyframe)
                hasIdr_.store(true);
            QByteArray data(reinterpret_cast<char*>(pkt->data), pkt->size);
            qint64 timestamp = QDateTime::currentMSecsSinceEpoch() - startTime_;

            emit encodedFrame(data, isKeyframe, timestamp);

            av_packet_unref(pkt);
        }

        // EMA 与过载判定统一在 updateOverloadState 内完成。
        // [B7] 旧代码在这里先更新一次 encodeEmaMs_，updateOverloadState 里又更新一次，
        // 等于每帧双重平滑（新样本权重 0.36 而非 0.2），过载判定系统性偏移。
        // 冷启动前几帧含编码器预热（首帧必然慢），跳过过载判定避免启动时误降码率
        if (frameCount_ <= 5) {
            if (codecCtx_->gop_size == 1)
                codecCtx_->gop_size = fps_;
            continue;
        }
        updateOverloadState(encodeStart);

        // 恢复正常 GOP 间隔
        if (codecCtx_->gop_size == 1)
            codecCtx_->gop_size = fps_;
    }
    av_packet_free(&packet); // [P2] 复用的 packet 在线程退出时统一释放
}

void VideoEncoder::updateOverloadState(qint64 encodeStartMs)
{
    qint64 encodeMs = QDateTime::currentMSecsSinceEpoch() - encodeStartMs;
    encodeEmaMs_ = (encodeEmaMs_ == 0) ? encodeMs : (encodeEmaMs_ * 0.8 + encodeMs * 0.2);
    double frameIntervalMs = fps_ > 0 ? 1000.0 / fps_ : 33.0;

    // 冷启动前几帧含编码器预热（首帧必然慢），跳过过载判定避免启动时误降码率
    if (frameCount_ <= 5)
        return;

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

    // 绝不能用 terminate() 强杀编码线程：
    // 1) 线程正持有 mutex_（encode()/encodingLoop 的 QMutexLocker）时被杀，
    //    mutex_ 永久锁死，下一次 initialize() 的 QMutexLocker 立即死锁；
    // 2) 线程可能正处在 emit encodedFrame() 的半发射状态（QMetaObject::activate 中途），
    //    强杀会让接收方访问已析构对象。
    // 正确做法：只置 abort_ + 唤醒条件变量，让线程自己从循环退出；超时只告警不硬杀。
    bool stopped = true;
    if (encoderThread_.isRunning()) {
        encoderThread_.requestInterruption();
        encoderThread_.quit();          // 线程无事件循环时无效，仅作兜底
        stopped = encoderThread_.wait(8000);
        if (!stopped) {
            // [H14] 不能无限 wait（调用方全在主线程，冻结 = 整个服务失联），
            // 也不能超时后继续销毁（QThread 析构 abort / 编码循环 UAF 成员）。
            // 折中：再给 25s 宽限（覆盖慢盘/驱动瞬时恢复）；仍不停说明卡死在
            // 驱动层编码调用里。服务由 systemd 托管（Restart=on-failure），
            // 主动 abort 换来自动重启恢复，优于静默永久卡死。
            qCritical() << "Encoder thread did not stop within 8s; granting 25s grace...";
            stopped = encoderThread_.wait(25000);
            if (!stopped)
                qFatal("VideoEncoder: encoding thread wedged in a driver-level call; "
                       "aborting so systemd can restart the service");
        }
    }

    // 只有线程确实停了才释放资源，避免与仍在运行的编码线程竞争（UAF）
    if (stopped) {
        if (mpp_) {
            mpp_->shutdown();
            delete mpp_;
            mpp_ = nullptr;
        }

        // 与 initialize() 开头的清理保持一致，保证 shutdown 后可安全重建
        releaseAv(&codecCtx_, &frame_, &swsCtx_);
    }
}
