// server/screen_capturer.cpp
#include "screencapturer.h"
#include <QColor>
#include <QDateTime>
#include <QDebug>
#include <QPixmap>

ScreenCapturer::ScreenCapturer(QObject* parent)
    : QObject(parent)
    , captureTimer_(new QTimer(this))
    , screen_(QGuiApplication::primaryScreen())
{
    // 两个入口都记"抓帧结束/单帧耗时"：nudgeCapture(true) 的最小间隔判定要靠它，
    // 否则节流基准会退化成"上次请求时刻"，抓帧耗时本身被漏算。
    connect(captureTimer_, &QTimer::timeout, this, &ScreenCapturer::captureOnce);
    // [IDLE-WAKE] 0ms 单次定时器：不在输入处理函数里同步抓屏（慢机一帧数百 ms，
    // 会把输入队列堵在后面），而是让事件循环尽快插入一次抓帧。
    wakeTimer_ = new QTimer(this);
    wakeTimer_->setSingleShot(true);
    wakeTimer_->setInterval(0);
    connect(wakeTimer_, &QTimer::timeout, this, [this]() {
        if (captureTimer_->isActive())
            captureOnce();
    });
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
    teardownGrabThread();
}

// 计一帧的采集耗时进滑动平均（指数平均，α=1/4）。慢机自适应配置用它分级。
static void noteCaptureCost(QAtomicInt& avg, qint64 cost)
{
    if (cost < 0)
        cost = 0;
    if (cost > 60000)
        cost = 60000;
    const int prev = avg.loadAcquire();
    avg.storeRelease(prev > 0 ? (prev * 3 + static_cast<int>(cost)) / 4
                              : static_cast<int>(cost));
}

void CaptureGrabWorker::grab()
{
    owner_->workerGrab();
    emit done();
}

#ifndef Q_OS_WIN
// 非 Windows 平台不启用采集线程（XGetImage/grabWindow 各有线程约束，且读回通常
// 只有个位数 ms，收益远小于风险）。给 workerGrab 一个空实现避免链接缺失——
// 这些平台上 grabWorker_ 恒为空，门面永远走同步的 captureFrameBody()。
void ScreenCapturer::workerGrab() {}
#endif

void ScreenCapturer::ensureGrabThread()
{
#ifdef Q_OS_WIN
    if (grabThread_)
        return;
    // 逃生阀：QTRD_CAPTURE_SYNC=1 时退回"采集在 GUI 线程"的老路径，便于线上排障。
    if (qgetenv("QTRD_CAPTURE_SYNC") == QByteArrayLiteral("1")) {
        qInfo() << "ScreenCapturer: async capture disabled by QTRD_CAPTURE_SYNC";
        return;
    }
    grabThread_ = new QThread(this);
    grabThread_->setObjectName(QStringLiteral("qtrd-capture"));
    grabWorker_ = new CaptureGrabWorker(this);
    grabWorker_->moveToThread(grabThread_);
    connect(grabWorker_, &CaptureGrabWorker::done,
            this, &ScreenCapturer::onCaptureDone, Qt::QueuedConnection);
    grabThread_->start(QThread::NormalPriority);
    qInfo() << "ScreenCapturer: async capture thread started";
#endif
}

void ScreenCapturer::teardownGrabThread()
{
    if (!grabThread_)
        return;
    // 等最后一次抓取返回（最多一帧耗时，慢机 ~350ms），否则 worker 可能在对象析构后
    // 继续访问 dxgiCapturer_/gdiCapturer_。
    for (int i = 0; i < 300 && grabBusy_.loadAcquire() != 0; ++i)
        QThread::msleep(10);
    grabThread_->quit();
    if (!grabThread_->wait(3000))
        qWarning() << "ScreenCapturer: capture thread did not stop in 3s";
    delete grabWorker_;
    grabWorker_ = nullptr;
    delete grabThread_;
    grabThread_ = nullptr;
}

void ScreenCapturer::captureFrame()
{
    // 同步路径（非 Windows，或线程未启用，或当前无 DXGI/GDI 实例可离线抓取）：
    // 与改动前完全一致。Qt 的 screen_->grabWindow() 只能在 GUI 线程调用，所以
    // 兜底路径也必须留在这里。
#ifdef Q_OS_WIN
    bool canGrabOffThread = (useGDI_ && gdiCapturer_);
#ifdef QTRD_HAVE_DXGI_DUP
    canGrabOffThread = canGrabOffThread || (useDXGI_ && dxgiCapturer_);
#endif
#else
    const bool canGrabOffThread = false;   // 其余平台不启用采集线程
#endif
    if (!grabWorker_ || !canGrabOffThread) {
        captureFrameBody();
        return;
    }
    // 背压：上一帧还在 worker 里跑就丢掉这个 tick。慢读回机器上必然发生（pace 已经
    // 按 capAvg 放大，正常不会持续丢帧），比排队堆积好——排队会让延迟无界增长。
    if (grabBusy_.loadAcquire() != 0) {
        grabSkipTicks_.ref();
        return;
    }
    grabBusy_.storeRelease(1);
    grabSubmitMs_ = QDateTime::currentMSecsSinceEpoch();
    QMetaObject::invokeMethod(grabWorker_, "grab", Qt::QueuedConnection);
}

void ScreenCapturer::onCaptureDone()
{
    // 抓取结果已就绪：在主线程跑原来的决策主体（checksum/去重/emit/idle/重建）。
    // body 里不再真正抓屏，直接取 grabFrame_/grabOk_ 等成员。
    useGrabResult_ = true;
    captureFrameBody();
    useGrabResult_ = false;
    const qint64 end = QDateTime::currentMSecsSinceEpoch();
    lastCaptureEndMs_ = end;
    lastCaptureCostMs_ = end - grabSubmitMs_;
    noteCaptureCost(avgCaptureMs_, lastCaptureCostMs_);
    grabBusy_.storeRelease(0);
}

void ScreenCapturer::stop()
{
    captureTimer_->stop();
#ifdef Q_OS_LINUX
    // PipeWire 的帧生产在独立线程里，停 timer 只是不再取帧，拷贝仍在继续。
    if (waylandCapturer_)
        waylandCapturer_->setPaused(true);
#endif
    cleanupPlatform();
}

void ScreenCapturer::suspend()
{
    qInfo() << "ScreenCapturer: suspend (timer active=" << captureTimer_->isActive() << ")";
    captureTimer_->stop();
#ifdef Q_OS_LINUX
    // 无人观看时把 Wayland 的整帧拷贝降到 1fps（详见 WaylandCapturer::streamProcess）。
    if (waylandCapturer_)
        waylandCapturer_->setPaused(true);
#endif
}

void ScreenCapturer::resume()
{
    qInfo() << "ScreenCapturer: resume (timer active=" << captureTimer_->isActive()
            << "interval=" << captureTimer_->interval() << ")";
#ifdef Q_OS_LINUX
    if (waylandCapturer_)
        waylandCapturer_->setPaused(false);
#endif
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
        // 尊重慢读回节流：保留上次测得的 paceIntervalMs_，避免刚连上时又满速空转。
        applyCaptureInterval();
    }
}

void ScreenCapturer::forceNextFrame()
{
    // 用户输入注入后，即使画面校验和未变化（如仅光标移动）也强制推送下一帧。
    // X11 光标是 overlay，不产生 XDamage；键盘/鼠标输入多不改变帧内容时会被
    // checksum 去重，导致远端画面长期不刷新。
    // 同时恢复全速帧率：idle 降频到 ~3s 周期时若不恢复，交互反馈仍要等数秒。
    forceSendNextFrame_ = true;
    forceFrameCount_ = 2;
    lastFrameChecksum_ = 0;
    idleCount_ = 0;
    if (captureTimer_->isActive()) {
        const bool wasIdle = idleThrottled_;
        leaveIdleThrottle();
        // 只在"从静止唤醒"时插即时抓帧：非 idle 时下一次 tick 本来就在 pace 内，
        // 而鼠标移动会高频调用本函数，无条件唤醒等于恢复满速抓屏。
        if (wasIdle)
            requestImmediateCapture();
    }
}

void ScreenCapturer::requestImmediateCapture()
{
    if (wakeTimer_ && !wakeTimer_->isActive())
        wakeTimer_->start();
}

void ScreenCapturer::captureOnce()
{
    ensureGrabThread();
    if (grabWorker_) {
        // 异步：提交后立即返回，计时与平均耗时在 onCaptureDone() 里完成。
        captureFrame();
        return;
    }
    const qint64 t0 = QDateTime::currentMSecsSinceEpoch();
    captureFrameBody();
    lastCaptureEndMs_ = QDateTime::currentMSecsSinceEpoch();
    lastCaptureCostMs_ = lastCaptureEndMs_ - t0;
    noteCaptureCost(avgCaptureMs_, lastCaptureCostMs_);
}

void ScreenCapturer::nudgeCapture(bool paced)
{
    forceSendNextFrame_ = true;
    forceFrameCount_ = qMax(forceFrameCount_, 1);
    lastFrameChecksum_ = 0;
    idleCount_ = 0;
    leaveIdleThrottle();          // 从 idle 降频回到 pace（交互/喂帧时都要即时响应）
    // 定时器停着（无客户端 / 已 suspend）就只置标志，不越过闸门去抓屏
    if (!captureTimer_->isActive())
        return;
    // [N1] 喂帧泵每 80ms 拍一次，而无条件插即时抓帧会让抓屏背靠背连跑：单帧
    // 320ms 的机器上占空比从稳态 33% 飙到 ~90%，每次关键帧请求都是满核尖峰。
    // 这里加下限间隔：取 min(pace/2, 上帧实测耗时) —— 前者跟随节流倍数（快机
    // pace=0 时退化成"不节流"），后者保证慢机上补帧至少留出一个"单帧耗时"的
    // 空隙，占空比封顶 50%，且不随 capturePaceFactor 调大而变慢。
    // 输入注入路径（forceNextFrame）不传 paced，不受影响。
    if (paced && paceIntervalMs_ > 0 && lastCaptureEndMs_ > 0) {
        const qint64 minGap = qMin(paceIntervalMs_ / 2,
                                   qMax<qint64>(1, lastCaptureCostMs_));
        if (QDateTime::currentMSecsSinceEpoch() - lastCaptureEndMs_ < minGap)
            return;
    }
    requestImmediateCapture();
}

void ScreenCapturer::emitCapturedFrame(const QImage& frame)
{
    emitCapturedFrame(frame, quickFrameChecksum(frame));
}

void ScreenCapturer::emitCapturedFrame(const QImage& frame, quint16 cs)
{
    // [KFR] 累计"画面真实变化"次数。这里用**独立**的 lastEmittedChecksum_，
    // 不复用去重用的 lastFrameChecksum_（后者会被 forceNextFrame()/resume()
    // 重置为 0，若拿来比较会把"强制抓帧"误判成"画面变了"）。
    // 只有出帧之间校验和真的不同才算一次内容变化；静止桌面下 PLI 反复强制抓帧
    // 得到的都是同一幅画面，校验和不变 → 计数不动 → 上层可放心复用缓存关键帧。
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
    // 走统一入口：保留慢读回节流（pace）与静止降频（idle）状态。
    if (captureTimer_->isActive())
        applyCaptureInterval();
}
