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

private slots:
    void captureFrame();

private:
    QTimer* captureTimer_ = nullptr;
    QScreen* screen_ = nullptr;
    int fps_ = 30;

    QImage lastFrame_;

#ifdef Q_OS_WIN
    GdiCapturer* gdiCapturer_ = nullptr;
    bool useGDI_ = false;
#endif

#if defined(Q_OS_WIN) && (_WIN32_WINNT >= _WIN32_WINNT_WIN8)
    // DXGI 相关成员...
    DXGICapturer* dxgiCapturer_ = nullptr; // 原始指针，配合 RAII

    bool useDXGI_ = false;
    // Windows DXGI 优化捕获
    bool initDXGI();
    void captureDXGI();
#endif

#ifdef Q_OS_LINUX
    // Linux X11 优化捕获
    class X11Capturer* x11Capturer_ = nullptr;
    bool useX11_ = false;
#endif
};

#endif
