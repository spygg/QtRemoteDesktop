// server/screen_capturer.h
#ifndef SCREEN_CAPTURER_H
#define SCREEN_CAPTURER_H

#include <QGuiApplication>
#include <QImage>
#include <QObject>
#include <QScreen>
#include <QTimer>

// 快速帧校验和：每隔 N 行采样一行做 CRC，大幅减少计算量
inline quint16 quickFrameChecksum(const QImage& frame)
{
    const uchar* bits = frame.constBits();
    int stride = frame.bytesPerLine();
    int h = frame.height();
    int step = qMax(1, h / 32);
    quint16 result = 0;
    for (int y = 0; y < h; y += step) {
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
        result ^= qChecksum(QByteArrayView(bits + y * stride, stride));
#else
        result ^= qChecksum(reinterpret_cast<const char*>(bits + y * stride), static_cast<uint>(stride));
#endif
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
};

class ScreenCapturer : public QObject {
    Q_OBJECT

public:
    explicit ScreenCapturer(QObject* parent = nullptr);
    ~ScreenCapturer();

    bool start(int fps);
    void stop();
    void suspend();  // 暂停捕获（不销毁平台捕获器）
    void resume();   // 恢复捕获

    int width() const {
#ifdef Q_OS_LINUX
        if (useX11_ && x11Capturer_)
            return x11Capturer_->width();
#endif
        return screen_ ? screen_->size().width() : 0;
    }
    int height() const {
#ifdef Q_OS_LINUX
        if (useX11_ && x11Capturer_)
            return x11Capturer_->height();
#endif
        return screen_ ? screen_->size().height() : 0;
    }

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
    bool screenLocked_ = false;
    int dxgiRetryCount_ = 0;

#ifdef Q_OS_WIN
    PlatformCapturer* gdiCapturer_ = nullptr;
    bool useGDI_ = false;
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
    int captureFailCount_ = 0;
#endif
};

#endif
