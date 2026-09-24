// server/screen_capturer.h
#ifndef SCREEN_CAPTURER_H
#define SCREEN_CAPTURER_H

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

#if defined(Q_OS_WIN) && (_WIN32_WINNT >= _WIN32_WINNT_WIN8)
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

    int width() const {
#ifdef Q_OS_LINUX
        if (useX11_ && x11Capturer_)
            return x11Capturer_->width();
        if (useWayland_ && waylandCapturer_)
            return waylandCapturer_->width();
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

    QTimer* captureTimer_ = nullptr;
    QScreen* screen_ = nullptr;
    int fps_ = 30;

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

#if defined(Q_OS_WIN) && (_WIN32_WINNT >= _WIN32_WINNT_WIN8)
    PlatformCapturer* dxgiCapturer_ = nullptr;
    bool useDXGI_ = false;
    bool initDXGI();
    void captureDXGI();
#endif

    int idleCount_ = 0;

#ifdef Q_OS_LINUX
    PlatformCapturer* x11Capturer_ = nullptr;
    bool useX11_ = false;
    PlatformCapturer* waylandCapturer_ = nullptr;
    bool useWayland_ = false;
    int captureFailCount_ = 0;
#endif
};

#endif
