// server/screen_capturer.cpp
#include "screencapturer.h"
#include <QColor>
#include <QDebug>
#include <QPixmap>

ScreenCapturer::ScreenCapturer(QObject* parent)
    : QObject(parent)
    , captureTimer_(new QTimer(this))
    , screen_(QGuiApplication::primaryScreen())
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
    cleanupPlatform();
}

void ScreenCapturer::suspend()
{
    captureTimer_->stop();
}

void ScreenCapturer::resume()
{
    if (!captureTimer_->isActive())
        captureTimer_->start(1000 / fps_);
}
