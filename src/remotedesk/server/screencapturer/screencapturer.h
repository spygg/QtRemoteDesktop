// server/screen_capturer.h
#ifndef SCREEN_CAPTURER_H
#define SCREEN_CAPTURER_H

#include <QAtomicInt>
#include <QElapsedTimer>
#include <QGuiApplication>
#include <QImage>
#include <QJsonArray>
#include <QList>
#include <QMutex>
#include <QObject>
#include <QPoint>
#include <QScreen>
#include <QString>
#include <QThread>
#include <QTimer>

// 快速帧校验和：每隔 N 行采样一行做 CRC，大幅减少计算量
inline quint16 quickFrameChecksum(const QImage& frame)
{
    // 空帧 / 0 尺寸帧保护：避免后续 seed % h 除零崩溃（Wayland 流初始化中
    // 可能提交 0 宽高帧导致 hasFrame_ 置位），并避免无意义计算
    if (frame.isNull() || frame.width() <= 0 || frame.height() <= 0)
        return 0;

    const uchar* bits = frame.constBits();
    int stride = frame.bytesPerLine();
    int w = frame.width();
    int h = frame.height();
    int step = qMax(1, h / 32);
    quint16 result = 0;

    auto checksumRow = [&](int y) {
        if (y < 0 || y >= h) return;
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
        result ^= qChecksum(QByteArrayView(bits + y * stride, stride));
#else
        result ^= qChecksum(reinterpret_cast<const char*>(bits + y * stride), static_cast<uint>(stride));
#endif
    };

    // 1. 均匀行采样
    for (int y = 0; y < h; y += step)
        checksumRow(y);

    // 2. 伪随机 4 行补充采样（基于当前校验和做种子，无状态依赖）
    unsigned seed = static_cast<unsigned>(result);
    for (int i = 0; i < 4; i++) {
        seed = seed * 1103515245u + 12345u;
        checksumRow(static_cast<int>(seed % static_cast<unsigned>(h)));
    }

    return result;
}

#ifdef Q_OS_WIN
// 前向声明 DXGICapturer 类（实际定义在 .cpp 中）

class GdiCapturer;
#endif

#if defined(Q_OS_WIN)
class DXGICapturer;
#endif

// 平台捕获器公共基类，确保跨文件 delete 安全
class PlatformCapturer {
public:
    virtual ~PlatformCapturer() = default;
    virtual bool initialize() = 0;
    virtual bool captureFrame(QImage& outImage, bool* updated = nullptr) = 0;
    virtual void resetDamage() {}
    virtual int width() const { return 0; }
    virtual int height() const { return 0; }
    // 捕获区在根窗口/虚拟桌面中的原点（多屏）。X11 按主输出裁剪时非 0，
    // 注入端必须把前端坐标加上该偏移才能点中目标位置。
    virtual int originX() const { return 0; }
    virtual int originY() const { return 0; }
    // 区域抓取实现（如 X11 Damage）可返回 true，表示本次捕获已知有变化，
    // 上层可跳过全帧校验和以省 CPU
    virtual bool regionDirty() const { return false; }
    // 强制恒全量抓取（禁用增量 Damage）：图片/低动态模式用它规避增量损伤
    // 漏报子窗口重绘（任务栏/顶部面板）导致的局部黑块。默认 false=尽量增量。
    bool fullCaptureOnly_ = false;
    virtual void setFullCaptureOnly(bool v) { fullCaptureOnly_ = v; }
    // 请求下一次 captureFrame 强制全量抓取。外部"强制帧"（新客户端接入、
    // 关键帧泵）必须用它：有增量检测的平台（X11 Damage）在桌面无变化时会
    // 回 updated=false，强制帧被吃掉，静止桌面下最长要等 ~2s 才有画面。
    // 无增量检测的平台天然每帧全量，默认 no-op。
    virtual void requestFullCapture() {}
    // 请求下一次 captureFrame 复用最近一帧（强制帧/新客户端接入用）。
    // 与 requestFullCapture（只要求跳过增量检测、仍抓取真实画面）语义不同：
    // DXGI 在静止桌面下 AcquireNextFrame 只会超时，只能拿缓存帧顶上。
    virtual void requestForceFrame() {}
    // 帧纹理格式不受支持（如 HDR 桌面的 R16G16B16A16_FLOAT）：平台捕获器
    // 无法按 BGRA 直通时置位，ScreenCapturer 立即切到 GDI。
    virtual bool frameFormatUnsupported() const { return false; }
    // 无客户端观看时暂停采集。Wayland/PipeWire 的帧生产在独立线程里持续
    // 运行（光标闪烁/面板刷新会不断出帧），暂停可把整帧拷贝降到 1fps。
    // 仅有后台生产线程的平台需要实现；默认 no-op。
    virtual void setPaused(bool) {}
};

// [CAP-ASYNC] 抓屏工作对象：只在独立线程里执行"纯抓取"（BitBlt / DXGI 取帧 +
// 整帧拷贝），不碰任何 QTimer、不改采集器状态机。详见 ScreenCapturer::workerGrab()。
class ScreenCapturer;

class CaptureGrabWorker : public QObject {
    Q_OBJECT
public:
    explicit CaptureGrabWorker(ScreenCapturer* owner) : owner_(owner) {}
public slots:
    void grab();
signals:
    void done();
private:
    ScreenCapturer* owner_ = nullptr;
};

class ScreenCapturer : public QObject {
    Q_OBJECT

public:
    explicit ScreenCapturer(QObject* parent = nullptr);
    ~ScreenCapturer();

    bool start(int fps);
    void stop();
    void suspend();
    void resume();
    void forceNextFrame();   // 输入注入后强制下一帧通过校验（光标移动不产生 XDamage）
    // 立刻驱动一次**真实**抓帧：不等定时器、不受去重与 idle 降频影响。
    // 与 forceNextFrame() 的区别是它无条件插一次即时抓帧（后者只在"从 idle 唤醒"
    // 时插），因此可以在固定时间窗内稳定产出帧。喂帧泵必须用它：openh264 有 1 帧
    // 流水线延迟，没有真实输入就没有输出，泵打空 → 兜底定时器误判"未产出 IDR"
    // → 反复重建编码器（XP 实测一天 471 次）。
    // paced=true：仍受 pace 节流（距上次抓帧结束不足 pace/2 就本次跳过）。喂帧泵
    // 必须用它——泵每 80ms 调一次，而无节流版本会变成背靠背连抓（单帧 320ms 的
    // XP 上占空比从 33% 飙到 ~90%，每次关键帧请求都是 1.3 秒满核尖峰）。
    // 输入注入路径（forceNextFrame）用默认 false，不能为节流牺牲交互延迟。
    void nudgeCapture(bool paced = false);
    // 慢读回节流倍数：paceIntervalMs_ = capAvg × factor（默认 2 ⇒ duty≈33%）。
    // 慢机可调大（4 ⇒ 20%、9 ⇒ 10%）用帧率换 CPU；快机不生效（pace 恒 0）。
    void setPaceFactor(double factor) { paceFactor_ = qBound(1.0, factor, 12.0); }
    void setFps(int fps);

    // [KFR] 桌面内容真实变化计数：仅在两次**出帧**之间采样校验和不同时递增。
    // 与去重用的 lastFrameChecksum_ 完全独立（后者会被 forceNextFrame()/resume()
    // 重置为 0，不能用来判断"内容是否变过"）。强制抓帧（forceNextFrame，画面未变
    // 但绕过去重）不会改变校验和，故不会误计数。
    // RDPServer 用它判断"缓存的关键帧是否仍与当前桌面一致"——一致则浏览器 PLI
    // 可直接重发缓存 IDR，省掉一次全屏抓屏 + H.264 IDR 编码（服务模式下还省掉
    // 一次"服务端→helper→服务端"的关键帧请求往返）；不一致则必须重新出帧，
    // 否则重发旧 IDR 会让客户端参考链错位（花屏）。
    quint64 contentChangeCount() const { return contentChangeCount_; }

    // [ADAPT] 实测单帧采集耗时（指数滑动平均，ms）。慢读回机器（VMware SVGA 上的
    // XP 可达 320~350ms）据此自动下调 fps/缩放并提高节流倍数。异步抓取时由主线程
    // 在 onGrabDone 里更新，跨线程读用原子。
    int avgCaptureMs() const { return avgCaptureMs_.loadAcquire(); }

    // 切换恒全量抓取模式（图片模式 true；视频模式 false）。转发到平台捕获器。
    void setFullCaptureOnly(bool v) {
#ifdef Q_OS_LINUX
        if (useX11_ && x11Capturer_)
            x11Capturer_->setFullCaptureOnly(v);
        if (useWayland_ && waylandCapturer_)
            waylandCapturer_->setFullCaptureOnly(v);
#endif
    }

    int width() const {
#ifdef Q_OS_LINUX
        if (useX11_ && x11Capturer_)
            return x11Capturer_->width();
        if (useWayland_ && waylandCapturer_)
            return waylandCapturer_->width();
#endif
#ifdef Q_OS_WIN
        // DXGI 自愈会按当前帧纹理修正内部尺寸（外部改了分辨率/热插拔），
        // 这里优先读捕获器实时尺寸，避免 screen_info/编码器用陈旧的枚举尺寸。
        if (useDXGI_ && dxgiCapturer_ && dxgiCapturer_->width() > 0)
            return dxgiCapturer_->width();
        // 多屏：必须返回**当前选中输出**的尺寸，而不是 screen_（主屏 QScreen）。
        // 选了非主输出（如副显示器）时二者不同，返回主屏尺寸会让编码器按错误尺寸
        // 初始化、每帧尺寸校验失败 → 周期性重建编码器、坐标映射错位。
        if (winCurrentIndex_ >= 0 && winCurrentIndex_ < winOutputs_.size()
            && winOutputs_[winCurrentIndex_].w > 0)
            return winOutputs_[winCurrentIndex_].w;
#endif
        return screen_ ? screen_->size().width() : 0;
    }
    int height() const {
#ifdef Q_OS_LINUX
        if (useX11_ && x11Capturer_)
            return x11Capturer_->height();
        if (useWayland_ && waylandCapturer_)
            return waylandCapturer_->height();
#endif
#ifdef Q_OS_WIN
        if (useDXGI_ && dxgiCapturer_ && dxgiCapturer_->height() > 0)
            return dxgiCapturer_->height();
        if (winCurrentIndex_ >= 0 && winCurrentIndex_ < winOutputs_.size()
            && winOutputs_[winCurrentIndex_].h > 0)
            return winOutputs_[winCurrentIndex_].h;
#endif
        return screen_ ? screen_->size().height() : 0;
    }

    // 当前捕获区在根窗口/虚拟桌面中的原点（Linux X11 多屏 / Windows 多显示器；
    // 其余平台为 0）。注入端据此把前端坐标换算成屏幕绝对坐标。
    int originX() const {
#ifdef Q_OS_LINUX
        if (useX11_ && x11Capturer_)
            return x11Capturer_->originX();
        if (useWayland_ && waylandCapturer_)
            return waylandCapturer_->originX();
#endif
#ifdef Q_OS_WIN
        // 选了副显示器时捕获区原点不是 (0,0)：鼠标注入按「原点+相对坐标」换算，
        // 否则点击会整体落在主屏上。
        if (winCurrentIndex_ >= 0 && winCurrentIndex_ < winOutputs_.size())
            return winOutputs_[winCurrentIndex_].x;
#endif
        return 0;
    }
    int originY() const {
#ifdef Q_OS_LINUX
        if (useX11_ && x11Capturer_)
            return x11Capturer_->originY();
        if (useWayland_ && waylandCapturer_)
            return waylandCapturer_->originY();
#endif
#ifdef Q_OS_WIN
        if (winCurrentIndex_ >= 0 && winCurrentIndex_ < winOutputs_.size())
            return winOutputs_[winCurrentIndex_].y;
#endif
        return 0;
    }
    static bool changeDisplayResolution(int w, int h);
    static QJsonArray enumerateSupportedResolutions();

    // 多屏切换（Linux X11）：枚举输出 / 切换捕获目标 / 当前输出索引
    QJsonArray enumerateOutputs() const;
    // 热插拔感知：重新枚举并保持/回退当前选择。
    // 返回值 = "当前输出可用"（可能什么都没变）；"几何是否变了/是否需要重建"必须
    // 通过 geometryChanged 取——旧实现把这两件事混在一个 bool 里，几何未变也返回
    // true，调用方若把 true 当"变了"就会白重建一次编码器。
    bool refreshOutputs(bool* geometryChanged = nullptr);
    bool switchOutput(int index);
    int currentOutputIndex() const;

#ifdef Q_OS_WIN
    // 外部改分辨率/插拔显示器后，捕获区几何是否已与系统不一致（GDI 捕获区是建实例时
    // 定死的，屏幕变了 BitBlt 不会报错）。变了就按新几何重建并返回 true，调用方负责
    // 重报 screen_info 与重建编码器。
    bool pollOutputGeometry();
#endif

#ifdef Q_OS_WIN
    // Windows 多屏输出描述（screencapturer_win.cpp 维护；public 供文件级枚举函数使用）
    struct WinOutput {
        QString name;
        int x = 0, y = 0, w = 0, h = 0;
        bool primary = false;
    };
#endif

signals:
    void frameCaptured(const QImage& frame);
    void screenLocked(bool locked);

private slots:
    void captureFrame();

private:
    friend class CaptureGrabWorker;
    void cleanupPlatform();
    // 平台实现的采集主体（原 captureFrame）。异步模式下由 onGrabDone() 在主线程
    // 调用，抓取结果已由 workerGrab() 放在 grabFrame_ 等成员里。
    void captureFrameBody();
    // 只做"纯抓取"，在采集线程执行（Windows 限定；其余平台不建线程）。
    void workerGrab();
    void ensureGrabThread();
    void teardownGrabThread();
    // 采集线程完成的回调（主线程）：跑决策主体 + 计时 + 平均耗时。
    void onCaptureDone();
    // 统一出帧入口：各平台分支一律经由它 emit，附带"画面是否真实变化"的累计。
    void emitCapturedFrame(const QImage& frame);
    // 已算过 quickFrameChecksum 的路径用这个重载：同一帧不重复做全帧采样扫描
    //（1080p 一次约 36 行 × 7.7KB，变化帧会被去重逻辑和这里各算一遍）。
    void emitCapturedFrame(const QImage& frame, quint16 knownChecksum);

    QTimer* captureTimer_ = nullptr;
    // [CAP-ASYNC] 抓屏线程（仅 Windows 启用；其余平台 grabThread_ 恒空 ⇒ 同步路径，
    // 行为与改动前完全一致）。把 BitBlt/整帧拷贝从 GUI 线程挪走：慢读回机器单帧
    // 320~350ms 期间主线程不再冻结，输入注入/心跳/兜底定时器不再被推迟。
    QThread* grabThread_ = nullptr;
    CaptureGrabWorker* grabWorker_ = nullptr;
    QAtomicInt grabBusy_{ 0 };       // 1=已提交、尚未处理完（背压丢帧用）
    QAtomicInt grabSkipTicks_{ 0 };  // 因上一帧未完成而跳过的 tick 数（诊断）
    // worker → 主线程的抓取结果。可见性由 grabBusy_ 的 release/acquire 保证：
    // worker 写完后 emit done()（queued），主线程在 onCaptureDone 里 acquire 后读。
    QImage grabFrame_;
    bool grabOk_ = false;
    bool grabUpdated_ = false;
    bool grabBlack_ = false;          // worker 判定本帧全黑（是否切换由主线程决定）
    bool grabFmtUnsupported_ = false;
    bool grabGdiHasContent_ = false;
    // true = 本次 captureFrameBody() 的帧已由采集线程抓好（取 grabFrame_，不再本地抓屏）
    bool useGrabResult_ = false;
    qint64 grabSubmitMs_ = 0;
    // 抓取资源（DXGI/GDI 实例、winOutputs_）跨线程访问互斥：workerGrab 抓取期间持有，
    // 主线程 start/stop/refreshOutputs/切路径时也持有（低频，等一帧可接受）。
    // 递归：start() → cleanupPlatform() → switchToGdiCapture() 等会嵌套加锁。
    QMutex captureResMutex_{ QMutex::Recursive };
    QAtomicInt avgCaptureMs_{ 0 };
    // [IDLE-WAKE] 静止降频后的即时唤醒：定时器可能还有 ~3s 才到点，这里插一次
    // 0ms 抓帧，让"闲置后第一次点击/按键"的反馈不必等满一个 idle 周期。
    QTimer* wakeTimer_ = nullptr;
    void requestImmediateCapture();
    QScreen* screen_ = nullptr;
    int fps_ = 30;

    // [KFR] 内容真实变化计数（见 contentChangeCount() 注释）。
    quint64 contentChangeCount_ = 0;
    quint16 lastEmittedChecksum_ = 0;
    bool lastEmittedValid_ = false;

    quint16 lastFrameChecksum_ = 0;
    bool forceSendNextFrame_ = false; // 兼容保留（部分平台按 bool 判定）
    int forceFrameCount_ = 0;         // 强制连续推送帧数（resume 后预热、切屏后重置校验）
    bool screenLocked_ = false;
    int dxgiRetryCount_ = 0;
    // DXGI 成功取帧但内容全黑时的兜底（详见 captureFrame）：连续黑帧计数 +
    // 交叉验证后的冷却，避免在本来就黑的桌面上反复创建 GDI 探测实例。
    int dxgiBlackCount_ = 0;
    int dxgiBlackProbeCooldown_ = 0;

#ifdef Q_OS_WIN
    PlatformCapturer* gdiCapturer_ = nullptr;
    bool useGDI_ = false;
    // [IDLE-DIFF] 慢读回机器的静止判定基准帧（QImage 隐式共享，赋值不拷贝像素）。
    // 详见 screencapturer_win.cpp 的 frameChangeIsSmall()。
    QImage lastFrameDiff_;
    // GDI 采集器存在但 BitBlt 连续失败的计数与已重建次数（详见 captureFrame 的
    // gdiAttempted 分支）：桌面切换以外的原因（分辨率变更、驱动重置、DIB 尺寸过期）
    // 也会让 BitBlt 一直失败，必须重建才能自愈；但锁屏期间失败是常态，所以重建次数
    // 封顶，取到帧即清零。
    int gdiFailStreak_ = 0;
    int gdiRebuilds_ = 0;

    // Windows 多屏选择状态（screencapturer_win.cpp 维护）
    QList<WinOutput> winOutputs_;
    int winCurrentIndex_ = -1;
    bool winApplyOutput(int index);   // 应用到指定输出（GDI 区域 / DXGI 输出号）并重建捕获器
    // 运行期补建 GDI 捕获器（DXGI 失败/黑帧时用），区域取当前选中输出。
    bool switchToGdiCapture();
#endif

// [ODR] 不能用 _WIN32_WINNT 作守卫：rdpserver 等包含本头的 TU 与 ScreenCapturer
// 目标的 _WIN32_WINNT 定义不同（0x0A00 vs 未定义/0x0601），会编译出两种类布局
// → ODR 违规、随机崩溃。成员声明无代价，平台可用性判断留给 .cpp 运行期。
#if defined(Q_OS_WIN)
    PlatformCapturer* dxgiCapturer_ = nullptr;
    bool useDXGI_ = false;
#endif

    int idleCount_ = 0;
    // 静止画面的降频/恢复：连续静止时降到 kIdleIntervalMs（4fps，兼顾交互反馈
    // 与 CPU），一旦画面有变化立刻恢复全帧率。
    // 注意：原实现在降频分支的守卫里加了 "interval() < 250"，导致降到 250ms 后
    // 该条件恒假、再也无法重新设置；而恢复分支同样被这层守卫挡住，使静止后
    // 即使画面变化也长期停在 4fps。改为显式 enter/leave 两个方法消除该歧义。
    static constexpr int kIdleIntervalMs = 250;
    // [SLOW-READBACK-PACE] 慢读回自适应节流：VMware SVGA 等无加速 VM 上，单帧
    // GDI 读屏（BitBlt 从帧缓冲读回）实测可达 300~400ms。固定 33ms 定时器会让
    // 采集回调背靠背连跑，单核被占满（XP 单核实测 50%~95%）。paceIntervalMs_
    // 由实测单帧耗时推导（≈2×capAvg，duty≈33%）。
    qint64 paceIntervalMs_ = 0;
    double paceFactor_ = 2.0;
    // 上一帧抓取**结束**的时刻与单帧实测耗时（ms）。nudgeCapture(true) 用它做
    // 最小间隔判定：抓帧是同步阻塞的，间隔只能从"上一次结束"算起才等于真实占空比。
    qint64 lastCaptureEndMs_ = 0;
    qint64 lastCaptureCostMs_ = 0;
    void captureOnce();      // 抓一帧并记录耗时（两个定时器入口都走这里）
    bool idleThrottled_ = false;
    // [IDLE-PACE] 慢读回机器上 pace 本身就有数百 ms，旧的 max(idle 250ms, pace)
    // 里 idle 分支永远选不上：静止桌面也一直按 duty≈33% 跑（XP 单核实测恒定
    // 33.5% CPU、0.56fps）。静止时把 pace 再放大 kIdlePaceFactor 倍（≈3s 一帧，
    // duty≈11%）；交互延迟由 forceNextFrame() 的即时唤醒兜住（输入到达后立刻
    // 抓一帧并恢复正常 pace），所以不需要为响应速度保留高频空转。
    static constexpr int kIdlePaceFactor = 4;
    // 定时器间隔上界：极端读回（capAvg>2s）时 pace*4 会把交互反馈拖到十几秒
    static constexpr int kMaxIntervalMs = 5000;
    void applyCaptureInterval() {
        qint64 pace = paceIntervalMs_;
        if (idleThrottled_)
            pace = pace > 0 ? pace * kIdlePaceFactor : kIdleIntervalMs;
        int target = (fps_ > 0) ? (1000 / fps_) : 33;
        if (pace > target)
            target = static_cast<int>(qMin<qint64>(pace, kMaxIntervalMs));
        if (captureTimer_->interval() != target)
            captureTimer_->setInterval(target);
    }
    void enterIdleThrottle() { idleThrottled_ = true; applyCaptureInterval(); }
    void leaveIdleThrottle() { idleThrottled_ = false; applyCaptureInterval(); }
    // 静止判定阈值（连续多少帧校验和不变即降频）。原 fps_*2 在慢读回（单帧数百
    // ms）时要几十秒才触发；这里收敛到 10~15 帧，静止时能及时降频。
    int idleThresholdFrames() const { return qMax(10, (fps_ > 0 ? fps_ / 2 : 15)); }

private:
    // [PUMP-REMOVED] 补帧泵（schedulePumpFlush/pumpFlushTick）已于 2026-10-29 删除：
    // 2026-10-09 起整段被 return 短路，调用点全是空转。孤立变化帧的顶出由 helper
    // 的 [KFR] + kfFallbackTimer 负责。

#ifdef Q_OS_LINUX
    PlatformCapturer* x11Capturer_ = nullptr;
    bool useX11_ = false;
    PlatformCapturer* waylandCapturer_ = nullptr;
    bool useWayland_ = false;
    int captureFailCount_ = 0;
    // captureFrame 防重入：Wayland 流 ERROR 重建会在本定时器槽内跑嵌套事件
    // 循环，同一 timer 会再次派发进来（此时平台捕获器正处于 teardown 半状态）。
    bool inCaptureFrame_ = false;

    // 真实锁屏状态查询（logind LockedHint 优先，gnome-screensaver 兜底），
    // 带 2s 缓存。返回 1=已锁屏 0=未锁屏 -1=查询失败（调用方回退旧启发式）。
    // 背景：Wayland 会话下服务端若回落 XWayland，采集会持续失败，旧的
    // "失败>=5 次 ⇒ 锁屏"启发式会误报锁屏（前端弹出假解锁界面）。
    int realLockState();
    // "采集失败/黑帧"时是否允许声明锁屏：查询到未锁屏 → 返回 false 并纠正
    // 既有状态；已锁屏或查询失败 → 返回 true 维持旧行为。
    bool shouldDeclareLocked();
    int cachedLockState_ = -1;
    QElapsedTimer lockQueryTimer_;
#endif
};

#ifdef Q_OS_LINUX
// 量一次"子窗口画完了，根缓冲区什么时候才真的有这块像素"，单位 ms；-1 = 无从判断
// （连不上 X，或根窗口读不出来，如 WSLg 的 rootless XWayland）。
// 用途：无垂直同步/RandR 刷新时序的屏幕（Xvfb/Xvnc/无 EDID 的 Xorg）上，xfwm4 的合成器
// 会退化成 ~1Hz 定时器，采集端从根窗口抓到的像素本身就是旧的（.9 实测：桌面内容 6.33Hz
// 变化、根窗口 1.00Hz → 端到端只有 3.1fps）。接真实显示器的会话按刷新率合成，只会量出
// 十几毫秒 —— 调用方据此决定**不动**用户自己的桌面设置。
int measureCompositorRepaintLagMs(const char* display);
#endif

#endif
