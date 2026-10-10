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
    flushPumpTimer_ = new QTimer(this);
    flushPumpTimer_->setSingleShot(true);
    connect(flushPumpTimer_, &QTimer::timeout, this, &ScreenCapturer::pumpFlushTick);
}

void ScreenCapturer::schedulePumpFlush()
{
    // [PUMP-DISABLED 2026-10-09] 243 实测：补帧泵在桌面有持续微变化（光标闪烁/
    // GNOME 面板刷新等）时形成自持循环——journal 显示 5 分钟自激发 1441 次
    // (~4.8/s)，采集被迫以 ~9.5fps 空转发射 1920x1200 帧，打字 g2g 延迟中位
    // 1.73s、最差 4.47s。泵原本只为解决"孤立变化帧被编码器 1 帧流水线吞掉"，
    // 代价远大于收益，先整体禁用（代码保留，pumpFlushEnabled_ 由模式切换控制）。
    return;
    if (!pumpFlushEnabled_ || pumpInProgress_)
        return;
    if (flushPumpTimer_ && !flushPumpTimer_->isActive())
        flushPumpTimer_->start(kFlushPumpDelayMs);
}

void ScreenCapturer::pumpFlushTick()
{
    if (!captureTimer_->isActive())
        return;
    pumpInProgress_ = true;
    // 强制推一帧（允许与上一帧内容相同），把编码器流水线里被 1 帧延迟吞住的
    // 真实变化帧顶出来。pumpInProgress_ 防止补帧再次调度泵（否则静止时无限
    // 80ms 空转）。
    forceFrameCount_ = qMax(forceFrameCount_, 1);
    qInfo() << "FLUSH-PUMP fire (forceFrameCount->1)";
    captureFrame();
    pumpInProgress_ = false;
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
    // 会一直以 4fps 周期空转，客户端连接后画面响应极慢（远程桌面延迟主因之一）。
    leaveIdleThrottle();
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
    if (captureTimer_->isActive())
        leaveIdleThrottle();
}

void ScreenCapturer::emitCapturedFrame(const QImage& frame)
{
    // [KFR] 累计"画面真实变化"次数。这里用**独立**的 lastEmittedChecksum_，
    // 不复用去重用的 lastFrameChecksum_（后者会被 forceNextFrame()/resume()
    // 重置为 0，若拿来比较会把"强制抓帧"误判成"画面变了"）。
    // 只有出帧之间校验和真的不同才算一次内容变化；静止桌面下 PLI 反复强制抓帧
    // 得到的都是同一幅画面，校验和不变 → 计数不动 → 上层可放心复用缓存关键帧。
    const quint16 cs = quickFrameChecksum(frame);
    if (lastEmittedValid_ && cs != lastEmittedChecksum_)
        contentChangeCount_++;
    lastEmittedChecksum_ = cs;
    lastEmittedValid_ = true;
    emit frameCaptured(frame);
}

void ScreenCapturer::setFps(int fps)
{
    if (fps < 1) return;
    // [H22] 上界保护：1000/fps 整数除法在 fps>1000 时得 0ms 定时器 → 忙转；
    // 客户端可发任意 fps 触发定时器忙转 + 编码器重建风暴（可控 DoS）
    if (fps > 60) fps = 60;
    fps_ = fps;
    if (captureTimer_->isActive())
        captureTimer_->setInterval(1000 / fps_);
}
