// server/screen_capturer.h
#ifndef SCREEN_CAPTURER_H
#define SCREEN_CAPTURER_H

#include <QElapsedTimer>
#include <QGuiApplication>
#include <QImage>
#include <QJsonArray>
#include <QList>
#include <QObject>
#include <QPoint>
#include <QScreen>
#include <QString>
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
        if (winCurrentIndex_ >= 0 && winCurrentIndex_ < winOutputs_.size()
            && winOutputs_[winCurrentIndex_].h > 0)
            return winOutputs_[winCurrentIndex_].h;
#endif
        return screen_ ? screen_->size().height() : 0;
    }

    static bool changeDisplayResolution(int w, int h);
    static QJsonArray enumerateSupportedResolutions();

    // 多屏切换（Linux X11）：枚举输出 / 切换捕获目标 / 当前输出索引
    QJsonArray enumerateOutputs() const;
    bool refreshOutputs();   // 热插拔感知：重新枚举并保持/回退当前选择
    bool switchOutput(int index);
    int currentOutputIndex() const;

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
    void cleanupPlatform();
    // 统一出帧入口：各平台分支一律经由它 emit，附带"画面是否真实变化"的累计。
    void emitCapturedFrame(const QImage& frame);

    QTimer* captureTimer_ = nullptr;
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

#ifdef Q_OS_WIN
    PlatformCapturer* gdiCapturer_ = nullptr;
    bool useGDI_ = false;

    // Windows 多屏选择状态（screencapturer_win.cpp 维护）
    QList<WinOutput> winOutputs_;
    int winCurrentIndex_ = -1;
    bool winApplyOutput(int index);   // 应用到指定输出（GDI 区域 / DXGI 输出号）并重建捕获器
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
    void enterIdleThrottle() {
        if (captureTimer_->interval() != kIdleIntervalMs)
            captureTimer_->setInterval(kIdleIntervalMs);
    }
    void leaveIdleThrottle() {
        const int target = (fps_ > 0) ? (1000 / fps_) : 33;
        if (captureTimer_->interval() != target)
            captureTimer_->setInterval(target);
    }

public:
    // [FLUSH-PUMP] 视频模式下启用：孤立的单次画面变化（弹窗关闭、单击、caret 闪烁）
    // 只产生 1 帧输入，而 openh264 等 1 帧流水线延迟的编码器需要 N+1 帧输入才输出
    // 第 N 帧 —— 该变化帧会被吞在编码器里，直到下次画面变化或 PLI 泵（可达数秒）
    // 才被顶出来，表现为"静止片刻后第一次交互延迟 2~3 秒"。启用后每次发射变化帧
    // 都安排一次 80ms 后的强制补帧，把流水线里的帧顶出来。图片模式无此问题（帧内
    // 编码），不必启用。
    void setFrameFlushPump(bool on) { pumpFlushEnabled_ = on; }

private:
    void schedulePumpFlush();
    void pumpFlushTick();

    QTimer* flushPumpTimer_ = nullptr;
    bool pumpFlushEnabled_ = false;
    bool pumpInProgress_ = false;
    static constexpr int kFlushPumpDelayMs = 80;

#ifdef Q_OS_LINUX
    PlatformCapturer* x11Capturer_ = nullptr;
    bool useX11_ = false;
    PlatformCapturer* waylandCapturer_ = nullptr;
    bool useWayland_ = false;
    int captureFailCount_ = 0;

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

#endif
