#include "videoencoder.h"
#include "mppencoder.h"
#include "rgb2yuv.h"
#include <QDateTime>
#include <chrono>
#include <cstring>
#include <QDebug>
#include <QFile>
#include <thread>
extern "C" {
#include <libavutil/error.h>
}
#ifdef Q_OS_LINUX
#include <QDir>
#endif

// [FIX-B2] 编码时间戳基准必须是进程级单调值。
// 浏览器（WebCodecs / MSE）按 timestamp 排序，任何一次编码器重建（切分辨率/
// 画质/帧率/缩放/编码协议、MPP 回退、PLI 兜底）若把基准归零，时间戳就会回退，
// 新帧被判为"过期"直接丢弃 —— 表现为重建后画面冻结直到下一个自然 IDR。
// 用函数内 static 保证整个进程只取一次，所有实例/所有重建共享同一基准。
static qint64 encoderEpochMs()
{
    static const qint64 epoch = QDateTime::currentMSecsSinceEpoch();
    return epoch;
}

// [CLOCK] 时长测量必须用单调时钟（steady_clock）：
// busyForMs()/initAgeMs() 的读数直接决定"是否重建编码器"。若用墙钟，NTP 校时或
// 用户改时间会让差值瞬间变大/变小——向前跳会误判"编码线程卡了 30s"而强杀重建，
// 向后跳则会让"从未产 IDR"永远不成立，客户端长期黑屏。
// 只用于内部时长；码流时间戳仍由 encoderEpochMs()（墙钟）提供，两者互不影响。
// +1 保证返回值恒 > 0（0 是"无在飞帧"的哨兵值）。
static qint64 monotonicMs()
{
    static const auto origin = std::chrono::steady_clock::now();
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - origin).count() + 1;
}

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

// FFmpeg < 4.0 需要显式注册编解码器；而能力探测类 static 可能在 initialize() 之前
// 被调用（如服务模式 helper 启动即上报 capture_caps），若不先注册，
// avcodec_find_encoder() 会返回 nullptr → 误报"本机不支持视频"，前端被永久降级。
static void ensureAvCodecsRegistered()
{
#if LIBAVCODEC_VERSION_INT < AV_VERSION_INT(58, 0, 0)
    static std::atomic<bool> done{ false };
    if (!done.exchange(true))
        avcodec_register_all();
#endif
}

// 硬件编码器平台可用性粗筛（编码器存在 != 运行时可用；避免假阳性导致每次初始化
// 都尝试 open 一个必然失败的编码器，从而产生无谓的延迟与告警日志）
static bool hwPlatformAvailable(const char* encName){
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
    ensureAvCodecsRegistered();
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
static void releaseAv(AVCodecContext** ctx, AVFrame** frame, std::vector<SwsContext*>* sws)
{
    if (sws) {
        for (SwsContext*& c : *sws) {
            if (c) {
                sws_freeContext(c);
                c = nullptr;
            }
        }
        sws->clear();
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

static int gcdInt(int a, int b)
{
    while (b) {
        const int t = a % b;
        a = b;
        b = t;
    }
    return a < 0 ? -a : a;
}

void VideoEncoder::releaseBandScratch()
{
    for (AVFrame*& f : bandTmpFrames_)
        av_frame_free(&f);
    bandTmpFrames_.clear();
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

// CodecType → FFmpeg CodecID（与 initialize() 内的 switch 保持一致）
static AVCodecID avCodecIdFor(CodecType type)
{
    switch (type) {
    case CodecType::H264: return AV_CODEC_ID_H264;
    case CodecType::HEVC: return AV_CODEC_ID_HEVC;
    case CodecType::VP8:  return AV_CODEC_ID_VP8;
    case CodecType::VP9:  return AV_CODEC_ID_VP9;
    case CodecType::AV1:  return AV_CODEC_ID_AV1;
    case CodecType::MPEG4:return AV_CODEC_ID_MPEG4;
    case CodecType::MJPEG:return AV_CODEC_ID_MJPEG;
    }
    return AV_CODEC_ID_NONE;
}

bool VideoEncoder::isCodecEncodable(CodecType type)
{
    ensureAvCodecsRegistered();
    if (isHwAcceleratedAvailable(type))
        return true;
    const AVCodecID id = avCodecIdFor(type);
    return id != AV_CODEC_ID_NONE && avcodec_find_encoder(id) != nullptr;
}

int VideoEncoder::estimateBitrate(CodecType codec, int encW, int encH, int fps, int qualityLevel)
{
    double bitsPerPixel;
    switch (qualityLevel) {
    case 80: bitsPerPixel = 0.20; break; // high：加大每像素码率，1080P60 下显著高于 medium
    case 60: bitsPerPixel = 0.11; break; // medium
    case 35: bitsPerPixel = 0.08; break; // low
    case 15: bitsPerPixel = 0.035; break; // verylow：极低码率，叠加降分辨率可见
    default: bitsPerPixel = 0.10; break;
    }
    // HEVC/VP9/AV1 压缩效率约为 H.264 的两倍，同画质下码率减半，节省带宽
    if (codec == CodecType::HEVC || codec == CodecType::VP9 || codec == CodecType::AV1)
        bitsPerPixel *= 0.5;
    qint64 pixels = static_cast<qint64>(encW) * encH;
    qint64 bitrate = static_cast<qint64>(pixels * bitsPerPixel * qMax(1, fps));
    // 实际范围：150kbps ~ 25Mbps（上限须够大，否则 1080P60 high/medium 被截断同码率）
    if (bitrate < 150000) bitrate = 150000;
    if (bitrate > 25000000) bitrate = 25000000;
    return static_cast<int>(bitrate);
}

bool VideoEncoder::initialize(CodecType type, int srcW, int srcH, int encW, int encH, int fps, int bitrate,
                              HwEncodeMode hwMode)
{
    // 契约安全：initialize() 允许被重复调用（上层 reinit 未必先 shutdown）。若编码
    // 线程仍在运行，必须先把线程停下来再释放/覆盖上下文，否则线程会继续使用马上
    // 被 releaseAv() 释放的 codecCtx_/frame_/swsCtxs_（use-after-free）。
    if (encoderThread_.isRunning())
        shutdown();

    // 条带工作线程按下标引用 swsCtxs_：context 即将重建，必须先把它们停掉
    // （shutdown() 已走 releaseStoppedResources 时这里是空操作）。
    stopBandWorkers();

    // 支持 shutdown() 后重新初始化（缩放/帧率/画质/编码协议/硬件开关改变时重建编码器）
    // 先释放旧 MPP 实例：hwMode=Off 回退软编时必须清掉残留的 MPP（否则 encodingLoop
    // 仍会走 isActive() 的 MPP 路径，软编永不生效）
    if (mpp_) {
        delete mpp_;
        mpp_ = nullptr;
    }
    // 释放上一轮 FFmpeg 资源：initialize 允许重复调用（上层 reinit 未必先 shutdown），
    // 若不先释放就直接覆盖指针，会泄漏整套编码器上下文（硬编时还含 GPU/驱动资源）。
    releaseAv(&codecCtx_, &frame_, &swsCtxs_);
    releaseBandScratch();

    abort_ = false;
    hasIdr_.store(false);
    frameCount_ = 0;
    startTime_ = encoderEpochMs();
    pendingBitrate_.store(0);
    // 初始化为**实际**打开的码率（而不是 0）：上层过载降码率时会读
    // currentBitrate() 作为基准（reduced = max(150000, cur*0.6)）。置 0 会让首次
    // 过载算出 max(150000, 0)=150000，画面瞬间掉到底码率。
    appliedBitrate_.store(bitrate);
    forceKeyframe_.store(false);
    mppBroken_.store(false);
    mppFailCount_ = 0;          // [H18] 上一轮累计的失败计数不清零，新实例再失败 1 次即被永久禁用
    encodeEmaMs_ = 0;
    lastOverloadLogMs_ = 0;
    overloaded_ = false;
    // 在飞标记必须随初始化归零：旧线程若因重建被掐断，残留的时间戳会让上层
    // 误判"编码线程仍在处理上一帧"，从而永远推迟 IDR 兜底重建。
    inFlightStartMs_.store(0);
    lastFrameCostMs_.store(0);
    // 记录本轮初始化时刻：上层"长时间没产 IDR 就重建"的判据必须扣除初始化的这段时间，
    // 否则新编码器还没收到任何帧就被判为"不产 IDR"而立刻重建（每次客户端接入多建一次）。
    initAtMs_.store(monotonicMs());
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
            startTime_ = encoderEpochMs();
            encoderThread_.start(QThread::HighPriority);
            // MPP 不带外带参数集：显式发一次空 extradata 让上层清掉上一轮编码器
            // （FFmpeg 软/硬编）缓存的 SPS/PPS。否则重建后新客户端仍会收到旧
            // avcC，分辨率/协议一变就花屏。
            emit codecConfigChanged(QByteArray());
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
    } else {
        // 硬编（nvenc/amf/videotoolbox）或带内参数集路径 open 后 extradata 常为空：
        // 必须显式清空上层缓存，否则上一轮编码器的旧 SPS/PPS 会被继续回放。
        emit codecConfigChanged(QByteArray());
    }

    frame_ = av_frame_alloc();
    // 分配失败（极端 OOM）必须返回：下面直接解引用 frame_ 会确定性空指针崩溃。
    if (!frame_) {
        qCritical() << "Failed to allocate AVFrame";
        return false;
    }
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
    //
    // [PERF-ARM] 单线程 sws_scale 在 ARM 上是纯 C 实现（FFmpeg 3.4 的 aarch64
    // swscale 只给 YUV→RGB 方向写了 NEON，RGB→YUV 没有），实测 Cortex-A55
    // 1600x1200 每帧 86ms，占满整个视频模式预算；Kylin/aarch64(TCG) 上 117ms，
    // perf 采样里 sws 相关函数占 ~68%（缩放档位下更是整帧单线程）。
    //
    // 因此「源分辨率 == 编码分辨率」与「缩放档位」都切成 N 条带并行（每条带
    // 一个 SwsContext，条带数需同时整除 srcH 与 encH、且目标条带行数为偶数）。
    // 同尺寸实测（Cortex-A55 / A72）：8 条带 3.36x（86ms→21.7ms），与整帧
    // 单上下文逐像素对比：1600x1200 完全零差异；1920x1080 仅 40 个色度像素
    // 有偏差（V 最大 91，占全帧 0.002%），肉眼不可见。
    //
    // 缩放档位的难点是 sws 在上下文边界处对色度垂直滤波做边缘钳制，直接切行
    // 会让接缝处留下 1 行色度错误（实测 1600x1200→1200x900 该行 V 最大偏差
    // 222，纯色块上肉眼可见横线）。解决方式：每条带的源窗口上下各扩 2 个
    // 「滤波支持单元」——unit = srcH/gcd(srcH,encH)，此时 padSrc=2*unitSrc 与
    // padDst=2*unitDst 严格成比例，条带起始行是 unit 的整数倍，缩放相位与整帧
    // 完全一致且目标边距必为偶数（色度按行对）。缩放进各条带的私有临时帧后
    // 只把可见区拷回 frame_。实测 1920x1200→1440x900 / 960x600 / 1152x720 /
    // 800x500 与整帧单上下文对比：最大偏差 1，受影响样本 <0.6%（纯滤波系数
    // 舍入），2:1 与同尺寸完全零差异。
    // 注意：拷回必须逐行 memcpy —— av_frame_get_buffer 的色度 linesize 会按
    // 32 对齐（720 → 736），按像素宽度连续拷贝会逐行错位。
    //
    // [HBAND] 竖切要求源/目标条带行数都为偶数；真实桌面可能给奇数高度
    // （如 UKUI 2048x1209），此时竖切只能取 1 条带。宽度通常可切（2048），
    // 于是回退为按列横切：色度按列成对，规则与竖切完全对称（起始列为偶数、
    // 条带宽度为偶数、缩放时左右扩同样比例的边距），CPU 上 sws 的
    // hScale16To15 / bgr32ToY 本来沿行扫描，横切后各条带可独立并行。
    {
        const int maxBands = qBound(1, QThread::idealThreadCount(), 8);
        bandPlans_.clear();
        releaseBandScratch();
        swsBandedScale_ = false;
        swsHorizontal_ = false;
        convNative_ = false;
        const bool sameSize = (srcW == encW && srcH == encH);

#if defined(__aarch64__) || defined(__arm__) || defined(_M_ARM64) || defined(_M_ARM)
        // ARM：标量版即可（Cortex-A 上 sws 的整体管线明显更慢）
        convNative_ = sameSize && pixFmt_ == AV_PIX_FMT_YUV420P;
#elif defined(RGB2YUV_X86_SIMD)
        // x86：仅在 CPU 支持 AVX2 且同尺寸时启用向量化版本；无 AVX2 的机器
        // 继续走 sws（实测无 AVX2 时标量版并不比 sws 快，不值得切换）
        convNative_ = sameSize && pixFmt_ == AV_PIX_FMT_YUV420P && rgb2yuvAvx2Available();
#endif

        // 竖切优先（已验证）；高度是奇数或公因子太小（如 2048x1209）时竖切拿不到
        // >1 条带，改按宽度横切。条带长度必须是 unit 的整数倍，否则各条带缩放比
        // 与整帧略有偏差，接缝处会出现亚像素错位。
        auto buildPlans = [&](bool horizontal) -> int {
            bandPlans_.clear();
            const int srcLen = horizontal ? srcW : srcH;
            const int dstLen = horizontal ? encW : encH;
            const int bands = pickBandCount(srcLen, dstLen, maxBands);
            if (bands <= 1)
                return 1;
            const int bandSrc = srcLen / bands;
            const int bandDst = dstLen / bands;
            int padSrc = 0;
            if (!sameSize) {
                const int g = gcdInt(srcLen, dstLen);
                padSrc = 2 * (srcLen / g);
                // 边距超过条带 1/4（极端比例）时并行收益接近零，退回单上下文
                if (padSrc * 4 > bandSrc)
                    return 1;
            }
            for (int i = 0; i < bands; ++i) {
                const int sStart = i * bandSrc;
                const int sEnd = sStart + bandSrc;
                const int sWin = qMax(0, sStart - padSrc);
                const int eWin = qMin(srcLen, sEnd + padSrc);
                BandPlan p;
                if (horizontal) {
                    p.srcX = sWin;
                    p.srcW = eWin - sWin;
                    p.dstX = i * bandDst;
                    p.dstW = bandDst;
                    p.tmpX = (sStart - sWin) * dstLen / srcLen;
                    p.tmpW = (eWin - sWin) * dstLen / srcLen;
                    p.srcY = 0;
                    p.srcH = srcH;
                    p.dstY = 0;
                    p.dstH = encH;
                    p.tmpY = 0;
                    p.tmpH = encH;
                } else {
                    p.srcY = sWin;
                    p.srcH = eWin - sWin;
                    p.dstY = i * bandDst;
                    p.dstH = bandDst;
                    p.tmpY = (sStart - sWin) * dstLen / srcLen;
                    p.tmpH = (eWin - sWin) * dstLen / srcLen;
                    p.srcX = 0;
                    p.srcW = srcW;
                    p.dstX = 0;
                    p.dstW = encW;
                    p.tmpX = 0;
                    p.tmpW = encW;
                }
                bandPlans_.push_back(p);
            }
            swsBandedScale_ = (padSrc > 0);
            swsHorizontal_ = horizontal;
            return bands;
        };

        swsBands_ = buildPlans(false);
        if (swsBands_ <= 1)
            swsBands_ = buildPlans(true);

        auto createCtx = [&](const BandPlan& p) -> SwsContext* {
            const bool useTmp = swsBandedScale_;
            if (swsHorizontal_)
                return sws_getContext(p.srcW, srcH, AV_PIX_FMT_RGB32,
                    useTmp ? p.tmpW : p.dstW, encH, codecCtx_->pix_fmt,
                    SWS_FAST_BILINEAR, nullptr, nullptr, nullptr);
            return sws_getContext(srcW, p.srcH, AV_PIX_FMT_RGB32,
                encW, useTmp ? p.tmpH : p.dstH, codecCtx_->pix_fmt,
                SWS_FAST_BILINEAR, nullptr, nullptr, nullptr);
        };
        // 原生转换（ARM 标量 / x86 AVX2）不经过 sws：不要为条带创建 SwsContext
        // （每个上下文都有独立的缩放系数表与行缓冲，8 条带白占内存），
        // 条带划分本身仍保留 —— convertBandNative 直接按 bandPlans_ 的偏移写目标平面。
        const bool nativeConv = convNative_;
        if (!nativeConv && swsBands_ > 1) {
            for (int i = 0; i < swsBands_; ++i) {
                SwsContext* c = createCtx(bandPlans_[i]);
                if (!c) break;
                swsCtxs_.push_back(c);
            }
            if (static_cast<int>(swsCtxs_.size()) != swsBands_) {
                // 任一条带创建失败：整组作废，回退单上下文整帧转换
                releaseAv(nullptr, nullptr, &swsCtxs_);
                swsBands_ = 1;
            } else if (swsBandedScale_) {
                bool tmpOk = true;
                for (int i = 0; i < swsBands_; ++i) {
                    AVFrame* tmp = av_frame_alloc();
                    if (!tmp) { tmpOk = false; break; }
                    tmp->format = codecCtx_->pix_fmt;
                    tmp->width = swsHorizontal_ ? bandPlans_[i].tmpW : encW;
                    tmp->height = swsHorizontal_ ? encH : bandPlans_[i].tmpH;
                    if (av_frame_get_buffer(tmp, 32) < 0) {
                        av_frame_free(&tmp);
                        tmpOk = false;
                        break;
                    }
                    bandTmpFrames_.push_back(tmp);
                }
                if (!tmpOk) {
                    releaseBandScratch();
                    releaseAv(nullptr, nullptr, &swsCtxs_);
                    swsBands_ = 1;
                }
            }
        }
        if (!nativeConv && swsCtxs_.empty()) {
            swsBands_ = 1;
            swsBandedScale_ = false;
            swsHorizontal_ = false;
            releaseBandScratch();
            bandPlans_.clear();
            BandPlan p;
            p.srcY = 0;
            p.srcH = srcH;
            p.dstY = 0;
            p.dstH = encH;
            p.srcX = 0;
            p.srcW = srcW;
            p.dstX = 0;
            p.dstW = encW;
            p.tmpY = 0;
            p.tmpH = 0;
            p.tmpX = 0;
            p.tmpW = 0;
            bandPlans_.push_back(p);
            SwsContext* c = createCtx(bandPlans_[0]);
            if (!c) {
                qCritical() << "sws_getContext failed";
                av_frame_free(&frame_);
                avcodec_free_context(&codecCtx_);
                codecCtx_ = nullptr;
                return false;
            }
            swsCtxs_.push_back(c);
        }
    }

    startBandWorkers(swsBands_);
    startTime_ = encoderEpochMs();
    encoderThread_.start(QThread::HighPriority);
    qInfo() << codecName_ << "encoder initialized" << encW << "x" << encH
            << "@" << fps << "fps"
            // conv=native 时色彩转换不经过 sws（ARM 标量 / x86 AVX2），日志要说实话，
            // 否则排查"某台机器为什么慢/崩"时会被 (sws ...) 这一段误导。
            << (convNative_ ? "conv native" : "sws")
            << srcW << "x" << srcH << " -> " << encW << "x" << encH
            << ", bands" << swsBands_ << (swsBandedScale_ ? " scale-banded" : "")
            << (swsHorizontal_ ? " horizontal" : "") << ")";
    emit encoderReady();
    return true;
}

// 条带数：必须同时整除 srcH 与 encH，且源/目标条带行数都为偶数（420 色度平面
// 按 2 行一组：奇数行会让后一条带的 U/V 平面偏移少一行 → 拼接处色度错位；
// 缩放条带还要求源条带起始行是偶数，色度下采样相位才与整帧一致）。
// 取满足条件的最大值，不满足则退化到 1（整帧单线程，行为与旧版一致）。
int VideoEncoder::pickBandCount(int srcLen, int dstLen, int maxBands)
{
    // 条带长度必须是 gcd 的整数倍：缩放条带的相位才与整帧一致
    const int g = gcdInt(srcLen, dstLen);
    for (int n = qMax(1, maxBands); n > 1; --n) {
        if (srcLen % n == 0 && dstLen % n == 0
            && g % n == 0
            && (srcLen / n) % 2 == 0 && (dstLen / n) % 2 == 0
            && srcLen / n >= 32 && dstLen / n >= 32)
            return n;
    }
    return 1;
}

// 标量 BGRA → YUV420P：一次遍历里同时算亮度与 2x2 平均色度，省掉 sws 的
// 逐行函数指针分发与中间平面。实测 aarch64(TCG) 2048x1209 单线程 17.8ms，
// 同尺寸 sws 为 164ms；亮度与 sws 偏差平均 0.09（最大 1），色度因 2x2 平均
// 与 sws 双线性核不同平均偏差约 1（桌面纯色块上肉眼不可见）。
// 仅用于「源尺寸 == 编码尺寸」，缩放档位仍由 sws 做重采样。
// 实现见 rgb2yuv.h（C 标量 / x86 AVX2 两条路径，输出逐位一致）。
static void rgb32ToYuv420p(const uint8_t* src, int srcStride,
                           uint8_t* yp, int ys, uint8_t* up, int us, uint8_t* vp, int vs,
                           int w, int h)
{
    rgb32ToYuv420pBest(src, srcStride, yp, ys, up, us, vp, vs, w, h);
}

int VideoEncoder::convertBandNative(const QImage& image, int band)
{
    const BandPlan& p = bandPlans_[band];
    const int srcStride = static_cast<int>(image.bytesPerLine());
    const uint8_t* src = image.bits()
        + static_cast<size_t>(p.srcY) * srcStride + static_cast<size_t>(p.srcX) * 4;

    uint8_t* dstY = frame_->data[0];
    uint8_t* dstU = frame_->data[1];
    uint8_t* dstV = frame_->data[2];
    if (swsHorizontal_) {
        // 横切：整幅高度，按列窗口写入；色度列偏移按 2 对齐
        dstY += p.dstX;
        dstU += p.dstX / 2;
        dstV += p.dstX / 2;
    } else {
        dstY += static_cast<size_t>(p.dstY) * frame_->linesize[0];
        dstU += static_cast<size_t>(p.dstY / 2) * frame_->linesize[1];
        dstV += static_cast<size_t>(p.dstY / 2) * frame_->linesize[2];
    }
    rgb32ToYuv420p(src, srcStride, dstY, frame_->linesize[0],
                   dstU, frame_->linesize[1], dstV, frame_->linesize[2],
                   p.srcW, p.srcH);
    return swsHorizontal_ ? frame_->height : p.dstH;
}

int VideoEncoder::convertBand(const QImage& image, int band)
{
    const BandPlan& p = bandPlans_[band];
    const int srcStride = static_cast<int>(image.bytesPerLine());
    const bool useTmp = swsBandedScale_;

    if (convNative_)
        return convertBandNative(image, band);

    if (swsHorizontal_) {
        const uint8_t* srcData[1] = {
            image.bits() + static_cast<size_t>(p.srcX) * 4
        };
        int srcLinesize[1] = { srcStride };
        if (!useTmp) {
            uint8_t* dst[4] = { frame_->data[0] + p.dstX,
                                frame_->data[1] + p.dstX / 2,
                                frame_->data[2] + p.dstX / 2,
                                frame_->data[3] };
            return sws_scale(swsCtxs_[band], srcData, srcLinesize, 0, p.srcH,
                             dst, frame_->linesize);
        }
        AVFrame* tmp = bandTmpFrames_[band];
        const int rows = sws_scale(swsCtxs_[band], srcData, srcLinesize, 0, p.srcH,
                                   tmp->data, tmp->linesize);
        if (rows != p.tmpH)
            return 0;
        // 逐行拷回可见列（色度 linesize 有 32 字对齐填充，不能按宽度连续拷贝）
        const int height = frame_->height;
        for (int plane = 0; plane < 3; ++plane) {
            const int pw = (plane == 0) ? p.dstW : p.dstW / 2;
            const int tmpLeft = (plane == 0) ? p.tmpX : p.tmpX / 2;
            const int dstLeft = (plane == 0) ? p.dstX : p.dstX / 2;
            const int ph = (plane == 0) ? height : (height + 1) / 2;
            for (int y = 0; y < ph; ++y)
                memcpy(frame_->data[plane]
                           + static_cast<size_t>(y) * frame_->linesize[plane] + dstLeft,
                       tmp->data[plane]
                           + static_cast<size_t>(y) * tmp->linesize[plane] + tmpLeft,
                       static_cast<size_t>(pw));
        }
        return height;
    }

    const uint8_t* srcData[1] = {
        image.bits() + static_cast<size_t>(p.srcY) * srcStride
    };
    int srcLinesize[1] = { srcStride };

    if (useTmp) {
        AVFrame* tmp = bandTmpFrames_[band];
        const int rows = sws_scale(swsCtxs_[band], srcData, srcLinesize, 0, p.srcH,
                                   tmp->data, tmp->linesize);
        if (rows != p.tmpH)
            return 0;
        // 逐行拷回可见区（色度 linesize 有 32 字对齐填充，不能按宽度连续拷贝）
        const int width = frame_->width;
        for (int plane = 0; plane < 3; ++plane) {
            const int pw = (plane == 0) ? width : (width + 1) / 2;
            const int tmpTop = (plane == 0) ? p.tmpY : p.tmpY / 2;
            const int dstTop = (plane == 0) ? p.dstY : p.dstY / 2;
            const int ph = (plane == 0) ? p.dstH : p.dstH / 2;
            for (int y = 0; y < ph; ++y)
                memcpy(frame_->data[plane]
                           + static_cast<size_t>(dstTop + y) * frame_->linesize[plane],
                       tmp->data[plane]
                           + static_cast<size_t>(tmpTop + y) * tmp->linesize[plane],
                       static_cast<size_t>(pw));
        }
        return p.dstH;
    }

    uint8_t* dst[4] = { frame_->data[0], frame_->data[1],
                        frame_->data[2], frame_->data[3] };
    int dstLinesize[4] = { frame_->linesize[0], frame_->linesize[1],
                           frame_->linesize[2], frame_->linesize[3] };
    // 亮度按条带行数偏移；色度按条带行数的一半偏移（每 2 行一组）
    if (dst[0]) dst[0] += static_cast<size_t>(p.dstY) * dstLinesize[0];
    if (dst[1]) dst[1] += static_cast<size_t>(p.dstY / 2) * dstLinesize[1];
    if (dst[2]) dst[2] += static_cast<size_t>(p.dstY / 2) * dstLinesize[2];
    if (dst[3]) dst[3] += static_cast<size_t>(p.dstY) * dstLinesize[3];
    return sws_scale(swsCtxs_[band], srcData, srcLinesize, 0, p.srcH,
                     dst, dstLinesize);
}

void VideoEncoder::bandWorkerLoop(int band)
{
    quint64 seen = 0;
    for (;;) {
        const QImage* image = nullptr;
        {
            std::unique_lock<std::mutex> lk(bandMutex_);
            bandCv_.wait(lk, [this, &seen] { return bandStop_ || bandGen_ != seen; });
            if (bandStop_)
                return;
            seen = bandGen_;
            image = bandImage_;
        }
        const int rows = convertBand(*image, band);
        {
            std::lock_guard<std::mutex> lk(bandMutex_);
            bandRows_[band] = rows;
            ++bandCompleted_;
        }
        bandDoneCv_.notify_one();
    }
}

void VideoEncoder::startBandWorkers(int bands)
{
    // 先停旧一组：initialize() 允许重复调用，条带数可能变化
    stopBandWorkers();
    if (bands <= 1)
        return;
    {
        std::lock_guard<std::mutex> lk(bandMutex_);
        bandCompleted_ = 0;
        bandGen_ = 0;
        bandImage_ = nullptr;
        bandStop_ = false;
    }
    bandThreads_.reserve(bands - 1);
    for (int i = 1; i < bands; ++i) {
        try {
            bandThreads_.emplace_back(&VideoEncoder::bandWorkerLoop, this, i);
        } catch (const std::system_error& e) {
            qWarning() << "VideoEncoder: band worker start failed:" << e.what();
            break;
        }
    }
    if (static_cast<int>(bandThreads_.size()) != bands - 1) {
        qWarning() << "VideoEncoder: only" << bandThreads_.size() << "of" << bands - 1
                   << "band workers started, falling back to serial bands";
        stopBandWorkers();
    }
}

void VideoEncoder::stopBandWorkers()
{
    std::vector<std::thread> threads;
    {
        std::lock_guard<std::mutex> lk(bandMutex_);
        bandStop_ = true;
        bandCv_.notify_all();
        threads.swap(bandThreads_);
    }
    // 工作线程可能正停在"等新帧"上，上面已唤醒；正在处理的一帧由
    // convertFrameBands 保证在本函数被调用前已收尾（调用方此时已停编码线程）。
    for (auto& t : threads) {
        if (t.joinable())
            t.join();
    }
    std::lock_guard<std::mutex> lk(bandMutex_);
    bandStop_ = false;
    bandCompleted_ = 0;
    bandImage_ = nullptr;
}

// 并行条带色彩转换。每个条带用自己的 SwsContext，且按「从第 0 行开始」调用 ——
// swscale 的 slice 接口要求同一条上下文按顺序推进（中途起头会直接返回 0 并报
// "Slices start in the middle!"），所以不能用偏移 srcSliceY 的方式切条。
//
// 条带工作线程常驻（startBandWorkers），条带 0 在编码线程上跑：本线程本来就要
// 阻塞等其余条带完成，顺手承担一条既省一次线程创建，也让唤醒/等待更均衡。
// 竖切返回写出行数之和（== frame_->height）；横切每条带都写整幅高度，只能按
// 「每条带是否成功」判定，成功时统一返回 frame_->height。
int VideoEncoder::convertFrameBands(const QImage& image)
{
    // 原生转换不需要 sws 上下文也能分条带（convertBandNative 按 bandPlans_ 写入）
    if (swsBands_ <= 1 || (!convNative_ && swsCtxs_.size() < 2)) {
        if (convNative_) {
            rgb32ToYuv420p(image.bits(), static_cast<int>(image.bytesPerLine()),
                           frame_->data[0], frame_->linesize[0],
                           frame_->data[1], frame_->linesize[1],
                           frame_->data[2], frame_->linesize[2],
                           image.width(), image.height());
            return image.height();
        }
        const int srcStride = static_cast<int>(image.bytesPerLine());
        const uint8_t* srcData[1] = { image.bits() };
        int srcLinesize[1] = { srcStride };
        return sws_scale(swsCtxs_[0], srcData, srcLinesize, 0, image.height(),
                         frame_->data, frame_->linesize);
    }

    if (static_cast<int>(bandThreads_.size()) != swsBands_ - 1) {
        // 工作线程未能全部启动：串行条带，结果一致只是慢
        bool ok = true;
        int total = 0;
        for (int i = 0; i < swsBands_; ++i) {
            const int rows = convertBand(image, i);
            ok = ok && rows != 0;
            total += rows;
        }
        if (swsHorizontal_)
            return ok ? frame_->height : 0;
        return total;
    }

    {
        std::lock_guard<std::mutex> lk(bandMutex_);
        bandImage_ = &image;
        bandCompleted_ = 0;
        ++bandGen_;
        bandCv_.notify_all();
    }
    const int firstRows = convertBand(image, 0);

    std::unique_lock<std::mutex> lk(bandMutex_);
    bandDoneCv_.wait(lk, [this] {
        return bandCompleted_ >= static_cast<int>(bandThreads_.size());
    });
    bandImage_ = nullptr;
    if (swsHorizontal_) {
        // 横切时每条带都覆盖整幅高度，行数累加没有意义；任一条带返回 0 即失败
        bool ok = (firstRows != 0);
        for (int i = 1; i < swsBands_; ++i)
            ok = ok && bandRows_[i] != 0;
        return ok ? frame_->height : 0;
    }
    int total = firstRows;
    for (int i = 1; i < swsBands_; ++i)
        total += bandRows_[i];
    return total;
}

void VideoEncoder::encode(const QImage& frame)
{
    // [V-2] 深拷贝移出锁外：持锁做 8MB 级 copy 会阻塞编码线程出队与并发 encode。
    // QImage 隐式共享下 copy() 仅在缓冲被共享时才真正复制；isDetached() 已独占
    // （如 JpegCompressor 输出）则零拷贝直接入队。
    QImage copy = frame.isDetached() ? frame : frame.copy();
    QMutexLocker locker(&mutex_);
    // 已退役/停止：直接丢弃（否则帧会积在队列里，只能等下一次 initialize 清空）
    if (abort_)
        return;
    // [V-1] 队列过载时清到 ≤2（含本帧）：原实现丢最老 1 帧后剩余 9 帧仍按序
    // 全编，编码偶发超时后延迟常驻 ~333ms 且白编过期帧。图片模式同场景已用
    // kMaxQueueSize=1。积压意味着编码跟不上，丢弃过期帧换来低延迟。
    while (frameQueue_.size() >= 2)
        frameQueue_.dequeue();
    // [DIAG] 记录入队时刻，供编码线程分解 排队等待 / 编码耗时
    lastEnqueueMs_ = QDateTime::currentMSecsSinceEpoch();
    // 必须深拷贝再入队：X11 全量抓取路径把持久缓冲 fullFrame_ 浅共享给上层，
    // 而 Damage 区域抓取路径会在同一缓冲上 memcpy 原地改写。若这里只存 QImage 引用，
    // 编码线程出队时可能读到被捕获线程并发改写的半新半旧帧 → 画面撕裂/花屏。
    frameQueue_.enqueue(copy);
    condition_.wakeOne();
}

void VideoEncoder::encodingLoop()
{
    // 一帧的"在飞"标记：析构时清零并记录本帧总耗时。用 RAII 是为了覆盖本函数
    // 里所有 continue / break 路径（丢帧、编码失败、排队等），漏掉一条就会让上层
    // 误判"编码线程空闲"从而在编码中途重建编码器。
    // 起始时刻由调用方在 mutex_ 内登记（见下方出队处），这样 retireIfIdle() 的
    // "空闲判定 + 置 abort_" 与"开始下一帧"是互斥的。
    struct InFlightGuard {
        std::atomic<qint64>& slot;
        std::atomic<qint64>& cost;
        qint64 start;
        InFlightGuard(std::atomic<qint64>& s, std::atomic<qint64>& c, qint64 startMs)
            : slot(s), cost(c), start(startMs)
        {
        }
        ~InFlightGuard()
        {
            cost.store(monotonicMs() - start, std::memory_order_relaxed);
            slot.store(0, std::memory_order_relaxed);
        }
    };

    // [P2] AVPacket 跨帧复用：旧代码每帧 av_packet_alloc/av_packet_free 各一次
    //（30fps = 每秒 60 次堆操作），packet 生命周期只在 receive 循环内，复用安全。
    AVPacket* packet = av_packet_alloc();
    while (!abort_) {
    QImage image;
    qint64 enqueueMs = 0;
    qint64 frameStartMs = 0;
    {
        QMutexLocker locker(&mutex_);
        while (frameQueue_.isEmpty() && !abort_)
            condition_.wait(&mutex_);
        if (abort_)
            break;
        image = frameQueue_.dequeue();
        enqueueMs = lastEnqueueMs_; // [DIAG] 入队时刻（近似=采集发射时刻）
        // 在锁内登记"在飞"：retireIfIdle() 用同一把锁做空闲判定，
        // 保证它看到 0 时线程不会再出队新帧（否则重建要白等一整帧）。
        // 时刻必须在出队后取：取在 wait 之前会把排队等待算进单帧耗时。
        frameStartMs = monotonicMs();
        inFlightStartMs_.store(frameStartMs, std::memory_order_relaxed);
    }
    const InFlightGuard inFlight(inFlightStartMs_, lastFrameCostMs_, frameStartMs);
    // [DIAG] ENC-LAT：排队等待（入队→出队）+ 本帧编码完成总延迟，每 60 帧打一条
    const qint64 dequeueMs = QDateTime::currentMSecsSinceEpoch();
    const qint64 queueWaitMs = dequeueMs - enqueueMs;

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
    // 因为 MPP 初始化成功时根本不会创建 swsCtxs_/frame_/codecCtx_（见 initialize 的
    // MPP 成功分支 early return），一旦走到 sws_scale 就是空指针崩溃。
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
                {
                    // [DIAG] 每输出帧计时（静止期稀疏，开销可忽略）
                    static int s_encSeq = 0;
                    static qint64 s_lastEncLog = 0;
                    const qint64 nowMs = QDateTime::currentMSecsSinceEpoch();
                    if (++s_encSeq % 15 == 0 || nowMs - s_lastEncLog > 300)
                        qInfo() << "ENC-OUT seq=" << s_encSeq << "key=" << key
                                << "sz=" << out.size() << "ts=" << timestamp;
                    s_lastEncLog = nowMs;
                }
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

    // MPP 之外的路径必须保证 FFmpeg 资源就绪，否则跳过本帧而不是崩溃。
    // 原生转换（convNative_）不需要 sws 上下文，此时 swsCtxs_ 为空是正常的；
    // 若这里仍要求非空，原生路径会每帧都被丢弃（hasIdr 永假 → 反复重建）。
    if (!codecCtx_ || !frame_ || (!convNative_ && swsCtxs_.empty()))
        continue;

    // [FIX-B4] avcodec_send_frame 不拷贝输入像素（内部只 av_frame_ref），带流水线
    // 延迟的编码器（openh264 / VP9 等）在下一帧入队时仍引用本帧缓冲。复用同一个
    // frame_ 前必须 make_writable，否则下面的 sws_scale 会覆写"编码器还在读"的
    // 像素，编出半新半旧的撕裂帧。libx264 ultrafast+zerolatency 无延迟，暂时掩盖
    // 了该问题，换编码器/开 B 帧即会暴露。
    int mwRet = av_frame_make_writable(frame_);
    if (mwRet < 0) {
        qWarning() << "VideoEncoder: av_frame_make_writable failed:" << mwRet;
        continue;
    }

    const int swsRows = convertFrameBands(image);
    if (swsRows != frame_->height) {
        // sws 失败时 frame_ 保留上一帧像素，照常送编会产出"画面停了但时间戳在走"
        // 的假活帧，且无任何线索可查 —— 直接丢帧并告警。
        qWarning() << "VideoEncoder: sws_scale returned" << swsRows
                   << "rows, expected" << frame_->height << "- frame dropped";
        continue;
    }

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
            // 导致后续每一帧都被强制为 IDR（码率暴增、CPU 过载）。
            // 恢复值必须与 initialize() 里的初值一致（fps/2，0.5s），
            // 用 fps_（1s）会让强制关键帧后 IDR 间隔永久翻倍。
            if (codecCtx_->gop_size == 1)
                codecCtx_->gop_size = qMax(1, fps_ / 2);
            // [FIX-B3] 编码错误不再静默：此前 ret<0 直接 continue，既不打日志
            // 也不上报，编码器进入错误态后会长期每帧失败，上层只能靠 hasIdr()
            // 每 4s 盲目重建，形成"重建-失败"死循环且日志无痕。
            if (ret == AVERROR(EAGAIN)) {
                // EAGAIN 是正常背压：编码器输出缓冲未排空、不接受新帧。
                // 必须走下方 receive 循环把已编码数据取走，否则输入永远不被
                // 消费、之后每帧都 EAGAIN → 画面冻结直到 4s 周期重建才恢复
                // （FFmpeg 模式周期性卡顿的来源之一）。本帧未被消费，排空后
                // 下一轮循环会以新 pts 重发，内容无损。
                consecutiveEncFailures_ = 0;
                ret = 0; // 使下方的 while (ret >= 0) 排空循环执行
            } else {
                char errBuf[AV_ERROR_MAX_STRING_SIZE] = {0};
                av_strerror(ret, errBuf, sizeof(errBuf));
                if (++consecutiveEncFailures_ == 1 || consecutiveEncFailures_ % 30 == 0) {
                    qWarning() << "VideoEncoder: send_frame failed:" << errBuf
                               << "consecutive=" << consecutiveEncFailures_;
                }
                if (consecutiveEncFailures_ >= 30) {
                    consecutiveEncFailures_ = 0;
                    qWarning() << "VideoEncoder: too many send failures, requesting reinit";
                    emit reinitRequired();
                }
                continue;
            }
        }
        consecutiveEncFailures_ = 0;

        AVPacket* pkt = packet; // [P2] 复用循环外分配的 packet
        while (ret >= 0) {
            ret = avcodec_receive_packet(codecCtx_, pkt);
            if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF)
                break;
            if (ret < 0) {
                // [FIX-B3] 真错误（非 EAGAIN/EOF）此前与 EAGAIN 同等对待静默 break
                char errBuf[AV_ERROR_MAX_STRING_SIZE] = {0};
                av_strerror(ret, errBuf, sizeof(errBuf));
                qWarning() << "VideoEncoder: receive_packet failed:" << errBuf;
                break;
            }

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
                codecCtx_->gop_size = qMax(1, fps_ / 2);
            continue;
        }
        updateOverloadState(encodeStart);
        // [DIAG] ENC-LAT：总延迟 = 排队等待 + sws + avcodec_send/receive（近似，不含 emit 后）
        if (frameCount_ % 60 == 0)
            qInfo() << "ENC-LAT queueWait=" << queueWaitMs << "ms total=" << (QDateTime::currentMSecsSinceEpoch() - enqueueMs) << "ms";

        // 恢复正常 GOP 间隔（与 initialize() 初值一致：fps/2）
        if (codecCtx_->gop_size == 1)
            codecCtx_->gop_size = qMax(1, fps_ / 2);
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

qint64 VideoEncoder::busyForMs() const
{
    const qint64 start = inFlightStartMs_.load(std::memory_order_relaxed);
    return start > 0 ? monotonicMs() - start : 0;
}

qint64 VideoEncoder::initAgeMs() const
{
    const qint64 init = initAtMs_.load(std::memory_order_relaxed);
    return init > 0 ? monotonicMs() - init : 0;
}

bool VideoEncoder::threadStopped() const
{
    return !encoderThread_.isRunning();
}

void VideoEncoder::shutdown()
{
    if (shutdownRetire())
        return;

    // [N6] 析构/退出路径无法"退役换新"，保留原兜底：再给 25s 宽限
    // （覆盖慢盘/驱动瞬时恢复）；仍不停说明卡死在驱动层编码调用里。
    // 服务由 systemd 托管（Restart=on-failure），主动 abort 换来自动重启恢复，
    // 优于静默永久卡死。
    qCritical() << "Encoder thread did not stop within 8s; granting 25s grace...";
    if (!encoderThread_.wait(25000))
        qFatal("VideoEncoder: encoding thread wedged in a driver-level call; "
               "aborting so systemd can restart the service");
    releaseStoppedResources();
}

bool VideoEncoder::shutdownRetire()
{
    {
        QMutexLocker locker(&mutex_);
        abort_ = true;
        condition_.wakeAll();
    }
    return stopEncoderThread(8000);
}

VideoEncoder::RetireState VideoEncoder::retireIfIdle()
{
    {
        QMutexLocker locker(&mutex_);
        // 与 encodingLoop 的出队共用 mutex_：这里看到 0 就说明线程此刻没有在飞帧，
        // 且它在本次 abort_ 置位后不会再出队新帧（循环顶部/等待处都会看到 abort_）。
        if (inFlightStartMs_.load(std::memory_order_relaxed) != 0)
            return RetireState::Busy;
        abort_ = true;
        condition_.wakeAll();
    }
    return stopEncoderThread(8000) ? RetireState::Retired : RetireState::Wedged;
}

bool VideoEncoder::stopEncoderThread(int timeoutMs)
{
    // 绝不能用 terminate() 强杀编码线程：
    // 1) 线程正持有 mutex_（encode()/encodingLoop 的 QMutexLocker）时被杀，
    //    mutex_ 永久锁死，下一次 initialize() 的 QMutexLocker 立即死锁；
    // 2) 线程可能正处在 emit encodedFrame() 的半发射状态（QMetaObject::activate 中途），
    //    强杀会让接收方访问已析构对象。
    // 正确做法：只置 abort_ + 唤醒条件变量，让线程自己从循环退出；超时只告警不硬杀。
    if (encoderThread_.isRunning()) {
        encoderThread_.requestInterruption();
        encoderThread_.quit();          // 线程无事件循环时无效，仅作兜底
        if (!encoderThread_.wait(timeoutMs)) {
            // 线程仍卡在驱动调用里：不释放任何资源（codecCtx_/mpp_ 还在被它触碰），
            // 返回 false 由上层把本对象断开信号后故意泄漏退役，新建编码器继续服务。
            qCritical() << "VideoEncoder: encode thread wedged in driver call; caller must retire this encoder";
            return false;
        }
    }

    releaseStoppedResources();
    return true;
}

void VideoEncoder::releaseStoppedResources()
{
    // 条带工作线程必须在释放 sws 上下文之前停掉：它们按下标引用 swsCtxs_/frame_
    if (mpp_) {
        mpp_->shutdown();
        delete mpp_;
        mpp_ = nullptr;
    }

    stopBandWorkers();

    // 与 initialize() 开头的清理保持一致，保证 shutdown 后可安全重建
    releaseAv(&codecCtx_, &frame_, &swsCtxs_);
    releaseBandScratch();
}
