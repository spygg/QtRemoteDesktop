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
    qInfo() << "ScreenCapturer: suspend (timer active=" << captureTimer_->isActive() << ")";
    captureTimer_->stop();
}

void ScreenCapturer::resume()
{
    qInfo() << "ScreenCapturer: resume (timer active=" << captureTimer_->isActive()
            << "interval=" << captureTimer_->interval() << ")";
    // 无论 timer 是否已激活都恢复目标帧率：idle 降频后 resume 若不重置，
    // 会一直以 1s 周期空转，客户端连接后画面响应极慢（远程桌面延迟主因之一）。
    if (captureTimer_->interval() != 1000 / fps_)
        captureTimer_->setInterval(1000 / fps_);
    if (!captureTimer_->isActive()) {
        // 强制连续推送若干帧：桌面静止时首帧会被 checksum 去重丢弃，导致客户端
        // 连接后永远收不到画面（黑屏）。连续预热帧还让编码器（含 MPP 失败回退
        // 软编）能快速完成初始化判定并输出首帧。
        forceSendNextFrame_ = true;
        forceFrameCount_ = 10;
        lastFrameChecksum_ = 0;
        idleCount_ = 0;
        captureTimer_->start(1000 / fps_);
    }
}

void ScreenCapturer::forceNextFrame()
{
    // 用户输入注入后，即使画面校验和未变化（如仅光标移动）也强制推送下一帧。
    // X11 光标是 overlay，不产生 XDamage；键盘/鼠标输入多不改变帧内容时会被
    // checksum 去重，导致远端画面长期不刷新。
    // 同时恢复全速帧率：idle 降频到 1s 周期时若不恢复，交互反馈仍要等 1 秒。
    forceSendNextFrame_ = true;
    forceFrameCount_ = 2;
    lastFrameChecksum_ = 0;
    idleCount_ = 0;
    if (captureTimer_->isActive() && captureTimer_->interval() != 1000 / fps_)
        captureTimer_->setInterval(1000 / fps_);
}

void ScreenCapturer::setFps(int fps)
{
    if (fps < 1) return;
    fps_ = fps;
    if (captureTimer_->isActive())
        captureTimer_->setInterval(1000 / fps_);
}
