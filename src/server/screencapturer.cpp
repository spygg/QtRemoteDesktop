// server/screen_capturer.cpp
#include "screencapturer.h"
#include <QColor>
#include <QDebug>
#include <QPixmap>

// sudo apt install libx11-dev libxtst-dev libxdamage-dev libxcomposite-dev libxrender-dev

ScreenCapturer::ScreenCapturer(QObject* parent)
    : QObject(parent)
    , captureTimer_(new QTimer(this))
    , screen_(QGuiApplication::primaryScreen())
#ifdef Q_OS_WIN
    , dxgiCapturer_(nullptr)
    , useDXGI_(false)
    , gdiCapturer_(nullptr)
    , useGDI_(false)
#endif
{
    connect(captureTimer_, &QTimer::timeout, this, &ScreenCapturer::captureFrame);
}

ScreenCapturer::~ScreenCapturer()
{
    stop();
}

void ScreenCapturer::stop()
{
    captureTimer_->stop();

#ifdef Q_OS_WIN
    delete gdiCapturer_;
    gdiCapturer_ = nullptr;
    useGDI_ = false;
#if (_WIN32_WINNT >= _WIN32_WINNT_WIN8)
    delete dxgiCapturer_;
    dxgiCapturer_ = nullptr;
    useDXGI_ = false;
#endif
#endif

#ifdef Q_OS_LINUX
    delete x11Capturer_;
    x11Capturer_ = nullptr;
    useX11_ = false;
#endif
}
