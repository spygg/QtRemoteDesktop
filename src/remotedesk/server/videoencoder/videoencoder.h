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

#include "codecdefs.h" // CodecType / HwEncodeMode（无 FFmpeg 的模块也要用）

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/avutil.h>
#include <libavutil/imgutils.h>
#include <libavutil/pixdesc.h>
#include <libswscale/swscale.h>
}

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

    // 编码线程是否正在处理某一帧：>0 表示已持续处理该帧的毫秒数，0 表示空闲。
    // 上层（IDR 兜底重建）据此避免在慢机器上打断正在进行的编码：单帧可能耗时
    // 数秒（QEMU/弱 ARM/4K），固定超时重建会把每帧都掐断，形成"重建→丢弃→重建"
    // 活锁（实测 TCG 1080p：30s 只输出 1 帧、每 1.2~4s 重建一次）。
    // 内部用单调时钟（steady_clock）：NTP 校时/改系统时间不会让该值跳变。
    qint64 busyForMs() const;
    // 最近一帧的完整处理耗时（转换 + 编码 + 发送，毫秒）；0 表示尚无样本。
    qint64 lastFrameCostMs() const { return lastFrameCostMs_.load(); }
    // 距最近一次 initialize() 的毫秒数（单调时钟）。上层判定"是否真的一直没产 IDR"
    // 时必须先排除"编码器刚建好、还没机会处理任何帧"的情况：用 initAgeMs() 判断。
    // 注意不要自己取墙钟再与内部时刻相减——两者时钟源不同，NTP 跳变会误判重建。
    qint64 initAgeMs() const;

    // 重建前的退役判定（供 IDR 兜底用）：把"当前是否空闲"与"置 abort_"放进同一把
    // 锁，避免先 busyForMs()==0 再 shutdownRetire() 之间编码线程恰好开始新帧，
    // 让主线程白等一整帧（慢机可达数秒）。
    enum class RetireState {
        Retired, // 线程已停、资源已释放，可以重新 initialize()
        Busy,    // 有在飞帧：未做任何改动，调用方应稍后重试
        Wedged,  // 线程卡死在驱动调用里：资源未释放，调用方必须退役本对象
    };
    RetireState retireIfIdle();

    // 退役对象（retireIfIdle/shutdownRetire 返回过 false）的线程是否已自行退出。
    // 卡死时线程仍可能触碰 codecCtx_/mpp_，此时析构是 UB；线程退出后才能安全回收，
    // 析构会走 shutdown()（isRunning()==false，直接释放资源，不再等待）。
    bool threadStopped() const;

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
    // 置 abort_ 后限时等线程退出；成功则释放资源。超时返回 false 且不释放
    // （线程仍在触碰 codecCtx_/mpp_），由调用方决定退役。
    bool stopEncoderThread(int timeoutMs);

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
    // 同尺寸（缩放 100%）且目标为 YUV420P 时改用内置转换，绕过 sws：
    // ARM（FFmpeg 3.4 的 aarch64 swscale 无 RGB→YUV 的 NEON 实现，通用 C 路径
    // 比融合标量循环慢约 9 倍）与 x86+AVX2（实测 1080p 单帧 0.65ms vs sws 3.11ms，
    // 约 4.8 倍）开启。
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
    // 编码线程当前帧的起始时刻与上一帧总耗时（单调时钟毫秒，见 busyForMs()/lastFrameCostMs()）
    std::atomic<qint64> inFlightStartMs_{ 0 };
    std::atomic<qint64> lastFrameCostMs_{ 0 };
    std::atomic<qint64> initAtMs_{ 0 };  // initialize() 进入时刻（单调时钟毫秒，见 initAgeMs()）
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
