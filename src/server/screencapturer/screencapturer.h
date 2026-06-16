// server/screen_capturer.h
#ifndef SCREEN_CAPTURER_H
#define SCREEN_CAPTURER_H

#include <QGuiApplication>
#include <QImage>
#include <QObject>
#include <QScreen>
#include <QTimer>

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
};

class ScreenCapturer : public QObject {
    Q_OBJECT

public:
    explicit ScreenCapturer(QObject* parent = nullptr);
    ~ScreenCapturer();

    bool start(int fps);
    void stop();

    int width() const { return screen_->size().width(); }
    int height() const { return screen_->size().height(); }

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

#ifdef Q_OS_LINUX
    PlatformCapturer* x11Capturer_ = nullptr;
    bool useX11_ = false;
    int captureFailCount_ = 0;
#endif
};

#endif
