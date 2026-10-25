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
#include <condition_variable>
#include <mutex>
#include <thread>
#include <vector>

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
    // [N6] 重建路径专用：停线程并释放资源；线程卡死在驱动调用时返回 false
    // （绝不 qFatal——调用方必须退役本对象、新建编码器继续服务）。
    bool shutdownRetire();

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
    // 编码线程最近一帧是否为关键帧（IDR）。上层据此前判断 flush 式强制关键帧
    // 是否生效：若长时间未产出 IDR（部分软编不吃 flush+pict_type），需重建编码器。
    bool hasIdr() const { return hasIdr_.load(); }
    // 帧尺寸与编码器/sws 上下文不匹配，需要上层重建编码器（防止 sws_scale 越界读）
    bool sizeMismatch() const { return sizeMismatch_.load(); }

    // 本进程中 MPP 是否被证明不可用（初始化成功但 encode 恒 null）。
    // 上层据此在前端展示"硬件编码实际未生效"，避免用户以为开着硬编。
    static bool isMppDisabled();

    // 指定编码类型当前是否可编码（硬件优先，否则探测软编编码器是否存在）。
    // 服务模式 helper 侧据此上报"本机是否具备视频能力"，无需真正建立编码器。
    static bool isCodecEncodable(CodecType type = CodecType::H264);

    // 依据编码分辨率/帧率/画质档位估算目标码率（bps）。
    // 抽成 static 供服务端本地编码与服务模式 helper 侧共用，避免两处公式漂移。
    static int estimateBitrate(CodecType codec, int encW, int encH, int fps, int qualityLevel);

signals:
    void encodedFrame(const QByteArray& data, bool keyframe, qint64 timestamp);
    void encoderReady();   // 编码器（重）初始化完成：请求捕获端强制推帧（静止桌面首帧兜底）
    void codecConfigChanged(const QByteArray& extradata); // 用于发送 H.264 SPS/PPS、HEVC VPS/SPS/PPS 等
    // 编码过载状态变化（true=持续过载，false=恢复），供上层自适应降质/恢复
    void encoderOverload(bool overloaded);
    // MPP 编码器被证明不可用，需要上层重新初始化编码器（重建为软编）。
    // 必须由上层在主线程执行：编码线程不能重建自己所属的 encoderThread_。
    void reinitRequired();

private slots:
    void encodingLoop();

private:
    // 编码耗时 EMA 过载监控（MPP 与 FFmpeg 两条编码路径共用）
    void updateOverloadState(qint64 encodeStartMs);
    // 线程确认停止后的资源释放（mpp 实例 + ffmpeg 上下文），shutdown/shutdownRetire 共用
    void releaseStoppedResources();

    QThread encoderThread_;
    QMutex mutex_;
    QWaitCondition condition_;
    QQueue<QImage> frameQueue_;
    // [DIAG] 最近一次入队时刻（mutex_ 保护）：用于 ENC-LAT 延迟分解日志
    qint64 lastEnqueueMs_ = 0;
    static constexpr int kMaxFrameQueueSize = 10; // 队列上限，防止 OOM
    std::atomic<bool> abort_{false};

    AVCodecContext* codecCtx_ = nullptr;
    AVFrame* frame_ = nullptr;
    // 色彩转换上下文：按 CPU 核数切成 N 条（每条独立 SwsContext），
    // 把单线程 sws_scale（ARM 上 RGB32→YUV 无 NEON 实现，实测 1600x1200 达 86ms）
    // 拆成并行条带。N=1 时退化为原来的单上下文路径。
    std::vector<SwsContext*> swsCtxs_;
    int swsBands_ = 1;
    // 每条带的行列参数。缩放档位下每条带除可见区域外还带重叠边距
    // （见 initialize 中的说明），先缩放进私有临时帧再逐行拷回 frame_。
    // 条带方向：默认按高度竖切；高度是奇数或因公因子太小切不动时，改按宽度横切
    // （此时只有 X 字段有值，Y 字段表示整幅）。
    struct BandPlan {
        int srcY = 0;   // 源帧起始行（含边距）
        int srcH = 0;   // 送入 sws 的源行数（含边距）
        int dstY = 0;   // frame_ 中的起始行
        int dstH = 0;   // frame_ 中的行数
        int tmpY = 0;   // 可见区域在临时帧中的起始行
        int tmpH = 0;   // 临时帧行数
        int srcX = 0;   // 源帧起始列（含边距）
        int srcW = 0;   // 送入 sws 的源列数（含边距）
        int dstX = 0;   // frame_ 中的起始列
        int dstW = 0;   // frame_ 中的列数
        int tmpX = 0;   // 可见区域在临时帧中的起始列
        int tmpW = 0;   // 临时帧列数
    };
    std::vector<BandPlan> bandPlans_;
    std::vector<AVFrame*> bandTmpFrames_;  // 仅缩放条带：每条带私有临时目标帧
    bool swsBandedScale_ = false;
    bool swsHorizontal_ = false;
    // 同尺寸（缩放 100%）且目标为 YUV420P 时改用内置标量转换，绕过 sws。
    // 仅 ARM 开启：FFmpeg 3.4 的 aarch64 swscale 没有 RGB→YUV 的 NEON 实现，
    // 其通用 C 路径在同一帧上比融合标量循环慢约 9 倍；x86 有 SSE 汇编，保留 sws。
    bool convNative_ = false;
    int convertBandNative(const QImage& image, int band);
    void releaseBandScratch();
    // 并行条带色彩转换；返回实际写出的目标行数（失败时 != frame_->height）
    int convertFrameBands(const QImage& image);
    // 单条带的转换：写 frame_ 对应条带区域，返回该条带写出的行数
    int convertBand(const QImage& image, int band);
    // 条带工作线程常驻复用：原先每帧 spawn N 个 std::thread（30fps × N 次
    // 创建/销毁），在弱设备（Cortex-A55 等）上这部分开销与调度抖动直接吃编码预算。
    // 初始化时按条带数建一次，跨帧复用；条带 0 由编码线程自己做。
    void startBandWorkers(int bands);
    void stopBandWorkers();
    void bandWorkerLoop(int band);
    std::vector<std::thread> bandThreads_;   // 承担条带 1..N-1
    std::mutex bandMutex_;
    std::condition_variable bandCv_;         // 新帧可用
    std::condition_variable bandDoneCv_;     // 本帧全部条带完成
    const QImage* bandImage_ = nullptr;      // 仅在一帧转换期间有效
    quint64 bandGen_ = 0;
    int bandCompleted_ = 0;
    int bandRows_[8] = { 0 };
    bool bandStop_ = false;
    // 按 srcLen/dstLen 都能整除的最大因子选条带数（保证每条带缩放相位一致、
    // 色度平面按偶数行/列对齐，拼接处不会出现 1 行错位）。
    // 竖切（高度）优先，竖切不可行时由调用方改用横切（宽度）。
    static int pickBandCount(int srcLen, int dstLen, int maxBands);
    AVPixelFormat pixFmt_ = AV_PIX_FMT_YUV420P;
    // sws 上下文建立时的源尺寸：必须与后续投入的帧尺寸一致，
    // 否则 sws_scale 会按旧宽度解释新帧（行距用新 bytesPerLine）→ 堆越界读。
    int swsSrcW_ = 0;
    int swsSrcH_ = 0;
    // 捕获到的帧尺寸与 sws 上下文不一致（分辨率/输出切换、热插拔后未重建编码器）：
    // 由编码线程置位，主线程据此触发重建。
    std::atomic<bool> sizeMismatch_{ false };
    QString hwName_;
    int64_t frameCount_ = 0;
    // 编码时间戳基准：进程级单调起点（encoderEpochMs()），重建不得归零，
    // 否则浏览器按 timestamp 排序会丢弃回退帧。
    qint64 startTime_ = 0;
    // 连续 send_frame/receive_packet 失败计数（仅编码线程访问）：达到阈值后
    // 请求上层重建，避免编码器进入错误态后长期静默失败。
    int consecutiveEncFailures_ = 0;
    int fps_ = 30;

    CodecType currentCodec_ = CodecType::H264;
    HwEncodeMode hwMode_ = HwEncodeMode::Auto;
    QString codecName_;
    std::atomic<bool> forceKeyframe_{ false };
    std::atomic<bool> hasIdr_{ false }; // 自最近一次初始化以来是否产出过 IDR（一次性标志，P 帧不重置）
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
    // MPP 编码器已判定损坏：上层重新初始化前不再向它投帧，
    // 也不能落到下面"未创建 swsCtxs_/frame_"的 FFmpeg 路径（否则空指针崩溃）。
    std::atomic<bool> mppBroken_{ false };
};

#endif // VIDEOENCODER_H
