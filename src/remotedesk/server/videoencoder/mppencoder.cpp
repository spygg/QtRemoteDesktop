#include "mppencoder.h"
#include "videoencoder.h"  // CodecType 枚举

#ifdef USE_MPP

#include <QDateTime>
#include <QDebug>
#include <QFile>

extern "C" {
#include <rockchip/rk_mpi.h>
#include <rockchip/rk_mpi_cmd.h>
#include <rockchip/rk_type.h>
#include <rockchip/rk_venc_cfg.h>
#include <rockchip/rk_venc_rc.h>
#include <rockchip/rk_venc_kcfg.h>
#include <rockchip/mpp_task.h>
#include <rockchip/mpp_frame.h>
#include <rockchip/mpp_packet.h>
#include <rockchip/mpp_buffer.h>
#include <libavutil/pixfmt.h>
#include <libswscale/swscale.h>
}

// MPP_PACKET_FLAG_INTRA 定义在 mpp 内部头 mpp/base/inc/mpp_packet_impl.h（值 0x00000010），
// 未随 public 头安装，此处按稳定 ABI 值定义。
#ifndef MPP_PACKET_FLAG_INTRA
#define MPP_PACKET_FLAG_INTRA 0x00000010
#endif

static inline int mppAlign16(int v) { return (v + 15) & ~15; }

// MPP 1.1.0 编码器出包不设置 MPP_PACKET_FLAG_INTRA（内部头常量已无代码引用），
// 关键帧必须从 NAL 头判断，否则前端 WebCodecs 会一直等待 keyframe 而不配置解码器。
// H264: 5=IDR (7/8=SPS/PPS 随 IDR 同包)；HEVC: 19=IDR_W_RADL 20=IDR_N_LP 21=CRA
static bool isKeyframeNal(const QByteArray& data, bool hevc)
{
    int i = 0;
    const int n = data.size();
    while (i < n - 3) {
        if (data[i] == 0 && data[i + 1] == 0 && data[i + 2] == 1)
            break;
        i++;
    }
    if (i >= n - 3)
        return false;
    int h = i + 3;
    if (h >= n)
        return false;
    uint8_t nal = static_cast<uint8_t>(data[h]);
    if (hevc) {
        int type = (nal >> 1) & 0x3f;
        return type == 19 || type == 20 || type == 21;
    }
    int type = nal & 0x1f;
    return type == 5 || type == 7;
}

MppEncoder::MppEncoder() {}
MppEncoder::~MppEncoder() { shutdown(); }

bool MppEncoder::isSupported()
{
    return QFile::exists("/dev/mpp_service");
}

bool MppEncoder::initialize(int codec, int w, int h, int fps, int bitrate, bool forceHw)
{
    shutdown();
    codec_ = codec;
    width_ = w;
    height_ = h;
    fps_ = fps;
    bitrate_ = bitrate;
    horStride_ = mppAlign16(w);
    verStride_ = mppAlign16(h);

    // RGB32 -> NV12（YUV420SP = NV12）缩放上下文，编码线程直接使用
    sws_ = sws_getContext(w, h, AV_PIX_FMT_RGB32,
                          w, h, AV_PIX_FMT_NV12,
                          SWS_FAST_BILINEAR, nullptr, nullptr, nullptr);
    if (!sws_) {
        qCritical() << "MppEncoder: sws_getContext failed";
        teardown();
        return false;
    }

    // NV12 帧缓冲：Y + UV 两平面，大小按 16 对齐 stride 计算
    mpp_buffer_get(NULL, &buffer_, (size_t)horStride_ * verStride_ * 3 / 2);
    if (!buffer_) {
        qCritical() << "MppEncoder: mpp_buffer_get failed";
        teardown();
        return false;
    }

    if (!setupEncoder()) {
        teardown();
        return false;
    }

    active_ = true;
    frameCount_ = 0;

    // 探测编码器是否真正可用：本设备 MPP 1.1.0 初始化成功但 encode 恒返回 null
    // （kcfg/kmpp 缺失），若等运行时连续失败再回退，静止桌面将长期黑屏。
    // 喂一帧黑帧，3 次尝试（含内部重试）均无输出则立即判为不可用，由上层回退软编。
    {
        QImage probe(w, h, QImage::Format_RGB32);
        probe.fill(Qt::black);
        QByteArray dummy;
        bool dummyKey = false;
        if (!encode(probe, dummy, dummyKey) || dummy.isEmpty()) {
            qWarning() << "MppEncoder probe encode produced no output, treating as unavailable";
            teardown();
            return false;
        }
        // 探测帧（IDR）被丢弃未发送，强制下一帧重新出关键帧，避免前端等 keyframe 超时
        keyframeReq_.store(true);
    }

    qInfo() << "MppEncoder initialized" << (static_cast<CodecType>(codec_) == CodecType::HEVC ? "hevc" : "h264")
            << w << "x" << h << "@" << fps << "fps stride" << horStride_ << "x" << verStride_;
    return true;
}

bool MppEncoder::setupEncoder()
{
    // 重建会话（requestKeyframe / 首次初始化）
    if (ctx_) {
        mpp_destroy(ctx_);
        ctx_ = nullptr;
        mpi_ = nullptr;
    }
    if (cfg_) {
        mpp_enc_cfg_deinit(cfg_);
        cfg_ = nullptr;
    }

    MppApi* mpiTmp = nullptr;
    MPP_RET ret = mpp_create(&ctx_, &mpiTmp);
    mpi_ = mpiTmp;
    if (ret != MPP_OK || !ctx_ || !mpi_) {
        qCritical() << "MppEncoder: mpp_create failed" << ret;
        return false;
    }

    MppCodingType coding = (static_cast<CodecType>(codec_) == CodecType::HEVC) ? MPP_VIDEO_CodingHEVC : MPP_VIDEO_CodingAVC;
    ret = mpp_init(ctx_, MPP_CTX_ENC, coding);
    if (ret != MPP_OK) {
        qCritical() << "MppEncoder: mpp_init failed" << ret;
        return false;
    }

    // kmpp 路径 init 配置（官方 mpi_enc_test 同款）：RK3588 的 HEVC 编码器（vepu541）
    // 依赖 MPP_SET_VENC_INIT_KCFG 才能正常输出码流；仅旧流程时 H.264 可绕开此配置
    MppVencKcfg initCfg = nullptr;
    ret = mpp_venc_kcfg_init(&initCfg, MPP_VENC_KCFG_TYPE_INIT);
    if (ret == MPP_OK && initCfg) {
        mpp_venc_kcfg_set_u32(initCfg, "type", (RK_U32)MPP_CTX_ENC);
        mpp_venc_kcfg_set_u32(initCfg, "coding", (RK_U32)coding);
        mpp_venc_kcfg_set_s32(initCfg, "chan_id", 0);
        mpp_venc_kcfg_set_s32(initCfg, "online", 0);
        mpp_venc_kcfg_set_u32(initCfg, "max_width", (RK_U32)width_);
        mpp_venc_kcfg_set_u32(initCfg, "max_height", (RK_U32)height_);
        mpp_venc_kcfg_set_u32(initCfg, "max_lt_cnt", 0);
        mpp_venc_kcfg_set_s32(initCfg, "input_timeout", MPP_POLL_BLOCK);
        ret = static_cast<MppApi*>(mpi_)->control(static_cast<MppCtx>(ctx_), MPP_SET_VENC_INIT_KCFG, initCfg);
        if (ret != MPP_OK)
            qWarning() << "MppEncoder: MPP_SET_VENC_INIT_KCFG failed" << ret;
        mpp_venc_kcfg_deinit(initCfg);
    } else {
        qWarning() << "MppEncoder: mpp_venc_kcfg_init failed" << ret;
    }

    ret = mpp_enc_cfg_init(&cfg_);
    if (ret != MPP_OK || !cfg_) {
        qCritical() << "MppEncoder: mpp_enc_cfg_init failed" << ret;
        return false;
    }

    auto set32 = [&](const char* key, RK_S32 v) {
        MPP_RET r = mpp_enc_cfg_set_s32(cfg_, key, v);
        if (r != MPP_OK)
            qWarning() << "MppEncoder: cfg set" << key << "failed" << r;
        return r == MPP_OK;
    };
    set32("prep:width", width_);
    set32("prep:height", height_);
    set32("prep:hor_stride", horStride_);
    set32("prep:ver_stride", verStride_);
    set32("prep:format", (RK_S32)MPP_FMT_YUV420SP);
    // 官方 mpi_enc_test 默认 VBR；RK3588 vepu541 在 CBR 下节流过严，
    // 输入帧间隔稍大就长期返回 null packet（黑屏），VBR 更宽容
    set32("rc:mode", (RK_S32)MPP_ENC_RC_MODE_VBR);
    set32("rc:bps_target", bitrate_);
    set32("rc:bps_max", bitrate_ * 17 / 16);
    set32("rc:bps_min", bitrate_ * 15 / 16);
    set32("rc:gop", fps_);          // 每秒一个 IDR（与 FFmpeg gop=1s 行为一致）
    set32("rc:fps_in_flex", 0);
    set32("rc:fps_in_num", fps_);
    set32("rc:fps_in_denom", 1);   // 官方键名（mpp venc_rc 用 denom，非 denorm）
    // 官方同时设置输出帧率；只设 fps_in 会导致编码器内部输出调度缺失而 null
    set32("rc:fps_out_flex", 0);
    set32("rc:fps_out_num", fps_);
    set32("rc:fps_out_denom", 1);
    set32("codec:type", (RK_S32)coding);

    ret = static_cast<MppApi*>(mpi_)->control(static_cast<MppCtx>(ctx_), MPP_ENC_SET_CFG, cfg_);
    if (ret != MPP_OK) {
        qCritical() << "MppEncoder: MPP_ENC_SET_CFG failed" << ret;
        return false;
    }

    // 每个 IDR 携带完整参数集（H264: SPS/PPS；HEVC: VPS/SPS/PPS）→ 前端 WebCodecs annexb 直接可解
    RK_S32 headerMode = MPP_ENC_HEADER_MODE_EACH_IDR;
    ret = static_cast<MppApi*>(mpi_)->control(static_cast<MppCtx>(ctx_), MPP_ENC_SET_HEADER_MODE, &headerMode);
    if (ret != MPP_OK)
        qWarning() << "MppEncoder: set header mode failed" << ret;

    name_ = (static_cast<CodecType>(codec_) == CodecType::HEVC) ? QStringLiteral("H.265 (hevc_rkmpp)")
                                                                : QStringLiteral("H.264 (h264_rkmpp)");
    hwName_ = (static_cast<CodecType>(codec_) == CodecType::HEVC) ? QStringLiteral("hevc_rkmpp")
                                                                  : QStringLiteral("h264_rkmpp");
    return true;
}

bool MppEncoder::encode(const QImage& img, QByteArray& out, bool& keyframe)
{
    if (!active_ || !ctx_ || !mpi_ || !sws_ || !buffer_)
        return false;

    // 强制关键帧：重建会话（MPP 无 force-IDR 命令）
    if (keyframeReq_.exchange(false)) {
        if (!setupEncoder())
            return false;
    }

    uint8_t* ptr = static_cast<uint8_t*>(mpp_buffer_get_ptr(buffer_));
    if (!ptr) {
        qWarning() << "MppEncoder: mpp_buffer_get_ptr failed";
        return false;
    }
    uint8_t* dstData[2] = { ptr, ptr + horStride_ * verStride_ };
    int dstLinesize[2] = { horStride_, horStride_ };
    const uint8_t* srcData[1] = { img.bits() };
    int srcLinesize[1] = { static_cast<int>(img.bytesPerLine()) };
    int swsRet = sws_scale(static_cast<SwsContext*>(sws_), srcData, srcLinesize, 0, img.height(), dstData, dstLinesize);
    if (swsRet != height_) {
        qWarning() << "MppEncoder: sws_scale returned" << swsRet << "expected" << height_;
        return false;
    }

    // 帧按 pts 递增（单位 us）；同步 encode 偶尔返回 null/空包（固定帧率节流边界）。
    // 对同一帧重试一次即可，过多重试无意义（节流由后续帧输入解除）。
    int64_t pts = frameCount_ * (1000000LL / qMax(1, fps_));
    frameCount_++;

    MppPacket packet = nullptr;
    MPP_RET ret = MPP_ERR_UNKNOW;
    for (int attempt = 0; attempt < 2; ++attempt) {
        packet = nullptr;
        MppFrame f = nullptr;
        mpp_frame_init(&f);
        if (!f) {
            qWarning() << "MppEncoder: mpp_frame_init failed";
            return false;
        }
        mpp_frame_set_width(f, width_);
        mpp_frame_set_height(f, height_);
        mpp_frame_set_hor_stride(f, horStride_);
        mpp_frame_set_ver_stride(f, verStride_);
        mpp_frame_set_fmt(f, MPP_FMT_YUV420SP);
        mpp_frame_set_pts(f, pts);
        mpp_frame_set_buffer(f, buffer_);
        ret = static_cast<MppApi*>(mpi_)->encode(static_cast<MppCtx>(ctx_), f, &packet);
        mpp_frame_deinit(&f);
        if (ret == MPP_OK && packet && mpp_packet_get_size(packet) > 0)
            break;
        if (attempt < 2)
            qWarning() << "MppEncoder: encode retry" << (attempt + 1) << "null/empty, ret =" << ret;
    }
    if (ret != MPP_OK) {
        qWarning() << "MppEncoder: mpi encode failed, ret =" << ret << "frameCount =" << (frameCount_ - 1);
        return false;
    }
    if (!packet) {
        qWarning() << "MppEncoder: encode returned null packet, frameCount =" << (frameCount_ - 1);
        return false;
    }

    void* data = mpp_packet_get_data(packet);
    size_t size = mpp_packet_get_size(packet);
    if (size > 0 && data) {
        out = QByteArray(reinterpret_cast<char*>(data), static_cast<int>(size));
        bool isHevc = (static_cast<CodecType>(codec_) == CodecType::HEVC);
        keyframe = isKeyframeNal(out, isHevc);
    } else {
        qWarning() << "MppEncoder: empty packet size =" << size << "frameCount =" << (frameCount_ - 1);
        mpp_packet_deinit(&packet);
        return false;
    }
    mpp_packet_deinit(&packet);
    return true;
}

void MppEncoder::setBitrate(int bitrate)
{
    if (bitrate <= 0)
        return;
    bitrate_ = bitrate;
    if (!active_ || !cfg_ || !mpi_)
        return;
    // MPP 支持运行时更新码控参数
    mpp_enc_cfg_set_s32(cfg_, "rc:bps_target", bitrate_);
    mpp_enc_cfg_set_s32(cfg_, "rc:bps_max", bitrate_ * 17 / 16);
    mpp_enc_cfg_set_s32(cfg_, "rc:bps_min", bitrate_ * 15 / 16);
    MPP_RET r = static_cast<MppApi*>(mpi_)->control(static_cast<MppCtx>(ctx_), MPP_ENC_SET_CFG, cfg_);
    if (r != MPP_OK)
        qWarning() << "MppEncoder: setBitrate control failed" << r;
}

void MppEncoder::requestKeyframe()
{
    keyframeReq_.store(true);
}

void MppEncoder::teardown()
{
    active_ = false;
    if (cfg_) {
        mpp_enc_cfg_deinit(cfg_);
        cfg_ = nullptr;
    }
    if (buffer_) {
        mpp_buffer_put(buffer_);
        buffer_ = nullptr;
    }
    if (sws_) {
        sws_freeContext(static_cast<SwsContext*>(sws_));
        sws_ = nullptr;
    }
    if (ctx_) {
        mpp_destroy(ctx_);
        ctx_ = nullptr;
        mpi_ = nullptr;
    }
}

void MppEncoder::shutdown()
{
    teardown();
}

#else // !USE_MPP —— 非 Rockchip/非 Linux：空实现，保证跨平台编译

MppEncoder::MppEncoder() {}
MppEncoder::~MppEncoder() { shutdown(); }
bool MppEncoder::isSupported() { return false; }
bool MppEncoder::initialize(int, int, int, int, int, bool) { return false; }
void MppEncoder::shutdown() {}
bool MppEncoder::encode(const QImage&, QByteArray&, bool&) { return false; }
void MppEncoder::setBitrate(int) {}
void MppEncoder::requestKeyframe() {}

#endif // USE_MPP
