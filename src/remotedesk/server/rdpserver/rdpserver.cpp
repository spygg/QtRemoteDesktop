// server/rdp_server.cpp
#include "rdpserver.h"
#include "authmanager.h"
#include "clipboardservice.h"
#include "filetransferservice.h"
#include "inputmanager.h"
#include "screencapturer.h"
#include "websocketserver.h"
#include "xdndmonitor.h"

#ifdef USE_FFMPEG
#include "videoencoder.h"
#include "idrfallbackpolicy.h"
#endif

#ifdef USE_WEBRTC
#include "webrtcsession.h"
#include <QSet>
#include <memory> // std::unique_ptr / std::move（勿依赖 Qt 头传递包含）
#endif

#include <QBuffer>
#include <QCoreApplication>
#include <QDir>
#include <QSaveFile>
#include <QScopedValueRollback>
#include <QStandardPaths>
#include <QTimer>
#include <QVector>
#include <algorithm>
#include <chrono>

// 高频输入事件（鼠标移动/点击/滚轮/按键）逐条打日志会刷爆日志文件，
// 只对 auth 等低频事件保留日志，便于排查连接/认证问题。
static bool isInputEventLogged(const QString& type)
{
    return type != "mousemove" && type != "mousedown" && type != "mouseup"
        && type != "wheel" && type != "keydown" && type != "keyup";
}

// 观看模式（只读）下必须丢弃的消息类型：所有会作用到被控端的操作。
//   · 鼠标/键盘/滚轮：直接输入注入；
//   · clipboard：服务端会把内容写进远端剪贴板并注入一次 Ctrl+V，等同一次粘贴操作；
//   · system_action：锁屏/注销/重启/关机；
//   · ime_cycle：切换远端输入法；
//   · file_upload_*/file_download：写入或读取远端文件，属于对被控端的操作。
// 不在此列的都是纯显示设置（config/set_output/set_resolution/request_keyframe、
// file_list）与 WebRTC 信令，观看模式下仍可正常使用。
static bool isRemoteControlType(const QString& type)
{
    return type == QLatin1String("mousemove") || type == QLatin1String("mousedown")
        || type == QLatin1String("mouseup") || type == QLatin1String("wheel")
        || type == QLatin1String("keydown") || type == QLatin1String("keyup")
        || type == QLatin1String("clipboard")
        || type == QLatin1String("system_action") || type == QLatin1String("ime_cycle")
        || type == QLatin1String("global_action")
        || type == QLatin1String("file_upload_start") || type == QLatin1String("file_upload_done")
        || type == QLatin1String("file_download");
}

// 剪贴板内容 → WS 消息。文本沿用 {text} 字段（兼容旧前端），
// 图片用 {mime:"image/png", data:base64}。
static QJsonObject clipboardPayload(const QString& mime, const QByteArray& data)
{
    QJsonObject msg;
    msg["type"] = "clipboard";
    if (mime == QLatin1String("image/png")) {
        msg["mime"] = mime;
        msg["data"] = QString::fromLatin1(data.toBase64());
    } else {
        msg["text"] = QString::fromUtf8(data);
    }
    return msg;
}

// 浏览器在按住某键期间会持续补发 repeat=true 的 keydown。服务端**不按平台分叉**，
// 统一把一次 repeat 翻译成"先松再按"（keyup + 原 keydown），每次 repeat 产出一个字符：
//
//   · Windows / macOS：SendInput / CGEvent 是合成注入，没有键盘驱动的 typematic
//     自动重复，丢弃 repeat 就等于"长按只出一个字符"。
//   · Linux：也不再依赖 X 服务器的原生自动重复。auto-repeat 是**显示级状态、跨进程
//     存活**：旧版本遗留的 XAutoRepeatOff、或用户/发行版默认的 `xset r off`，都会让
//     长按静默失效且没有任何报错。统一翻译后这条隐式依赖彻底断开，长按行为只由本
//     服务端决定，与 X 的 repeat 开关无关（`xset r off` 下长按同样正常）。
//
// 翻译放在 onInputReceived 入口（注入之前）而不是注入层：直连与 helper（服务模式）
// 两条路径都从这里分流，避免再次出现"只修了直连、helper 仍丢重复"的分叉。
// 实现见 onInputReceived 入口的 repeat 分支。

#include <QCursor>
#include <QDateTime>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcess>
#include <QStandardPaths>
#include <QNetworkInterface>
#include <QScreen>
#include <QSysInfo>
#include <QThread>
#include <QTimer>
#include <QPointer>
#include <QUrl>
#include <thread>
#include <QUrlQuery>
#include <QSslCertificate>
#include <string>

#include "systeminfo.h"

#ifdef _WIN32
#include <windows.h>
#include <wtsapi32.h>
#include <tlhelp32.h>
#endif

#if defined(Q_OS_LINUX) && !defined(Q_OS_ANDROID)
#include <QDBusConnection>
#include <QDBusMessage>
#include <QDBusPendingCallWatcher>
#include <QDBusPendingReply>
#include <QDBusUnixFileDescriptor>
#include <fcntl.h>
#include <unistd.h>
#include <pwd.h>
// X11 EWMH 直发（显示桌面等）：工程已通过 XdndMonitor PUBLIC 链接 X11
#include <X11/Xlib.h>
#include <X11/Xatom.h>
// X11 头会把 None 定义成 0L，与工程枚举（InhibitBackend::None 等）冲突
#ifdef None
#undef None
#endif
#endif

#ifdef Q_OS_MACOS
#include <QProcess>
#endif

// 被控端平台标识：随 screen_locked 下发给前端。前端据此决定「锁屏密码怎么送」——
// Windows 的锁屏 UI 在 winlogon 安全桌面，浏览器逐字发 keydown 到不了那里，
// 必须走 system_action(unlock_screen) 让 secure-input 在安全桌面上写登录框；
// Linux 同一 X 会话，逐字 XTEST 注入即可。前端原先两种都试不出来，就是这个差别。
static QString platformTag()
{
#if defined(Q_OS_WIN)
    return QStringLiteral("windows");
#elif defined(Q_OS_ANDROID)
    return QStringLiteral("android");
#elif defined(Q_OS_MACOS)
    return QStringLiteral("macos");
#else
    return QStringLiteral("linux");
#endif
}

// ==================== JpegCompressor ====================
JpegCompressor::JpegCompressor(QObject* parent)
    : QObject(parent)
{
    moveToThread(&thread_);
    connect(&thread_, &QThread::started, this, &JpegCompressor::processLoop);
}

JpegCompressor::~JpegCompressor()
{
    shutdown();
}

void JpegCompressor::shutdown()
{
    {
        QMutexLocker locker(&mutex_);
        abort_ = true;
        cond_.wakeAll();
    }
    thread_.quit();
    // 无限等待线程自然退出：processLoop 在 abort_ 置位 + wakeAll 后必然在
    // while(!abort_) 处 break 并返回，QThread::run() 随之结束。
    // 不能用 wait(3000)+terminate()：terminate() 走 TerminateThread 强杀线程，
    // 若线程正持锁或正执行 QMetaObject::activate，会留下锁死的互斥锁 / 半完成的
    // 信号发射，导致 JpegCompressor 析构完成后编码线程仍在 emit jpegCompressed ->
    // activate 访问已析构的 QObjectData（d_ptr=0）-> EXCEPTION_ACCESS_VIOLATION
    // （use-after-free，与本次崩溃现场完全一致）。
    thread_.wait();
}

void JpegCompressor::enqueue(const QImage& frame)
{
    // 深拷贝：编码线程与采集线程不再共享 QImage 底层数据。
    // QImage 隐式共享的引用计数是原子的，但两个线程对同一底层数据的
    // detach（写时复制）/释放时序在 Qt 5.7 下仍可能产生竞态，
    // 导致堆损坏并在后续 emit（QMetaObject::activate）处崩溃（本次现象）。
    //
    // 采集侧几乎总是交付一块新分配/已 detach 的 QImage（captureFrame 内已 copy），
    // 这种帧不必再拷一次；只有真正共享底层数据的帧才走深拷贝兜底。
    //
    // 拷贝必须放在锁外：持锁做 1080p≈8MB 的 copy 会把压缩线程的出队一起卡住
    //（与 VideoEncoder::encode 的 [V-2] 同理）。
    QImage owned = frame.isDetached() ? frame : frame.copy();
    QMutexLocker locker(&mutex_);
    if (queue_.size() >= kMaxQueueSize)
        queue_.dequeue();
    queue_.enqueue(owned);
    cond_.wakeOne();
}

void JpegCompressor::processLoop()
{
    while (!abort_) {
        QImage image;
        {
            QMutexLocker locker(&mutex_);
            while (queue_.isEmpty() && !abort_)
                cond_.wait(&mutex_);
            if (abort_)
                break;
            image = queue_.dequeue();
        }

        QByteArray jpegData;
        QBuffer buffer(&jpegData);
        buffer.open(QIODevice::WriteOnly);

        // JPEG 线程内先缩放再压缩，主线程只需入队（避免全帧缩放阻塞输入处理）。
        //
        // 这里只做 [10,100] 的取值保护，**不再按“编码耗时”二次夹取档位**。
        // 背景（2026-10-08 修复）：此前此处硬编码 effScale<=75、q<=70，而档位映射层
        // 给“高”档的取值是 scale=100 / q=80，于是用户点「高」被静默压成 75%/q70 ——
        // 画面先缩到 75% 再由浏览器放大回物理分辨率显示，必然发虚，即用户反馈的
        // “画质切到高还是很模糊”。性能权衡交给档位映射层承担（medium/low/verylow
        // 的档位值本身就低，且 verylow 另行钳制分辨率上限），而「高」档的语义就是
        // 画质优先、允许更大的单帧编码开销（队列只保留最新帧，不会累积延迟）。
        // 实测（2560x1600 屏）：高=2560x1600/869KB，中=1920x1200/394KB，
        // 极低=1280x800/92KB。
        const int effScale = qBound(10, scalePercent_.load(), 100);
        const int q = qBound(10, quality_.load(), 100);
        if (effScale < 100) {
            int sw = image.width() * effScale / 100;
            int sh = image.height() * effScale / 100;
            if (sw > 0 && sh > 0)
                // 高画质档用双线性（Smooth）保住细节，避免缩小后的锯齿/像素丢失；
                // 其余档仍用最近邻（Fast），不为低画质档白增单帧缩放耗时。
                // effScale==100 时不进入此分支，无缩放、无插值损失。
                image = image.scaled(sw, sh, Qt::KeepAspectRatio,
                                     q >= 75 ? Qt::SmoothTransformation
                                             : Qt::FastTransformation);
        }

        if (!image.save(&buffer, "JPEG", q)) {
            qWarning() << "JpegCompressor: failed to compress frame";
            continue;
        }
        buffer.close();

        // emit 前再检查一次：避免 shutdown 后仍发射信号
        if (abort_)
            break;
        emit jpegCompressed(jpegData);
    }
}
// ==================== RDPServer ====================

// [CLOCK] 时长/冷却判据统一用 rdpserver.h 的 monoNowMs()（单调时钟）。

RDPServer::RDPServer(QObject* parent)
    : QObject(parent)
    , useSsl_(true) // 默认启用 HTTPS/WSS（与头文件声明一致；构造列表会覆盖头文件默认值）
    , sslConfiguration_(nullptr)
{
    // IME 工作线程结果回传：工作线程 emit（auto 连接 → queued 投递到主线程执行）
    connect(this, &RDPServer::imeResultReady, this, &RDPServer::onImeResultReady);
    connect(this, &RDPServer::imeWorkerFinished, this, &RDPServer::onImeWorkerFinished);

    // 周期关键帧：只要有视频客户端（WebRTC/RTP 或 WS-MSE/WebCodecs）在线，
    // 每 2s 强制一帧 IDR。静态桌面下编码器不会再自然产 IDR，若不定时补帧，
    // 任何在 GOP 中途加入、或刚重置等帧门的客户端（videoStarted_）会永远
    // 等不到 IDR 而把所有 P 帧丢弃 → 画面完全冻结。
    webrtcKfTimer_ = new QTimer(this);
    webrtcKfTimer_->setInterval(2000);
    connect(webrtcKfTimer_, &QTimer::timeout, this, [this]() {
        // 注意：不能只看 webrtcSessions_。ffmpeg/MSE 模式下 WS 客户端同样需要
        // 关键帧，而它不在 webrtcSessions_ 里（旧逻辑导致 MSE 静态冻结）。
        const bool hasVideoClient =
#ifdef USE_WEBRTC
                !webrtcSessions_.isEmpty()
                ||
#endif
                (wsServer_ && wsServer_->hasVideoClients());
        if (!hasVideoClient)
            return;
#ifdef USE_FFMPEG
        // 仅在编码器尚未产出过任何关键帧时才周期驱动：持续强制抓帧让首个 IDR
        // 尽快出来（卡住则由 pumpKeyframe 内部冷却重建兜底）。一旦 hasIdr_=true
        // 就不再每 2s 空转全量抓取+编码——静态桌面下这纯粹是浪费；中途接入的
        // 客户端已由 onClientConnected 回放缓存关键帧并 pumpKeyframe()，
        // 以及前端 request_keyframe 兜底，不依赖这里的周期性强制抓帧。
        if (currentMode_ == ServerMode::Video && videoEncoder_ && !videoEncoder_->hasIdr())
            pumpKeyframe();
#endif
    });
#ifdef USE_FFMPEG
    // [ASYNC-RETIRE] 退役编码器回收定时器：切换模式/重建时只置 abort_ 唤醒编码线程，
    // 主线程不再同步等最长 8s。线程真正退出后（threadStopped()）才安全析构墓地对象，
    // 因此用单次定时器周期性重扫；仍有未退出的对象就继续重挂。
    retiredEncoderReapTimer_ = new QTimer(this);
    retiredEncoderReapTimer_->setSingleShot(true);
    retiredEncoderReapTimer_->setInterval(500);
    connect(retiredEncoderReapTimer_, &QTimer::timeout, this, [this]() {
        reapRetiredEncoders();
        if (!retiredEncoders_.empty()) {
            retiredEncoderReapTimer_->start();
#ifdef USE_FFMPEG
            if (videoRebuildPending_) {
                // 旧硬编线程一直不退出也不能无限等：超过上限就带着风险重试，
                // MPP 的"一次重试"策略会兜住这次失败，不会立刻永久退软编。
                static constexpr qint64 kMaxDeferredRebuildWaitMs = 5000;
                if (monoNowMs() - videoRebuildPendingSinceMs_ >= kMaxDeferredRebuildWaitMs) {
                    qWarning() << "Retired HW encoder still running after" << kMaxDeferredRebuildWaitMs
                               << "ms; attempting deferred rebuild anyway";
                    rebuildVideoEncoder(/*onlyIfIdle=*/false);
                }
            }
#endif
            return;
        }
#ifdef USE_FFMPEG
        if (videoRebuildPending_) {
            if (currentMode_ != ServerMode::Video || !screenCapturer_) {
                videoRebuildPending_ = false;
                return;
            }
            qInfo() << "Retired HW encoder stopped; resuming deferred encoder rebuild";
            rebuildVideoEncoder(/*onlyIfIdle=*/false);
        }
#endif
    });

    // [PLI-REUSE] IDR 请求兜底定时器：videoEncoder_->requestKeyframe() 只是置标志，
    // 部分软编（openh264 实测）不认 avcodec_flush_buffers + frame->pict_type，flush
    // 后仍输出 P 帧。1.2s 内没等到关键帧就重建编码器（重建后首帧必为 IDR）。
    // 单次触发，避免与 webrtcKfTimer_ 的周期逻辑互相干扰；重建另受 lastReinitMs_
    // 冷却保护（MPP 库反复 init 有固件崩溃历史）。到点后的判定在
    // idrfallbackpolicy.h（纯函数，可单测冷却重挂/等当前帧/硬上限强制重建等分支）。
    idrFallbackTimer_ = new QTimer(this);
    idrFallbackTimer_->setSingleShot(true);
    // 间隔取 1200ms：requestNewIdr 分支会同步把 lastReinitMs_ 设为当前时刻，触发时
    // 已过 1000ms 冷却（曾用 900ms：每次都落在冷却窗口内直接 return，兜底从不生效，
    // PLI 退化为每 3s 抓一帧 P 帧的死循环）。若触发时仍因其它路径刷新了 lastReinitMs_
    // 而处于冷却期内，回调会把定时器重挂到冷却结束——单次定时器不能一 return 了之。
    idrFallbackTimer_->setInterval(1200);
    connect(idrFallbackTimer_, &QTimer::timeout, this, [this]() {
        if (idrRequestMs_ <= 0)
            return;
        if (currentMode_ != ServerMode::Video || !videoEncoder_ || remoteVideoSourceAvailable()) {
            idrRequestMs_ = 0;
            return;
        }
        // 没有观众就不必为任何 IDR 请求重建：请求发出后客户端可能立刻断开/切图片，
        // 无人接收时的重建纯属白费（MPP 反复 init 有固件崩溃史）。与 webrtcKfTimer_
        // 的判据保持一致；新客户端接入时 onClientConnected 会重新 pumpKeyframe。
        const bool hasVideoClient =
#ifdef USE_WEBRTC
                !webrtcSessions_.isEmpty()
                ||
#endif
                (wsServer_ && wsServer_->hasVideoClients());
        if (!hasVideoClient) {
            idrRequestMs_ = 0;
            return;
        }
        const qint64 now = monoNowMs();
        const qint64 busy = videoEncoder_->busyForMs();
        // 判定逻辑抽在 idrfallbackpolicy.h（纯函数，便于覆盖冷却重挂/等当前帧/
        // 硬上限强制重建三条难以端到端复现的分支）。
        const IdrFallbackDecision d = decideIdrFallback(
            now, idrRequestMs_, lastReinitMs_, busy, videoEncoder_->lastFrameCostMs());
        if (d.action == IdrFallbackDecision::Retry) {
            // 冷却未到 / 编码线程在飞：重挂定时器，绝不能一 return 了之——
            // 单次定时器一旦停掉，idrRequestMs_ 就成了永远没人处理的悬空请求
            // （hasIdr_ 为真时周期定时器的兜底分支也不会介入），浏览器可能长期黑屏。
            idrFallbackTimer_->start(d.retryMs);
            return;
        }
        if (d.busyOverHardLimit)
            qWarning() << "[PLI-REUSE] encoder busy for" << busy
                       << "ms without IDR, forcing rebuild (possible hang)";
        // 空闲时走原子退役（retireIfIdle）：避免"判定空闲"与"线程开始下一帧"之间
        // 的竞态让主线程白等一整帧。Deferred 表示恰好有帧在飞，重挂定时器下一轮
        // 再按 busy 逻辑等待即可。超过硬上限时明知在飞也强制重建（线程可能已卡死）。
        if (!d.busyOverHardLimit) {
            const RebuildResult r = rebuildVideoEncoder(/*onlyIfIdle=*/true);
            if (r == RebuildResult::Deferred) {
                // 推迟：冷却时间戳不能更新（否则下一轮会白等一个冷却窗口），
                // idrRequestMs_ 也保持原值，让 busy 等待逻辑继续按请求年龄计时。
                idrFallbackTimer_->start(200);
                return;
            }
            if (r == RebuildResult::NotApplicable) {
                // 视频模式里却没有可重建的编码器/捕获源：这个 IDR 请求永远无法被
                // 满足，清掉以避免每 200ms 无限重试空转。
                idrRequestMs_ = 0;
                return;
            }
        }
        idrRequestMs_ = 0;
        lastReinitMs_ = now;
        qInfo() << "[PLI-REUSE] no IDR after requestKeyframe, rebuilding encoder to force IDR";
        if (d.busyOverHardLimit)
            rebuildVideoEncoder(/*onlyIfIdle=*/false);
        if (screenCapturer_)
            screenCapturer_->forceNextFrame();
    });
#endif
    webrtcKfTimer_->start();
}

RDPServer::~RDPServer()
{
    // WebRTC 会话是 QObject 子对象：若仅依赖基类析构，它们会在本类所有成员
    // （wsServer_ 等）销毁之后才 close()；close() 内 metaRTC 的回调仍可能
    // 同步触发 failed/closed → 槽函数访问已销毁成员。必须在成员有效时显式停止。
#ifdef USE_WEBRTC
    if (!webrtcSessions_.isEmpty()) {
        const QList<QString> ids = webrtcSessions_.keys();
        for (const QString& id : ids) {
            WebRtcSession* session = webrtcSessions_.take(id);
            if (session) {
                session->close();
                delete session;
            }
        }
        webrtcExcluded_.clear();
    }
#endif
    // 成员按声明的逆序析构：screenCapturer_ 在 jpegCompressor_ 之前声明，
    // 会自动先析构 jpegCompressor_。若不显式停机，工作线程尚在 emit jpegCompressed、
    // 或主线程 captureTimer 仍在向 jpegCompressor_->enqueue 时对象已被销毁，
    // 会造成 use-after-free（QMetaObject::activate 访问 d_ptr=0 崩溃）。
    // 故须逆数据流显式停机：先停采集（不再产生新帧/入队），排空已投递事件，再停编码线程。
    // [P2] 去抖落盘的配置先强制刷盘，避免退出丢配置
    if (configSaveTimer_ && configSaveTimer_->isActive()) {
        configSaveTimer_->stop();
        saveServerConfig(QString());
    }
    // 其余定时器 lambda 都捕获 this 并触碰 wsServer_/videoEncoder_：显式先停，
    // 不依赖"析构期间不跑事件循环"这一隐式前提（对象销毁顺序调整后即变隐患）。
    if (configSaveTimer_) configSaveTimer_->stop();
#ifdef USE_FFMPEG
    if (idrFallbackTimer_) idrFallbackTimer_->stop();
    if (retiredEncoderReapTimer_) retiredEncoderReapTimer_->stop();
#endif
    if (webrtcKfTimer_) webrtcKfTimer_->stop();
    if (cursorTimer_) cursorTimer_->stop();
    if (screenCapturer_)
        screenCapturer_->stop();
    if (jpegCompressor_)
        jpegCompressor_->shutdown(); // 幂等：线程已退出时立即返回

    // [B10] wait 超时后不能继续析构：QThread 对象销毁时若仍在运行会直接 abort，
    // 且 worker 可能正访问已销毁的成员。线程卡在大文件 IO/拖拽轮询时阻塞析构
    // 优于崩溃，故超时后无限等待。
    if (transferThread_ && transferThread_->isRunning()) {
        // 先解除下载线程的背压等待：主线程马上要阻塞在 wait()，事件循环不再处理
        // bytesWritten → 积压计数永不下降 → 工作线程永远醒不过来（互等死锁）。
        if (fileTransferService_)
            fileTransferService_->abortPendingTransfer();
        transferThread_->quit();
        if (!transferThread_->wait(3000)) {
            qWarning() << "RDPServer: transfer thread did not stop in 3s, waiting...";
            transferThread_->wait();
        }
    }
    if (xdndThread_ && xdndThread_->isRunning()) {
        xdndThread_->quit();
        if (!xdndThread_->wait(3000)) {
            qWarning() << "RDPServer: xdnd thread did not stop in 3s, waiting...";
            xdndThread_->wait();
        }
    }
    // [B8] join IME 工作线程：最坏 ~9s（命令链各步超时之和）。不 join 直接析构，
    // 线程还在 emit imeResultReady（QMetaObject::activate 解引用 d_ptr）会
    // use-after-free；join 阻塞析构优于崩溃，与上面两个线程的策略一致。
    if (imeWorker_ && imeWorker_->joinable()) {
        qInfo() << "RDPServer: waiting for IME worker thread to finish...";
        imeWorker_->join();
    }
    if (sslConfiguration_) {
        delete sslConfiguration_;
        sslConfiguration_ = nullptr;
    }
}

// 编码协议字符串 <-> CodecType（前端 setMode/config 消息）。未知值回退 H.264（兼容性最好）
static CodecType codecFromString(const QString& val)
{
    if (val == "hevc") return CodecType::HEVC;
    if (val == "vp8") return CodecType::VP8;
    if (val == "vp9") return CodecType::VP9;
    if (val == "av1") return CodecType::AV1;
    return CodecType::H264;
}

static QString codecToString(CodecType c)
{
    switch (c) {
    case CodecType::HEVC: return "hevc";
    case CodecType::VP8: return "vp8";
    case CodecType::VP9: return "vp9";
    case CodecType::AV1: return "av1";
    default: return "h264";
    }
}

// server_config.json 原子落盘（QSaveFile：临时文件 + commit 原子替换）。
// 主服务与 helper 进程都可能写这份文件，QSaveFile 保证任何时刻磁盘上
// 要么是旧完整内容要么是新完整内容，不会留下截断 JSON（截断文件下次
// 加载会被判损坏，users 等字段全部丢失 = 登录账号全没）。
static void writeServerConfigAtomic(const QString& path, const QJsonObject& root)
{
    QSaveFile save(path);
    if (!save.open(QIODevice::WriteOnly)) {
        qWarning() << "Failed to save server config to" << path
                   << ":" << save.errorString();
        return;
    }
    save.write(QJsonDocument(root).toJson(QJsonDocument::Indented));
    if (save.commit())
        qInfo() << "Server config saved to" << path;
    else
        qWarning() << "Failed to commit server config:" << path
                   << ":" << save.errorString();
}

// 首次启动（配置文件不存在）时的默认档位探测。
//
// 背景：默认值若按桌面 x86 写死（60fps / scale 100 / quality 60），弱机
// （Android chroot、ARM 单板）首次运行就直接过载 —— 实测 Cortex-A55
// 1600x1200 单帧色彩转换 86ms + 编码 38ms，配置 60fps 相当于超载 8 倍。
// 这里用一次轻量基准（RGB32→YUV420P 色彩转换 + JPEG 编码，各取中位数）
// 量化本机每百万像素的耗时，再按档位生成 fps/scale/quality；只跑一次，
// 且只在生成默认配置时执行，不覆盖已有配置。
struct ProbeDefaults {
    int fps;
    int quality;
    int scale;
    double swsMsPerMp;   // 色彩转换每百万像素耗时（无 FFmpeg 时为 0）
    double jpegMsPerMp;  // JPEG 编码每百万像素耗时
};

static ProbeDefaults probeMachineDefaults()
{
    constexpr int PW = 1280, PH = 800;
    const double megaPixels = static_cast<double>(PW) * PH / 1e6;

    QImage probe(PW, PH, QImage::Format_RGB32);
    for (int y = 0; y < PH; ++y) {
        QRgb* line = reinterpret_cast<QRgb*>(probe.scanLine(y));
        for (int x = 0; x < PW; ++x)
            line[x] = qRgb((x * 7) & 0xff, (y * 5) & 0xff, (x + y) & 0xff);
    }

    QVector<double> jpegSamples;
    for (int i = 0; i < 5; ++i) {
        QByteArray out;
        QBuffer buf(&out);
        buf.open(QIODevice::WriteOnly);
        QElapsedTimer t;
        t.start();
        probe.save(&buf, "JPEG", 35);
        if (i >= 1) // 第 1 次含 JPEG 插件加载/表初始化，不计入
            jpegSamples.append(t.nsecsElapsed() / 1e6);
    }
    double jpegMs = 0;
    if (!jpegSamples.isEmpty()) {
        std::sort(jpegSamples.begin(), jpegSamples.end());
        jpegMs = jpegSamples[jpegSamples.size() / 2];
    }

    double swsMs = 0;
#ifdef USE_FFMPEG
    SwsContext* sws = sws_getContext(PW, PH, AV_PIX_FMT_RGB32, PW, PH, AV_PIX_FMT_YUV420P,
                                     SWS_FAST_BILINEAR, nullptr, nullptr, nullptr);
    if (sws) {
        uint8_t* dst[4] = { nullptr, nullptr, nullptr, nullptr };
        int dstStride[4] = { 0, 0, 0, 0 };
        if (av_image_alloc(dst, dstStride, PW, PH, AV_PIX_FMT_YUV420P, 32) >= 0) {
            const uint8_t* src[1] = { probe.bits() };
            int srcStride[1] = { static_cast<int>(probe.bytesPerLine()) };
            QVector<double> swsSamples;
            for (int i = 0; i < 4; ++i) {
                QElapsedTimer t;
                t.start();
                sws_scale(sws, src, srcStride, 0, PH, dst, dstStride);
                if (i >= 1)
                    swsSamples.append(t.nsecsElapsed() / 1e6);
            }
            if (!swsSamples.isEmpty()) {
                std::sort(swsSamples.begin(), swsSamples.end());
                swsMs = swsSamples[swsSamples.size() / 2];
            }
            av_freep(&dst[0]);
        }
        sws_freeContext(sws);
    }
#endif

    ProbeDefaults d;
    d.swsMsPerMp = swsMs / megaPixels;
    d.jpegMsPerMp = jpegMs / megaPixels;

    // 每百万像素总开销分档（单线程口径）：
    //   ≤4ms   普通 x86 桌面（实测 sws+jpeg ≈1~3ms/MP）
    //   ≤12ms  x86 低功耗 / 较强 ARM
    //   ≤30ms  4 核级 ARM
    //   ≤60ms  Cortex-A55 级弱机（实测 A55：sws 37.8 + jpeg 10.7 ≈ 48.7ms/MP）
    // 编码线程里的色彩转换已按核数分条带并行（实测 8 条带 ≈3.3x），
    // 所以单线程耗时高不代表跑不动，档位按"并行后 + 采集仍串行"的预算定。
    const double cost = d.swsMsPerMp + d.jpegMsPerMp;
    if (jpegMs <= 0) {
        // JPEG 编码完全失败（多为 Qt 插件缺依赖库）：图片模式本身就不可用，
        // 探测结果不可信，直接给最保守档并告警。
        d.fps = 20; d.quality = 35; d.scale = 50;
        qWarning() << "Machine probe: JPEG encoding unavailable, using lowest default tier";
    } else if (cost <= 4.0) {
        d.fps = 60; d.quality = 60; d.scale = 100;
    } else if (cost <= 12.0) {
        d.fps = 40; d.quality = 50; d.scale = 100;
    } else if (cost <= 30.0) {
        d.fps = 30; d.quality = 40; d.scale = 75;
    } else if (cost <= 60.0) {
        d.fps = 24; d.quality = 35; d.scale = 60;
    } else {
        d.fps = 20; d.quality = 35; d.scale = 50;
    }
#if defined(Q_OS_ANDROID)
    // Android（MediaProjection 采集）：浏览器 canvas 必须与手机物理分辨率一致
    // （720x1612 这类），否则用户看到的仍是缩放后的画面，无法与手机截图对照。
    // 帧率仍按探测结果保守起步，跟不上时服务端自适应会继续降 fps。
    d.scale = 100;
#endif
    qInfo() << "Machine probe: sws =" << d.swsMsPerMp << "ms/MP, jpeg =" << d.jpegMsPerMp
            << "ms/MP -> defaults fps =" << d.fps << ", scale =" << d.scale
            << ", quality =" << d.quality;
    return d;
}

// 配置文件目录：桌面端沿用可执行文件目录（与既有部署约定一致）；
// Android 应用目录在 APK 内且只读，配置必须落到应用私有可写目录。
static QString serverConfigDirectory()
{
#if defined(Q_OS_ANDROID)
    return QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
#else
    return QCoreApplication::applicationDirPath();
#endif
}

void RDPServer::loadServerConfig(const QString& configPath)
{
    QString path = configPath.isEmpty()
        ? serverConfigDirectory() + "/server_config.json"
        : configPath;

    QJsonObject root;
    bool exists = false;
    bool filePresent = false;

    QFile file(path);
    if (file.exists()) {
        filePresent = true;
        if (file.open(QIODevice::ReadOnly)) {
            QJsonDocument doc = QJsonDocument::fromJson(file.readAll());
            file.close();
            if (doc.isObject()) {
                root = doc.object();
                exists = true;
            }
        }
    }

    if (!exists) {
        // 文件存在但解析失败/不可读 ≠ 文件不存在。前者直接覆盖会把 users 等
        // 全部旧字段冲掉（登录账号全没）：先改名留档再按全新安装写默认值。
        if (filePresent) {
            const QString bak = path + QStringLiteral(".corrupt-")
                + QDateTime::currentDateTime().toString(QStringLiteral("yyyyMMddHHmmss"));
            if (QFile::rename(path, bak))
                qCritical() << "server config corrupted (invalid/unreadable JSON), moved to"
                            << bak << "and recreated with defaults";
            else
                qCritical() << "server config corrupted (invalid/unreadable JSON) and cannot"
                            << "be moved aside; recreating with defaults";
        }
        qInfo() << "No server config found at" << path << "creating with defaults";
        root["ssl"] = false;
        root["httpPort"] = 8080;
        root["console"] = false;
        // 默认档位按本机实测性能生成，不再写死桌面档（60fps/scale 75/quality 60）。
        // 弱机（Android chroot、ARM 单板）沿用桌面档会在首次运行就长期过载。
        // 静态桌面有 checksum/XDamage 去重兜底，强机仍可跑满高帧率。
        const ProbeDefaults defaults = probeMachineDefaults();
        root["fps"] = defaults.fps;
        root["quality"] = defaults.quality;
        root["scale"] = defaults.scale;
        root["codec"] = "h264";
        root["hw_encode"] = "auto";
        writeServerConfigAtomic(path, root);
    }

    if (root.contains("ssl"))
        useSsl_ = root["ssl"].toBool();

    if (useSsl_ && !QSslSocket::supportsSsl()) {
        useSsl_ = false;
        qInfo() << "SSL not supported, disabling";
    }

    if (root.contains("httpPort")) {
        int port = root["httpPort"].toInt();
        if ((port < 1) || (port > 65535)) {
            qWarning() << "Invalid httpPort" << port << "using default 8080";
            port = 8080;
        }
        httpPort_ = static_cast<quint16>(port);
    }

    if (root.contains("fps"))
        configFps_ = userFps_ = qBound(1, root["fps"].toInt(), 60);
    if (root.contains("quality"))
        configQuality_ = qBound(10, root["quality"].toInt(), 100);
    if (root.contains("scale")) {
        configScale_ = userScale_ = configScaleWish_ = qBound(10, root["scale"].toInt(), 100);
    }
    // [PACE-FACTOR] 采集节流倍数（见 rdpserver.h）。缺省 2 = 保持历史行为。
    if (root.contains("capturePaceFactor"))
        configPaceFactor_ = qBound(1.0, root["capturePaceFactor"].toDouble(), 12.0);

    if (root.contains("codec"))
        configCodec_ = codecFromString(root["codec"].toString());
    if (root.contains("hw_encode")) {
        QString m = root["hw_encode"].toString();
        configHwEncodeMode_ = (m == "on") ? HwEncodeMode::On
                          : (m == "off") ? HwEncodeMode::Off : HwEncodeMode::Auto;
    }
    if (root.contains("unlockPassword"))
        unlockPassword_ = root["unlockPassword"].toString();
    // [Win-登录] 登录界面远程登录的目标账户；留空时运行时取"最后控制台用户"。
    if (root.contains("loginUsername"))
        loginUsername_ = root["loginUsername"].toString().trimmed();
    if (root.contains("systemName"))
        systemName_ = root["systemName"].toString();

    qInfo() << "Server config loaded: ssl =" << useSsl_ << "httpPort =" << httpPort_
            << "fps =" << configFps_ << "quality =" << configQuality_ << "scale =" << configScale_
            << "codec =" << codecToString(configCodec_) << "hw_encode =" << (configHwEncodeMode_ == HwEncodeMode::On ? "on" : configHwEncodeMode_ == HwEncodeMode::Off ? "off" : "auto");
}

// [P2] 配置去抖落盘
void RDPServer::scheduleSaveConfig()
{
    if (!configSaveTimer_) {
        configSaveTimer_ = new QTimer(this);
        configSaveTimer_->setSingleShot(true);
        configSaveTimer_->setInterval(800);
        connect(configSaveTimer_, &QTimer::timeout, this, [this]() {
            saveServerConfig(QString());
        });
    }
    configSaveTimer_->start();
}

QString RDPServer::effectiveSystemName() const
{
    // 只有纯空白也算"没配"：用户手滑打个空格不该让标签页变成空白。
    const QString name = systemName_.trimmed();
    if (!name.isEmpty())
        return name;
    QString host = QSysInfo::machineHostName().trimmed();
    if (host.isEmpty())
        host = QStringLiteral("远程桌面");
    return host;
}

void RDPServer::broadcastSystemName()
{
    if (!wsServer_)
        return;
    QJsonObject msg;
    msg["type"] = "system_name";
    msg["systemName"] = systemName_;                 // 配置值（可能为空 = 默认）
    msg["effectiveName"] = effectiveSystemName();    // 实际显示用
    wsServer_->broadcastJson(msg);
}

void RDPServer::saveServerConfig(const QString& configPath)
{
    QString path = configPath.isEmpty()
        ? serverConfigDirectory() + "/server_config.json"
        : configPath;

    QJsonObject root;

    // 读取已有的配置文件，保留 users 等其他字段
    QFile file(path);
    bool parsedOk = true;
    if (file.open(QIODevice::ReadOnly)) {
        const QByteArray raw = file.readAll();
        file.close();
        if (!raw.trimmed().isEmpty()) {
            QJsonParseError perr;
            const QJsonDocument doc = QJsonDocument::fromJson(raw, &perr);
            if (doc.isObject())
                root = doc.object();
            else
                parsedOk = false;
        }
    }
    // [A11] 解析失败时**绝不能**用默认值整份覆盖：这个文件里同时存着 users
    // （账号与口令哈希）。覆盖后 AuthManager 会因"有文件但无用户"而拒绝重建
    // 默认管理员（防止口令被重置成 admin/admin），结果是永久无法登录，且
    // 用户的端口/画质/分辨率配置全部丢失、原文件无从恢复。
    // 这里保留原文件、另存一份 .corrupt 供人工修复，并拒写。
    if (!parsedOk) {
        const QString broken = path + QStringLiteral(".corrupt");
        QFile::remove(broken);
        QFile::copy(path, broken);
        qCritical() << "server config is not valid JSON, refusing to overwrite it."
                    << "Original kept at" << path << "copy at" << broken;
        return;
    }

    // 更新 ssl、httpPort、fps、quality 为当前运行值
    root["ssl"] = useSsl_;
    root["httpPort"] = httpPort_;
    root["fps"] = userFps_;
    root["quality"] = configQuality_;
    // 持久化用户手动选择的缩放（userScale_），而不是生效值 configScale_：
    // high/verylow 画质档位会临时把 configScale_ 改成 100/≤50，若按旧逻辑写
    // configScale_，选一次档位再重启，用户手动设的缩放就被永久覆盖。
    root["scale"] = userScale_;
    root["capturePaceFactor"] = configPaceFactor_;
    // WebRTC 期间 configCodec_ 被临时改成 H.264：落盘必须写用户原本的选择，
    // 否则用一次 WebRTC 就会把 HEVC 等配置永久覆盖成 H.264。
    root["codec"] = codecToString(codecOverriddenForWebRtc_ ? codecBeforeWebRtc_ : configCodec_);
    root["hw_encode"] = (configHwEncodeMode_ == HwEncodeMode::On) ? "on"
                      : (configHwEncodeMode_ == HwEncodeMode::Off) ? "off" : "auto";
    // 页面标题名（前端口子里的「计算机名称」）。空 = 跟随主机名，因此必须照写：
    // 用户把自定义名清空后要能"恢复默认"，只在非空时写会让旧名字永远留着。
    root["systemName"] = systemName_;
    // [Win-登录] 空 = 运行时取最后控制台用户，照写以支持"清空恢复默认"。
    root["loginUsername"] = loginUsername_;

    // 原子替换：旧实现 QFile::remove(path) 后再 rename，两步之间进程被杀/
    // 断电会永久丢失配置文件；Windows 上 remove 失败则 rename 必失败。
    // QSaveFile::commit 全程原子（Qt 5.1+ 即有，AuthManager 同款）。
    writeServerConfigAtomic(path, root);
}

void RDPServer::loadSslConfig()
{
    qDebug() << "SSL supported:" << QSslSocket::supportsSsl();
    qDebug() << "SSL library version:" << QSslSocket::sslLibraryVersionString();
    qDebug() << "SSL library build version:" << QSslSocket::sslLibraryBuildVersionString();

    if (!useSsl_ || !QSslSocket::supportsSsl()) {
        return;
    }

    // Load the SSL certificate
    QFile certFile(":/res/sslperm/cacert.crt");
    if (!certFile.open(QIODevice::ReadOnly)) {
        qCritical("RDPServer: cannot open sslCertFile");
        useSsl_ = false; // 证书缺失：降级为普通 HTTP/WS，避免半开 SSL 监听
        return;
    }

    QByteArray certData = certFile.readAll();
    certFile.close();
    QSslCertificate certificate(certData, QSsl::Pem);

    if (certificate.isNull()) {
        qDebug() << "certificate is invalid";
        useSsl_ = false;
        return;
    }

    // Load the key file
    QFile keyFile(":/res/sslperm/privkey.pem");
    if (!keyFile.open(QIODevice::ReadOnly)) {
        qCritical("RDPServer: cannot open sslKeyFile");
        useSsl_ = false;
        return;
    }

    QByteArray keyData = keyFile.readAll();
    keyFile.close();
    QSslKey sslKey(keyData, QSsl::Rsa, QSsl::Pem);

    if (sslKey.isNull()) {
        qDebug() << "keyFile invalid";
        useSsl_ = false;
        return;
    }

    // Create the SSL configuration
    sslConfiguration_ = new QSslConfiguration();

    sslConfiguration_->setLocalCertificate(certificate);
    sslConfiguration_->setPrivateKey(sslKey);
    sslConfiguration_->setPeerVerifyMode(QSslSocket::VerifyNone);
    sslConfiguration_->setProtocol(QSsl::TlsV1_2OrLater);

    qDebug("RDPServer: SSL settings loaded");
}

void RDPServer::onModeChangeRequested(const QString& mode)
{
    if (mode == "image") {
        switchToImageMode();
    } else if (mode == "video" || mode == "webrtc") {
        // webrtc/video 都切到视频模式；是否建立 RTP 会话由前端 signal_start 发起
        if (!switchToVideoMode()) {
            // 服务端无法进入视频模式（无本地捕获源且 helper 未连/不具视频能力）。
            // 旧逻辑忽略返回值且不回包，前端会一直对着 MSE/WebRTC 的全屏 <video>
            // 黑屏干等。这里显式广播一次 mode_changed，带上 videoSupported=false，
            // 前端据此立即优雅降级为图片模式并提示用户。
            QJsonObject note;
            note["type"] = "mode_changed";
            note["mode"] = "image";
            note["videoSupported"] = false;
            note["hwEncode"] = hwEncodeAvailable();
            note["codec"] = codecToString(configCodec_);
            note["reason"] = "no_video_source";
            wsServer_->broadcastJson(note);
            qWarning() << "set_mode video rejected: no video source available "
                          "(no local capturer and no video-capable helper)";
        }
    }
}

bool RDPServer::initialize(const QString& configPath, bool useSslOverride, bool serviceMode)
{
    serviceMode_ = serviceMode;

    // 加载配置文件
    httpPort_ = 8080;
    loadServerConfig(configPath);

    shellCurrentDir_ = QDir::currentPath();

    // 命令行参数 --no-ssl 覆盖配置文件
    if (!useSslOverride)
        useSsl_ = false;

    // 将当前运行配置写回文件（ssl、httpPort 等）
    saveServerConfig(configPath);

    wsPort_ = httpPort_ + 1;
    if (httpPort_ >= 65535) {
        // quint16 溢出会让 wsPort_ 变成 0，listen(0) 随机挑一个临时端口，
        // 浏览器按 httpPort+1 永远连不上。65535 时改用相邻低位端口。
        wsPort_ = httpPort_ - 1;
        qWarning() << "httpPort 65535 leaves no room for wsPort, using" << wsPort_;
    }

    if (useSsl_)
        loadSslConfig();

    // 初始化认证管理器
    authManager_ = new AuthManager(this);

    // 设置 HTTP 服务器（端口被占用等绑定失败 = 致命，中止启动）
    if (!setupHttpServer())
        return false;

    // 初始化 WebSocket 服务器
    // wsServer_ = std::make_unique<WebSocketServer>(this);

    QWebSocketServer::SslMode wsMode = sslConfiguration_ ? QWebSocketServer::SecureMode : QWebSocketServer::NonSecureMode;
    wsServer_ = std::unique_ptr<WebSocketServer>(new WebSocketServer(wsMode, this));
    if (sslConfiguration_) {
        wsServer_->setSslConfiguration(*sslConfiguration_);
    }

    connect(wsServer_.get(), &WebSocketServer::clientConnected,
        this, &RDPServer::onClientConnected);
    connect(wsServer_.get(), &WebSocketServer::clientDisconnected,
        this, &RDPServer::onClientDisconnected);
    connect(wsServer_.get(), &WebSocketServer::inputReceived,
        this, &RDPServer::onInputReceived);
    // [C] secure-input 的上行解锁进度：verified / typed / blocked / uncertain。
    // 服务端据此决定"这一轮还要不要补发"，以及"要不要告诉客户端被拒绝"。
    // 排序/去重全部在这里做：secure-input 不知道服务端的屏幕状态，也没有序列号以外
    // 的判据可用。
    connect(wsServer_.get(), &WebSocketServer::secureInputMessageReceived,
        this, [this](const QJsonObject& msg) {
            if (msg["type"].toString() != QLatin1String("unlock_progress"))
                return;
            const int seq = msg["seq"].toInt();
            const QString stage = msg["stage"].toString();
            const QString reason = msg["reason"].toString();
            const QString text = msg["message"].toString();
            const QString desktop = msg["desktop"].toString();

            // 过期上报：上一轮解锁的、或解锁已经完成的。seq 不匹配就整条丢弃，
            // 这样"成功后 8s 补发报 blocked"那种误报不会再出现在客户端。
            if (seq != unlockSeq_ || secureInputUnlockClientId_.isEmpty()) {
                qInfo() << "Service: drop stale unlock progress (stage =" << stage
                        << ", seq =" << seq << ", current =" << unlockSeq_ << ")";
                return;
            }

            if (stage == QLatin1String("verified")) {
                qInfo() << "Service: unlock target verified (seq =" << seq << ")";
                return;
            }

            if (stage == QLatin1String("typed")) {
                // 这一轮密码真的键入了：Vista+ 绝不能再补发第二轮 —— 补发发生在
                // 登录成功之后，会把明文密码敲进新用户的桌面/普通窗口。
                // 同时把 15s 终判换成"看得见结果"的判据：再过 8s 屏幕还锁着，就是密码不对。
                secureUnlockTyped_ = true;
                qInfo() << "Service: unlock password typed (typed =" << msg["typed"].toInt()
                        << ", dropped =" << msg["dropped"].toInt()
                        << ", cleared =" << msg["cleared"].toInt()
                        << ") -> no retry this round, seq =" << seq;
                QTimer::singleShot(8000, this, [this, seq]() {
                    if (seq != unlockSeq_ || !screenLocked_ || !secureUnlockTyped_)
                        return;
                    if (secureUnlockBlockedReported_)
                        return;
                    qWarning() << "Service: unlock still locked 8s after typed password"
                               << "-> wrong_password";
                    QJsonObject out;
                    out["type"] = "unlock_result";
                    out["ok"] = false;
                    out["reason"] = "wrong_password";
                    out["message"] = "密码未通过验证（unlockPassword 与 Windows 登录密码不一致？）";
                    wsServer_->sendJson(secureInputUnlockClientId_, out);
                });
                return;
            }

            // blocked / uncertain：这一轮没键入任何字符。
            const bool definite = (stage == QLatin1String("blocked"));
            secureUnlockDefinite_ = definite;
            qWarning() << "Service: secure-input did not inject -" << stage << reason
                       << "desktop =" << desktop << text
                       << "seq =" << seq;

            // 屏幕已经解开 / 客户端已换：不再报"被拒绝"（否则就是"已经解开却显示被拒绝"）。
            if (!screenLocked_ || secureInputUnlockClientId_.isEmpty())
                return;
            // 同一轮只报一次（uncertain 允许 8s 后补发一次，别连着报两遍）。
            if (secureUnlockBlockedReported_)
                return;
            secureUnlockBlockedReported_ = true;

            QJsonObject out;
            out["type"] = "unlock_result";
            out["ok"] = false;
            out["blocked"] = true;
            out["stage"] = stage;
            out["reason"] = reason;      // not_logon_ui / uncertain / nothing_typed
            out["message"] = text;
            wsServer_->sendJson(secureInputUnlockClientId_, out);
        });

#ifdef Q_OS_WIN32
    // [Win-登录] secure-input 的登录进度：typed → 服务端轮询 WTS 会话用户名做
    // 最终确认；blocked/uncertain → 直接把固定枚举回给客户端。与 unlock_progress
    // 共用通道，靠 type 分流，互不影响。
    connect(wsServer_.get(), &WebSocketServer::secureInputMessageReceived,
        this, [this](const QJsonObject& msg) {
            if (msg["type"].toString() != QLatin1String("login_progress"))
                return;
            handleLoginProgress(msg);
        });
#endif

    // 剪贴板服务：系统剪贴板变化（文本/图片）→ 广播给所有客户端
    clipboardService_ = std::unique_ptr<ClipboardService>(new ClipboardService(this));
    clipboardService_->start();

    // [A10] 光标位置广播默认关闭。
    // 前端 updateRemoteCursor() 只把 x/y 写进 state.remoteCursorX/Y，整个 index.html
    // 没有任何读取点（光标 overlay 早已停用、恢复成浏览器默认光标）⇒ 这个 33Hz
    // 定时器是纯死流量：每次 mousemove 一次 QJsonObject 构造 + 每个客户端一次 WS 写，
    // 换来前端两个谁也不读的字段。
    // 实现保留：将来要恢复 overlay（或做"多端同看一个光标"）时用 QTRD_CURSOR_POS=1
    // 打开即可，不必重新写一遍平台查询。
    cursorTimer_ = new QTimer(this);
    connect(cursorTimer_, &QTimer::timeout, this, &RDPServer::broadcastCursorPos);
    if (qgetenv("QTRD_CURSOR_POS") == "1") {
        cursorTimer_->start(30);
        qInfo() << "Cursor position broadcast enabled (QTRD_CURSOR_POS=1)";
    }

    connect(clipboardService_.get(), &ClipboardService::contentChanged,
        this, [this](const QString& mime, const QByteArray& data) {
            // [N1] 服务模式下 helper 通道已上报同一内容，这里不再重复广播
            if (serviceMode_ && wsServer_->isCaptureSourceConnected())
                return;
            wsServer_->broadcastJson(clipboardPayload(mime, data));
        });
    // 读取剪贴板是异步的（CLI 模式走工作线程，避免阻塞主线程）：结果回来后
    // 推送给所有处于等待中的新客户端。
    connect(clipboardService_.get(), &ClipboardService::contentReady,
        this, [this](const QString& mime, const QByteArray& data) {
            const QStringList ids = pendingClipboardClientIds_;
            pendingClipboardClientIds_.clear();
            if (data.isEmpty())
                return;
            const QJsonObject payload = clipboardPayload(mime, data);
            for (const QString& id : ids)
                wsServer_->sendJson(id, payload);
        });
    // 客户端内容写入剪贴板成功后：广播给所有客户端（含发送者）保持多端一致，
    // 并在远端触发一次 Ctrl+V 粘贴。
    connect(clipboardService_.get(), &ClipboardService::contentApplied,
        this, [this](bool ok, const QString& mime, const QByteArray& data) {
            if (!ok)
                return;
            wsServer_->broadcastJson(clipboardPayload(mime, data));
            injectPasteShortcut();
        });

    connect(wsServer_.get(), &WebSocketServer::modeChangeRequested,
        this, &RDPServer::onModeChangeRequested);

    connect(wsServer_.get(), &WebSocketServer::shellConnected,
        this, &RDPServer::onShellConnected);

    // Initialize file transfer service on dedicated thread
    transferThread_ = new QThread(this);
    fileTransferService_ = new FileTransferService();
    fileTransferService_->moveToThread(transferThread_);

    connect(transferThread_, &QThread::finished,
        fileTransferService_, &QObject::deleteLater);

    // Main thread requests -> worker thread processing
    connect(this, &RDPServer::requestFileList,
        fileTransferService_, &FileTransferService::processFileList);
    connect(this, &RDPServer::requestDownload,
        fileTransferService_, &FileTransferService::processDownload);
    connect(this, &RDPServer::requestUploadStart,
        fileTransferService_, &FileTransferService::processUploadStart);
    connect(this, &RDPServer::requestUploadDone,
        fileTransferService_, &FileTransferService::processUploadDone);

    // Upload chunk signal：先按 clientId 过滤观看模式，再转投文件服务线程。
    // file_upload_start 被拦后通常不会有块，但"上传中途切只读"或改过的客户端
    // 仍可能继续投喂，这里兜底丢弃。
    connect(wsServer_.get(), &WebSocketServer::fileChunkReceived,
        this, [this](const QString& clientId, const QString& path, const QByteArray& data) {
            if (viewOnlyClients_.contains(clientId))
                return;
            emit fileChunkFromClient(clientId, path, data);
        });
    connect(this, &RDPServer::fileChunkFromClient,
        fileTransferService_, &FileTransferService::processUploadChunk);

#if defined(Q_OS_LINUX) && !defined(Q_OS_ANDROID)
    // 远端屏幕文件拖拽侦测（Linux/X11，XDND 协议）：
    // 用户在远端桌面里拖住文件时，把可下载项广播给浏览器端 —— 指针拖到
    // 屏幕边缘即触发本机下载（Wayland 桌面无公开的拖拽侦测接口，跳过）。
    if (!InputManager::desktopSessionIsWayland()) {
        xdndThread_ = new QThread(this);
        xdndWorker_ = new XdndMonitor();   // 无 parent：moveToThread 后归线程管理
        xdndWorker_->moveToThread(xdndThread_);
        connect(xdndThread_, &QThread::started, xdndWorker_, &XdndMonitor::start);
        connect(xdndThread_, &QThread::finished, xdndWorker_, &QObject::deleteLater);
        connect(xdndWorker_, &XdndMonitor::dragChanged, this,
            [this](bool active, const QStringList& files) {
                QJsonObject msg;
                msg["type"] = "remote_drag";
                msg["active"] = active;
                QJsonArray arr;
                if (active) {
                    for (const QString& raw : files) {
                        // 与 WS/HTTP 下载同一条越权校验：受限根外的拖拽项不下发
                        const QString safe = FileTransferService::sanitizeFilePath(raw);
                        if (safe.isEmpty())
                            continue;
                        QFileInfo fi(safe);
                        QJsonObject item;
                        item["path"] = safe;
                        item["name"] = fi.fileName();
                        item["isDir"] = fi.isDir();
                        arr.append(item);
                    }
                }
                // 文件一个都解析不出来时不下发，避免前端空弹提示
                if (active && arr.isEmpty())
                    return;
                msg["files"] = arr;
                lastRemoteDragMsg_ = msg;   // 供新接入客户端补发快照
                wsServer_->broadcastJson(msg);
            });
        xdndThread_->start();
        qInfo() << "XdndMonitor: remote file drag detection enabled (X11)";
    } else {
        qInfo() << "XdndMonitor: Wayland desktop, remote file drag detection disabled";
    }
#endif

    // Service responses -> WebSocket sends (main thread)
    connect(fileTransferService_, &FileTransferService::jsonResponse,
        this, [this](const QString& clientId, const QJsonObject& obj) {
            wsServer_->sendJson(clientId, obj);
        });

    connect(fileTransferService_, &FileTransferService::downloadChunkReady,
        this, [this](const QString& clientId, const QString& path, qint64 offset, const QByteArray& data, qint64 totalSize) {
            Q_UNUSED(totalSize);
            QByteArray pathUtf8 = path.toUtf8();
            QByteArray packet;
            QDataStream stream(&packet, QIODevice::WriteOnly);
            stream.setByteOrder(QDataStream::BigEndian);
            stream << quint8(0x12);
            stream << quint32(pathUtf8.size());
            stream.writeRawData(pathUtf8.constData(), pathUtf8.size());
            stream << quint64(offset);
            stream << quint32(data.size());
            stream.writeRawData(data.constData(), data.size());
            wsServer_->sendBinaryToClient(clientId, packet);
        });

    connect(fileTransferService_, &FileTransferService::transferProgress,
        this, [this](const QString& clientId, const QString& path, qint64 transferred, qint64 total, double speedKBps) {
            wsServer_->sendJson(clientId, QJsonObject { { "type", "transfer_progress" }, { "path", path }, { "transferred", transferred }, { "total", total }, { "speedKBps", speedKBps } });
        });

    // [C5-①] 给文件传输工作线程注入 WS 背压查询：下游积压超限时暂停产出
    fileTransferService_->setBackpressureQuery([this]() {
        return wsServer_ ? wsServer_->downloadBacklog() : qint64(0);
    });
    // 断线感知：积压查询无法区分"客户端已断线"（包被丢弃 → 积压恒 0），
    // 必须额外注入存活性查询，否则整树打包会为一个不存在的对端空转。
    fileTransferService_->setClientAliveQuery([this](const QString& clientId) {
        return wsServer_ ? wsServer_->isClientAlive(clientId) : false;
    });

    transferThread_->start();

    if (!wsServer_->listen(QHostAddress::Any, wsPort_)) {
        qCritical() << "Failed to start WebSocket server on port" << wsPort_;
        return false;
    }

    if (!serviceMode_) {
        // 服务模式：捕获由 helper 进程完成
        screenCapturer_ = std::unique_ptr<ScreenCapturer>(new ScreenCapturer(this));
        connect(screenCapturer_.get(), &ScreenCapturer::frameCaptured,
            this, &RDPServer::onFrameCaptured);
#if defined(Q_OS_ANDROID)
        // 旋转/分辨率变化：帧尺寸变了，输入换算基准与前端 canvas 都要同步，
        // 否则点击会按旧尺寸映射到错误位置。
        connect(screenCapturer_.get(), &ScreenCapturer::androidFrameSizeChanged,
            this, [this](int w, int h) {
                if (inputManager_)
                    inputManager_->setScreenSize(w, h);
                if (wsServer_) {
                    QJsonObject info;
                    info["type"] = "screen_info";
                    info["width"] = w;
                    info["height"] = h;
                    info["platform"] = platformTag();
                    wsServer_->broadcastJson(info);
                }
            });
#endif
        connect(screenCapturer_.get(), &ScreenCapturer::screenLocked,
            this, [this](bool locked) {
#ifdef _WIN32
                // XP 分支 = 非 Vista+（判定见 systeminfo.h：kernel32 导出探测，
                // 不受"以兼容模式运行 Windows XP"标记伪造）
                static const bool isWinXP = !isVistaPlusOs();
#endif
                screenLocked_ = locked;
                if (!locked) {
                    // [C] 屏幕已解开 = 当前解锁序列结束：seq 自增让所有延时回调
                    // （首轮投递 / 8s 重试 / 8s wrong_password 判定 / 15s 终判）立刻
                    // 失效，连同已排队的 blocked 转发一起作废 —— 客户端不会再看到
                    // "已经解开了却弹出一条解锁失败"。
                    ++unlockSeq_;
                    secureUnlockTyped_ = false;
                    secureUnlockDefinite_ = false;
                    secureUnlockBlockedReported_ = false;
                    secureInputUnlockClientId_.clear();
                }
                // [B16] 与 startCapture 的处理器保持一致：坐标归一化基准 + 锁屏
                // 焦点抓取开关。前台路径缺这两个调用 → 锁屏密码输入失焦逻辑失效、
                // 坐标回退物理像素错位。
                if (inputManager_) {
                    inputManager_->setScreenSize(screenCapturer_->width(), screenCapturer_->height());
                    inputManager_->setScreenOffset(screenCapturer_->originX(), screenCapturer_->originY());
                }
#if defined(Q_OS_LINUX) && !defined(Q_OS_ANDROID)
                if (inputManager_)
                    inputManager_->setScreenLocked(locked);
#endif
                broadcastScreenLocked(QJsonObject {
                    { "type", "screen_locked" },
                    { "locked", locked },
                    { "platform", platformTag() },
#ifdef _WIN32
                    { "isXP", isWinXP },
#endif
                    { "hint", locked ? QString::fromUtf8(
#ifdef Q_OS_WIN
                        // XP 也走 secure-input：屏保锁屏先结束 .scr，再点用户磁贴展开
                        // 密码框、键入密码。两代机器的提示可以合并成一句。
                        "Windows 锁屏在安全桌面上，点「解锁」由服务端注入密码"
#elif defined(Q_OS_ANDROID)
                        "Android 锁屏输入暂不支持"
#elif defined(Q_OS_LINUX)
                        "如需在锁屏界面输入密码，请先执行："
                        "sudo usermod -aG input $USER && "
                        "sudo modprobe uinput && sudo chmod 666 /dev/uinput"
#elif defined(Q_OS_MACOS)
                        "macOS 锁屏状态输入需要辅助功能权限："
                        "系统偏好设置 → 隐私与安全性 → 辅助功能 → 添加此应用"
#else
                        "锁屏状态下输入可能受限"
#endif
                                           "然后直接输入密码回车即可")
                                     : QString() } });
                if (locked) {
                    qInfo() << "Screen locked";
#if defined(Q_OS_LINUX) && !defined(Q_OS_ANDROID)
                    if (inputManager_ && !inputManager_->initUinput())
                        qWarning() << "uinput unavailable, XTest fallback";
#endif
                } else {
                    qInfo() << "Screen unlocked, restoring normal input";
#if defined(Q_OS_LINUX) && !defined(Q_OS_ANDROID)
                    if (inputManager_)
                        inputManager_->destroyUinput();
#endif
                }
            });

        // 获取屏幕几何信息
        QScreen* screen = QGuiApplication::primaryScreen();
        if (!screen) {
            qCritical() << "No primary screen available";
            return false;
        }
        screenGeometry_ = screen->geometry();

        // 初始化 JPEG 压缩器（独立线程）
        jpegCompressor_ = std::unique_ptr<JpegCompressor>(new JpegCompressor(nullptr));
        jpegCompressor_->setQuality(configQuality_);
        jpegCompressor_->setScalePercent(configScale_);
        connect(jpegCompressor_.get(), &JpegCompressor::jpegCompressed,
            this, &RDPServer::onJpegCompressed, Qt::QueuedConnection);
        jpegCompressor_->start();

#ifdef USE_FFMPEG
        createVideoEncoder();
#endif
    } else {
        // 服务模式：helper 进程的截屏帧通过 WebSocket 传入
        connect(wsServer_.get(), &WebSocketServer::captureFrameReceived,
            this, [this](const QByteArray& jpegData) {
                if (wsServer_->clientCount() == 0)
                    return;
                QByteArray packet;
                QDataStream stream(&packet, QIODevice::WriteOnly);
                stream.setByteOrder(QDataStream::BigEndian);
                stream << quint8(0x03);
                stream << quint32(jpegData.size());
                packet.append(jpegData);
                wsServer_->broadcastBinary(packet);
            });
        // 服务模式视频：helper 侧完成 H.264 编码，编码帧经 /capture 上报（0x01/0x02），
        // 服务进程直接复用 onEncodedFrame()（广播浏览器 + 喂 WebRTC RTP），无需二次编码。
        connect(wsServer_.get(), &WebSocketServer::captureVideoFrameReceived,
            this, [this](const QByteArray& data, bool isKeyframe, qint64 timestamp) {
                if (currentMode_ != ServerMode::Video)
                    return;
                onEncodedFrame(data, isKeyframe, timestamp);
            });
        connect(wsServer_.get(), &WebSocketServer::captureMessageReceived,
            this, [this](const QJsonObject& msg) {
                const QString t = msg["type"].toString();
                // 低频控制消息记 qDebug；cursor_pos 高频事件不打日志
                if (t != "cursor_pos")
                    qDebug() << "Service: capture msg =" << t;
                if (t == "screen_info") {
                    lastScreenInfo_ = msg;
                    // 几何变了（用户切分辨率）：旧几何上算出的降级上限不再成立，松开它。
                    releaseAdaptCapsForGeometry(msg["width"].toInt(), msg["height"].toInt());
                } else if (t == "screen_locked") {
                    screenLocked_ = msg["locked"].toBool();
                    // helper 在用户会话里能看到真实输入桌面，服务进程看不到（Session 0 的
                    // OpenInputDesktop 直接失败）；锁屏 UI 在哪个桌面只有它知道。
                    lockedDesktop_ = screenLocked_ ? msg["desktop"].toString() : QString();
                    lockedKind_ = screenLocked_ ? msg["kind"].toString() : QString();
                    lockedHint_ = screenLocked_ ? msg["hint"].toString() : QString();
                    if (screenLocked_)
                        startSecureInputProcess();
                    else {
                        stopSecureInputProcess();
                        // [B] 已解锁：这一轮解锁序列（含可能的拦截上报）结束，
                        // 清掉请求来源，避免把拦截原因回给过期的客户端。
                        // [C] 同时作废该序列的全部延时回调与已排队的 blocked 转发：
                        // seq 自增后，8s 重试 / 8s wrong_password / 15s 终判的 lambda
                        // 与之后到达的 unlock_progress 都会被 seq 校验丢弃。
                        ++unlockSeq_;
                        secureUnlockTyped_ = false;
                        secureUnlockDefinite_ = false;
                        secureUnlockBlockedReported_ = false;
                        secureInputUnlockClientId_.clear();
                    }
                    // [LOCK] helper 报"已锁"是比 ack 更硬的证据：等锁屏结果的客户端
                    // 在这里收尾，之后 3s/2.5s 的兜底定时器会因 lockPending_=false 失效。
                    // [LOCK-GATE] 必须有显式非空 clientId 才回执：Linux 分支的锁屏
                    // 在途状态只用 lockPending_ 表达（从不设置 lockPendingClientId_），
                    // 其结果由 Linux 分支自己发出。不能依赖"sendJson(空 clientId) 是
                    // no-op"这一隐式行为——那会让 Linux 的正确性挂在实现细节上；
                    // 而这里若清掉 lockPending_，也会误清 Linux 的在途标志。
                    if (screenLocked_ && lockPending_ && !lockPendingClientId_.isEmpty()) {
                        lockPending_ = false;
                        const QString cid = lockPendingClientId_;
                        lockPendingClientId_.clear();
                        qInfo() << "Lock: confirmed by screen_locked=true, notify client";
                        wsServer_->sendJson(cid, QJsonObject {
                            { "type", "lock_result" }, { "ok", true },
                            { "reason", "locked" }, { "message", "已锁屏" } });
                    }
                    // 补平台标识后统一走去重广播（状态未变化不重复下发）。
                    QJsonObject out = msg;
                    out["platform"] = platformTag();
#ifdef _WIN32
                    out["isXP"] = !isVistaPlusOs();
#endif
                    broadcastScreenLocked(out);
                    return;
                } else if (t == "system_action_result") {
                    // [LOCK] helper 对系统操作的回执（服务端↔helper 的内部消息，
                    // 绝不能广播给浏览器）。lock 用它收尾等待中的请求。
                    const QString act = msg["action"].toString();
                    const bool ok = msg["ok"].toBool();
                    qInfo() << "Service: helper system_action_result action =" << act
                            << "ok =" << ok << "detail =" << msg["detail"].toString();
                    if (act == QLatin1String("lock") && lockPending_) {
                        lockPending_ = false;
                        const QString cid = lockPendingClientId_;
                        lockPendingClientId_.clear();
                        wsServer_->sendJson(cid, QJsonObject {
                            { "type", "lock_result" }, { "ok", ok },
                            { "reason", ok ? QStringLiteral("locked")
                                           : QStringLiteral("lock_failed") },
                            { "message", ok ? QStringLiteral("已锁屏")
                                            : QStringLiteral("用户会话执行 LockWorkStation 失败（%1）")
                                                  .arg(msg["detail"].toString()) } });
                    } else if (sysActionPending_ && act == sysActionPendingAction_
                               && (act == QLatin1String("logout") || act == QLatin1String("reboot")
                                   || act == QLatin1String("poweroff"))) {
                        // [SYS-ACT] helper 用 shutdown 的真实退出码回 ok。
                        // logout 例外：退出码 0 只说明命令被交互会话接受，此时会话
                        // 往往还没开始销毁（XP 实测：ACK 后 ~0.6s 才出现注销/登录
                        // 界面，explorer 要 8~10s 才退出）。先不下结论，交给
                        // beginLogoutConfirmation 在 ≤3s 内等"会话开始销毁"的证据；
                        // 拿不到就回 unconfirmed（绝不再凭退出码报成功）。
                        const QString cid = sysActionPendingClientId_;
                        const QString actName = sysActionPendingAction_;
                        const QString label = actName == QLatin1String("logout")
                                                  ? QStringLiteral("注销")
                                                  : actName == QLatin1String("reboot")
                                                        ? QStringLiteral("重启")
                                                        : QStringLiteral("关机");
                        const QString detail = msg["detail"].toString();
#ifdef _WIN32
                        // 只有 Windows 的 shutdown /l 存在"退出码 0 但会话还没开始
                        // 销毁"的窗口，需要额外确认；其它平台维持原样。
                        if (ok && actName == QLatin1String("logout")) {
                            beginLogoutConfirmation(cid, sysActionPendingSeq_);
                            return;
                        }
#endif
                        sysActionPending_ = false;
                        sysActionPendingClientId_.clear();
                        sysActionPendingAction_.clear();
                        wsServer_->sendJson(cid, QJsonObject {
                            { "type", actName + QStringLiteral("_result") },
                            { "ok", ok },
                            { "reason", ok ? QStringLiteral("initiated")
                                           : actName + QStringLiteral("_failed") },
                            { "message", ok ? (detail.isEmpty()
                                                   ? QStringLiteral("已发起%1").arg(label)
                                                   : detail)
                                            : QStringLiteral("%1失败：%2")
                                                  .arg(label)
                                                  .arg(detail.isEmpty() ? QStringLiteral("shutdown 返回失败")
                                                                        : detail) } });
                    }
                    return;
                } else if (t == "capture_adapt") {
                    // [ADAPT] helper 的慢机自适应结果。以前只记日志：结果只活在 helper
                    // 的局部变量里，任何一次 config 重下发（切模式/改画质/重连）都会把
                    // 它冲回用户配置值（实测 XP 切一次 ffmpeg 就从 1386×838@10fps 弹回
                    // 1732×1048@15fps），而 helper 的 adaptDecided 已置真不会再降一次。
                    // 现在把它存成服务端的上限（运行期，不落盘），从此所有下发值都过一遍。
                    const int aFps = msg["fps"].toInt();
                    const int aScale = msg["scale"].toInt();
                    const double aPace = msg["paceFactor"].toDouble();
                    if (aFps >= 1 && aFps <= 60)
                        adaptFpsCap_ = (adaptFpsCap_ > 0) ? qMin(adaptFpsCap_, aFps) : aFps;
                    if (aScale >= 10 && aScale <= 100)
                        adaptScaleCap_ = (adaptScaleCap_ > 0) ? qMin(adaptScaleCap_, aScale) : aScale;
                    if (aPace >= 1.0 && aPace <= 12.0)
                        adaptPaceFactor_ = qMax(adaptPaceFactor_, aPace);
                    // 立刻把生效值压到上限：服务端后续给 helper 的 config（sendVideoParams-
                    // ToCaptureSource）和给新客户端的状态报告都取这两个成员。
                    const int prevFps = configFps_, prevScale = configScale_;
                    configFps_ = cappedFps(userFps_);
                    configScale_ = cappedScale(configScaleWish_);
                    // 记下这套上限测于哪个采集几何：几何一变就松开（见成员声明处说明）。
                    adaptGeomW_ = lastScreenInfo_["width"].toInt();
                    adaptGeomH_ = lastScreenInfo_["height"].toInt();
                    if (screenCapturer_ && configFps_ != prevFps)
                        screenCapturer_->setFps(configFps_);
                    if (jpegCompressor_ && configScale_ != prevScale)
                        jpegCompressor_->setScalePercent(configScale_);
                    qInfo() << "Capture adapted: avgCapMs =" << msg["avgCapMs"].toInt()
                            << "fps =" << configFps_ << "(cap" << adaptFpsCap_ << ")"
                            << "scale =" << configScale_ << "(cap" << adaptScaleCap_ << ")"
                            << "paceFactor =" << cappedPaceFactor();
                    // 前端据此把 fps/画质标签改成"实际生效"值，否则用户看到 15fps
                    // 实际只有 10fps，只会以为又卡了。
                    if (wsServer_ && wsServer_->clientCount() != 0) {
                        QJsonObject out;
                        out["type"] = "config_adapted";
                        out["avgCapMs"] = msg["avgCapMs"].toInt();
                        out["fps"] = configFps_;
                        out["scale"] = configScale_;
                        wsServer_->broadcastJson(out);
                    }
                    return;
                } else if (t == "capture_caps") {
                    // helper 视频能力上报：仅服务端内部决策用，绝不能泄漏给浏览器
                    captureVideoCapable_ = msg["video"].toBool();
                    captureHwEncode_ = msg["hwEncode"].toBool();
                    const QString codec = msg["codec"].toString();
                    if (!codec.isEmpty())
                        captureCodecName_ = codec;
                    qInfo() << "Service: capture caps video =" << captureVideoCapable_
                            << "hw =" << captureHwEncode_ << "codec =" << captureCodecName_
                            << "reason =" << msg["reason"].toString();
                    // 能力变化 → 重新广播模式信息，前端据此更新 videoSupported/hwEncode
                    if (wsServer_->clientCount() != 0) {
                        QJsonObject mode;
                        mode["type"] = "mode_changed";
                        mode["mode"] = (currentMode_ == ServerMode::Video) ? "video" : "image";
                        mode["hwEncode"] = hwEncodeAvailable();
                        mode["videoSupported"] = videoModeSupported();
                        mode["codec"] = captureCodecName_;
                        mode["webrtc"] = remoteVideoSourceAvailable();
                        wsServer_->broadcastJson(mode);
                    }
                    // helper 明确表示无法编码 → 若正在视频模式则回退图片
                    if (!captureVideoCapable_ && currentMode_ == ServerMode::Video)
                        switchToImageMode();
                    return;
                } else if (t == "capture_outputs") {
                    // [多屏] helper 的采集输出列表：服务端只存快照，在新客户端接入时
                    // 随 server_config 下发（这里广播会让前端收到一条不完整的配置，
                    // 把画质/帧率标签清成默认值）。绝不透传给浏览器。
                    lastCaptureOutputs_ = msg["outputs"].toArray();
                    lastCaptureOutputIdx_ = msg["currentOutput"].toInt();
                    return;
                } else if (t == "capture_hb") {
                    // [N2] helper 保活心跳：仅刷新 /capture 空闲计时（websocketserver
                    // 收到任何上行文本都已刷新），绝不能转发给浏览器。
                    return;
                } else if (t == "codec_config") {
                    // helper 上报 SPS/PPS：缓存供新客户端补发，然后照常广播给浏览器
                    lastCodecExtra_ = QByteArray::fromBase64(msg["extradata"].toString().toLatin1());
                }
                wsServer_->broadcastJson(msg);
            });
        connect(wsServer_.get(), &WebSocketServer::captureSourceConnected,
            this, [this]() {
                bool hasClients = wsServer_->clientCount() != 0;
                qInfo() << "Service: capture source connected, hasClients =" << hasClients;
                // [N1] helper 自带剪贴板通道，挂起 service 进程的 xclip 轮询，
                // 避免双通道重复广播 + 无谓的轮询进程开销
                clipSuspendedByHelper_ = true;
                updateClipboardSuspend();
                QJsonObject msg;
                msg["type"] = "capture_control";
                msg["action"] = hasClients ? "resume" : "pause";
                wsServer_->sendToCaptureSource(msg);
                // helper 重连：若服务端仍处于视频模式，需重新下发视频参数并进入编码，
                // 否则 helper 会回 JPEG 而服务端仍在等 H.264 → 画面冻结。
                if (currentMode_ == ServerMode::Video && captureVideoCapable_)
                    sendVideoParamsToCaptureSource(QStringLiteral("video_on"));
                // helper 重连：补发掉线期间积压的 keyup（只针对本产品记录过的键）。
                flushPendingKeyReleases();
            });
        connect(wsServer_.get(), &WebSocketServer::captureSourceDisconnected,
            this, [this]() {
                // [N1] helper 断开 → 恢复 service 通道剪贴板同步
                clipSuspendedByHelper_ = false;
                updateClipboardSuspend();
                captureVideoCapable_ = false;
                captureHwEncode_ = false;
                // helper 掉线：把各客户端当前按下的键移入待补发队列（重连后 keyup），
                // 否则客户端若在此期间崩溃/断线，回放会被丢弃 → 键卡死。
                movePressedKeysToPending();
                // [LOCK] helper 在等 lock 回执时掉线：本次锁屏请求已经不可能被确认，
                // 立即如实回 + 清等待位（否则 3s 定时器还会再发一次兜底，制造噪声）。
                if (lockPending_) {
                    lockPending_ = false;
                    const QString cid = lockPendingClientId_;
                    lockPendingClientId_.clear();
                    qWarning() << "Lock: helper disconnected while waiting ack";
                    wsServer_->sendJson(cid, QJsonObject {
                        { "type", "lock_result" }, { "ok", false },
                        { "reason", "helper_lost" },
                        { "message", "helper 在锁屏过程中断开，锁屏结果未知" } });
                }
                // [SYS-ACT] 注销/重启/关机在等回执时 helper 掉线：动作可能已经发出
                // （helper 先执行再 ack），也可能没执行 —— 如实回"结果未知"，
                // 不要猜成成功（更不能出现"界面报已关机、机器没动"）。
                // logout 例外：helper 已经 ack 成功、正在等"会话销毁"证据时，
                // helper 随会话被销毁而掉线是预期现象，不能在这里抢答 helper_lost，
                // 让 beginLogoutConfirmation 继续按 LogonUI/explorer 证据判定。
                if (sysActionPending_
                    && !(logoutConfirmPending_
                         && sysActionPendingAction_ == QLatin1String("logout"))) {
                    sysActionPending_ = false;
                    const QString cid = sysActionPendingClientId_;
                    const QString actName = sysActionPendingAction_;
                    sysActionPendingClientId_.clear();
                    sysActionPendingAction_.clear();
                    const QString label = actName == QLatin1String("logout")
                                              ? QStringLiteral("注销")
                                              : actName == QLatin1String("reboot")
                                                    ? QStringLiteral("重启")
                                                    : QStringLiteral("关机");
                    qWarning() << "SysAction:" << actName
                               << "helper disconnected while waiting ack";
                    wsServer_->sendJson(cid, QJsonObject {
                        { "type", actName + QStringLiteral("_result") }, { "ok", false },
                        { "reason", "helper_lost" },
                        { "message", QStringLiteral("helper 在%1过程中断开，操作可能已发出，"
                                                    "结果未知").arg(label) } });
                }
                // helper 掉线且当前在视频模式：回落图片模式并通知前端（否则画面冻结无提示）
                if (currentMode_ == ServerMode::Video)
                    switchToImageMode();
            });
    }

    // 初始化输入管理器
    inputManager_ = std::unique_ptr<InputManager>(new InputManager(this));

    qInfo() << "RDP Server initialized on port" << httpPort_;
    return true;
}

// 例如在 VideoEncoder 发出 codecConfigChanged 信号的槽中
void RDPServer::onCodecConfigChanged(const QByteArray& extradata)
{
    lastCodecExtra_ = extradata;
    QJsonObject obj;
    obj["type"] = "codec_config";
    obj["extradata"] = QString::fromLatin1(extradata.toBase64());
    wsServer_->broadcastJson(obj);
}

QStringList RDPServer::getLocalIpAddr()
{
    QStringList ipList;
    QList<QNetworkInterface> interfaceList = QNetworkInterface::allInterfaces();
    foreach (QNetworkInterface interfaceItem, interfaceList) {
        if (interfaceItem.flags().testFlag(QNetworkInterface::IsUp)
            && interfaceItem.flags().testFlag(QNetworkInterface::IsRunning)
            && interfaceItem.flags().testFlag(QNetworkInterface::CanBroadcast)
            && interfaceItem.flags().testFlag(QNetworkInterface::CanMulticast)
            && !interfaceItem.flags().testFlag(QNetworkInterface::IsLoopBack)
            && !interfaceItem.humanReadableName().contains("VirtualBox", Qt::CaseInsensitive)
            && !interfaceItem.humanReadableName().contains("VMware", Qt::CaseInsensitive)) {
            QList<QNetworkAddressEntry> addressEntryList = interfaceItem.addressEntries();

            foreach (QNetworkAddressEntry addressEntryItem, addressEntryList) {
                if (addressEntryItem.ip().protocol() == QAbstractSocket::IPv4Protocol) {
                    ipList.append(addressEntryItem.ip().toString());
                }
            }
        }
    }

    if (ipList.size() == 0) {
        ipList.append("127.0.0.1");
    }

    return ipList;
}

bool RDPServer::setupHttpServer()
{
    httpServer_ = std::unique_ptr<SslTcpServer>(new SslTcpServer(this));

    const quint16 httpPort = httpPort_;

    // 端口绑定失败按致命处理：HTTP 承载登录页 / 文件下载直链 / shell API，
    // 挂了整个服务等于不可用。旧实现只 qWarning 后继续跑，只剩 WS 半个通道，
    // 用户看到的现象是“服务在跑但网页打不开”，排障时极易误判为前端问题。
    if (!httpServer_->listen(QHostAddress::Any, httpPort)) {
        qCritical() << "HTTP server failed to listen on port" << httpPort
                    << "-" << httpServer_->errorString();
        return false;
    }

    foreach (const QString& ip, getLocalIpAddr()) {
        qInfo() << QString("listen  http%1://%2:%3").arg(useSsl_ ? "s" : "").arg(ip).arg(httpPort);
    }
    return true;
}

void RDPServer::handleIncomingSslConnection(qintptr socketDescriptor)
{
    // HTTP 空闲连接兜底：监听端口对内网开放，任何半开连接（SYN 后不再发请求、
    // 或握手后不发数据）都会常驻一个 socket + 缓冲，长期累积耗尽句柄。
    // 30s 内没有完成请求/没有新数据就断开（下载/上传路径自身会持续有字节流动，
    // 且成功接管后会 disconnect/切换角色，不受影响）。
    auto armIdleTimeout = [](QTcpSocket* s) {
        QTimer* idle = new QTimer(s);
        idle->setObjectName(QStringLiteral("httpIdle"));
        idle->setSingleShot(true);
        idle->start(30000);
        QObject::connect(idle, &QTimer::timeout, s, [s]() {
            if (s->state() == QAbstractSocket::ConnectedState)
                s->abort();
        });
    };

    if (useSsl_ && QSslSocket::supportsSsl() && sslConfiguration_) {
        QSslSocket* sslSocket = new QSslSocket(this);
        if (!sslSocket->setSocketDescriptor(socketDescriptor)) {
            qWarning() << "Failed to set socket descriptor to QSslSocket";
            delete sslSocket;
            return;
        }
        sslSocket->setSslConfiguration(*sslConfiguration_);
        armIdleTimeout(sslSocket);
        // 服务端握手：在 incomingConnection 内同步启动最稳妥——此刻尚未返回事件循环，
        // 不会有任何 readyRead 抢先读走 ClientHello（否则明文 ClientHello 进了普通缓冲、
        // TLS 层收不到，握手会卡死）。OpenSSL 已与 Qt QSslSocket 统一为 1.0.x，底层 BIO
        // 读通知即时可用，无需再延迟到事件循环或用 readyRead 触发。
        sslSocket->startServerEncryption();
        qInfo() << "SSL: startServerEncryption (useSsl=" << useSsl_
                << " supportsSsl=" << QSslSocket::supportsSsl() << ")";
        connect(sslSocket, &QSslSocket::readyRead, this, &RDPServer::onHttpRequest);
        connect(sslSocket, &QSslSocket::encrypted, this, [this, sslSocket]() {
            if (sslSocket->bytesAvailable() > 0)
                onHttpRequest();
        });
        // 自签名证书会触发 sslErrors；与 helper / secure-input 客户端一致，
        // 服务端忽略之以完成握手（浏览器仍会提示自签名警告，用户手动信任一次即可）。
        // 原先此处直接 disconnectFromHost() 会在 ServerHello 前掐断握手，导致 HTTPS 页面打不开。
        connect(sslSocket, QOverload<const QList<QSslError>&>::of(&QSslSocket::sslErrors),
            [sslSocket](const QList<QSslError>& errors) {
                qWarning() << "SSL errors (ignored, self-signed cert):" << errors;
                sslSocket->ignoreSslErrors();
            });
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
        connect(sslSocket, &QAbstractSocket::errorOccurred,
#else
        connect(sslSocket, QOverload<QAbstractSocket::SocketError>::of(&QAbstractSocket::error),
#endif
            [sslSocket](QAbstractSocket::SocketError) {
                qWarning() << "HTTPS socket error:" << sslSocket->errorString();
            });
        connect(sslSocket, &QSslSocket::disconnected, sslSocket, &QSslSocket::deleteLater);
    } else {
        QTcpSocket* tcpSocket = new QTcpSocket(this);
        if (!tcpSocket->setSocketDescriptor(socketDescriptor)) {
            qWarning() << "Failed to set socket descriptor to QTcpSocket";
            delete tcpSocket;
            return;
        }
        armIdleTimeout(tcpSocket);
        connect(tcpSocket, &QTcpSocket::readyRead, this, &RDPServer::onHttpRequest);
        connect(tcpSocket, &QTcpSocket::disconnected, tcpSocket, &QTcpSocket::deleteLater);
    }
}

void RDPServer::onHttpNewConnection()
{
    QTcpSocket* socket = httpServer_->nextPendingConnection();
    if (!socket)
        return;

    // 非 SSL 监听（HTTP 端口）同样要有空闲兜底
    QTimer* idle = new QTimer(socket);
    idle->setObjectName(QStringLiteral("httpIdle"));
    idle->setSingleShot(true);
    idle->start(30000);
    connect(idle, &QTimer::timeout, socket, [socket]() {
        if (socket->state() == QAbstractSocket::ConnectedState)
            socket->abort();
    });

    connect(socket, &QTcpSocket::readyRead, this, &RDPServer::onHttpRequest);
    connect(socket, &QTcpSocket::disconnected, socket, &QTcpSocket::deleteLater);
}

QByteArray RDPServer::loadHtmlResource()
{
    QMutexLocker locker(&htmlCacheMutex_);
    if (!htmlCache_.isEmpty())
        return htmlCache_;

    // 从 Qt 资源系统加载 HTML
    QFile file(":/res/html/index.html");
    if (file.open(QIODevice::ReadOnly)) {
        QByteArray html = file.readAll();
        file.close();

        html = html.replace("ws://", QString("ws%1://").arg(sslConfiguration_ ? "s" : "").toUtf8());
        html = html.replace("websocketPort", QString("%1").arg(wsPort_).toUtf8());

        // 页面标题：优先用配置的 systemName（多台被控机靠它区分标签页），
        // 未配置则回退主机名（如 localhost.localdomain 这类无辨识度输出）。
        html = html.replace("__REMOTE_HOSTNAME__",
                            effectiveSystemName().toHtmlEscaped().toUtf8());

        htmlCache_ = html;
        return htmlCache_;
    }

    qWarning() << "Failed to load HTML from Qt resources";

    // 如果资源加载失败，返回简单错误页面
    return R"(<!DOCTYPE html>
<html>
<head><title>Error</title></head>
<body>
    <h1>无法加载页面</h1>
    <p>资源文件未找到，请确保正确编译了资源。</p>
</body>
</html>)";
}

QByteArray RDPServer::loadLoginHtml()
{
    QFile file(":/res/html/login.html");
    if (file.open(QIODevice::ReadOnly)) {
        return file.readAll();
    }
    return R"(<!DOCTYPE html>
<html><head><title>Login</title></head>
<body><h1>Login page not found</h1></body></html>)";
}

QByteArray RDPServer::buildHttpResponse(int statusCode, const QString& statusText,
    const QString& contentType, const QByteArray& body,
    const QString& extraHeaders)
{
    QByteArray resp;
    resp += "HTTP/1.1 " + QByteArray::number(statusCode) + " " + statusText.toUtf8() + "\r\n";
    resp += "Content-Type: " + contentType.toUtf8() + "\r\n";
    resp += "Content-Length: " + QByteArray::number(body.size()) + "\r\n";
    resp += "Access-Control-Allow-Origin: *\r\n";
    if (!extraHeaders.isEmpty())
        resp += extraHeaders.toUtf8() + "\r\n";
    resp += "Connection: close\r\n";
    resp += "\r\n";
    resp += body;
    return resp;
}

// Content-Disposition 文件名安全化：文件名是远程主上的任意 UTF-8 串，
// 直接拼接进响应头会（1）被 CR/LF 注入伪造响应头、（2）被引号截断。
// 这里把控制字符与引号/反斜杠替换为下划线；非 ASCII 用 RFC5987 filename* 另送一份。
static QString httpSafeFilename(const QString& name)
{
    QString out;
    out.reserve(name.size());
    for (const QChar c : name) {
        const ushort u = c.unicode();
        if (u < 32 || u == 127 || c == '"' || c == '\\' || c == '\r' || c == '\n')
            out += QLatin1Char('_');
        else
            out += c;
    }
    return out;
}

// GET /api/file?path=<受限根内的路径> —— 远程文件直链下载
//
// 为什么需要它：前端原来的"拖到本机"必须经 WebSocket 把整个文件拉进浏览器内存，
// 再拼成 Blob 用 DownloadURL 交给系统。浏览器对一个 blob: URL 的可拖出性、
// 内存占用（>32MB 直接被预制上限拦掉）、以及目录（只能先打成 tar）都有硬限制。
// 提供标准 HTTP GET 后：
//   - Chrome/Edge 支持 `DownloadURL` 指向 http(s) 直链，拖到桌面由浏览器后台接管下载；
//   - 不受预取上限限制，超大文件也能拖出；
//   - 支持 Range，浏览器/下载工具可断点续传；
//   - 鉴权沿用 cookie `session`（同源请求自动携带），不额外开认证口子。
void RDPServer::handleApiFileDownload(QTcpSocket* socket, const QString& path, const QString& headerText)
{
    auto fail = [&](int code, const QString& text, const char* detail) {
        QByteArray body = detail ? QByteArray(detail) : text.toUtf8();
        QByteArray resp = buildHttpResponse(code, text, "text/plain; charset=utf-8", body);
        socket->write(resp);
        socket->flush();
        socket->disconnectFromHost();
    };

    const int qIdx = path.indexOf('?');
    if (qIdx < 0) {
        fail(400, "Bad Request", "missing path parameter");
        return;
    }
    // FullyDecoded：path 值本身是百分号编码的 UTF-8 路径，不能再被二次转义误解
    QUrlQuery query(path.mid(qIdx + 1));
    const QString rawPath = query.queryItemValue("path", QUrl::FullyDecoded);
    if (rawPath.isEmpty()) {
        fail(400, "Bad Request", "missing path parameter");
        return;
    }

    // 与 WS 文件传输共用同一份根目录越权校验
    const QString safePath = FileTransferService::sanitizeFilePath(rawPath);
    if (safePath.isEmpty()) {
        qWarning() << "HTTP download rejected (outside file root):" << rawPath
                   << "root=" << FileTransferService::rootPath();
        fail(403, "Forbidden", "path outside allowed file root");
        return;
    }

    QFileInfo fi(safePath);
    if (!fi.exists()) {
        fail(404, "Not Found", "file not found");
        return;
    }

    // 状态必须在堆上：QHash 内部按值拷贝节点，而这里持有独占的文件句柄
    HttpDownloadState* st = new HttpDownloadState;
    QString displayName;

    if (fi.isDir()) {
        // 目录：tar 打包（保留目录树）。[P1 perf] 改为流式生成：此处只收集条目
        // 清单（每条几百字节），文件内容在泵送阶段按需读盘——GB 级目录不再
        // 整树 readAll 进内存（旧行为 = OOM + 主线程冻结）。tar 总大小可精确
        // 预估，Content-Length 依旧准确。不支持 Range（流式生成器不可随机
        // 寻址），浏览器对无 Range 响应整体下载即可。
        qint64 tarTotal = 0;
        st->tarQueue = FileTransferService::collectTarEntries(safePath, &tarTotal);
        if (tarTotal < 0) {
            delete st;
            fail(500, "Internal Server Error", "directory has too many entries for tar download");
            return;
        }
        st->tarStream = true;
        st->totalSize = tarTotal;
        displayName = fi.fileName() + ".tar";
    } else {
        if (!fi.isFile() || !fi.isReadable()) {
            delete st;
            fail(403, "Forbidden", "not a readable file");
            return;
        }
        st->file.reset(new QFile(safePath));
        if (!st->file->open(QIODevice::ReadOnly)) {
            delete st;
            fail(500, "Internal Server Error", "cannot open file");
            return;
        }
        st->totalSize = st->file->size();
        displayName = fi.fileName();
    }

    // 空文件：Range 的最小尾端会变成 -1，若走通用分支会被误判成 416；单独处理
    if (st->totalSize == 0) {
        delete st;
        QByteArray h = "HTTP/1.1 200 OK\r\n"
                       "Content-Type: application/octet-stream\r\n"
                       "Content-Length: 0\r\n"
            + QByteArray("Content-Disposition: attachment; filename=\"") + httpSafeFilename(displayName).toUtf8()
            + "\"; filename*=UTF-8''" + QUrl::toPercentEncoding(displayName, "~-_./") + "\r\n"
            + "Cache-Control: no-store\r\nConnection: close\r\n\r\n";
        socket->write(h);
        socket->flush();
        socket->disconnectFromHost();
        return;
    }

    // Range 支持（单区间）：浏览器拖动下载大文件/断点续传会带 Range
    qint64 start = 0;
    qint64 end = st->totalSize - 1;
    bool partial = false;
    // 提前建立自清理：任何后续错误返回都不得泄漏这块状态（含打开的文件句柄）。
    // 成功路径最后由 pumpFileDownload 收尾，同样会走到 cleanupHttpDownload。
    connect(socket, &QObject::destroyed, this, [this, socket]() {
        cleanupHttpDownload(socket);
    });
    connect(socket, &QTcpSocket::bytesWritten, this, [this, socket]() {
        pumpFileDownload(socket);
    });
    // 按行精确匹配请求头名：不能直接用 headerText.indexOf("Range:")，否则
    // "If-Range:"（合法的条件请求头，浏览器缓存重验证会自动带上）也会命中，
    // 随后把 If-Range 里的 ETag/日期当区间解析，产生错误的 206/416 或整段偏移。
    auto headerValue = [headerText](const char* name) -> QString {
        const QString key = QString::fromLatin1(name);
        int pos = 0;
        while (pos < headerText.size()) {
            int nl = headerText.indexOf('\n', pos);
            if (nl < 0)
                nl = headerText.size();
            const QString line = headerText.mid(pos, nl - pos).trimmed();
            if (line.size() > key.size() && line.at(key.size()) == ':'
                && line.startsWith(key, Qt::CaseInsensitive))
                return line.mid(key.size() + 1).trimmed();
            pos = nl + 1;
        }
        return QString();
    };
    const QString rangeValue = headerValue("Range");
    if (!rangeValue.isEmpty()) {
        QString spec = rangeValue;
        if (spec.startsWith("bytes=", Qt::CaseInsensitive)) {
            QString ranged = spec.mid(6).trimmed();
            const int dash = ranged.indexOf('-');
            bool okStart = false;
            if (dash >= 0) {
                const QString s = ranged.left(dash).trimmed();
                const QString e = ranged.mid(dash + 1).trimmed();
                if (!s.isEmpty()) {
                    start = s.toLongLong(&okStart);
                    // "bytes=N-M" / "bytes=N-"：只有 M 存在时才覆盖 end，
                    // 缺省保持 totalSize-1（由调用方初始值保证）。
                    if (okStart && !e.isEmpty()) {
                        bool okEnd = false;
                        const qint64 ev = e.toLongLong(&okEnd);
                        if (okEnd)
                            end = qMin(ev, st->totalSize - 1);
                    }
                } else {
                    // [FIX] "bytes=-N" → 末尾 N 字节，end 必须恒为 totalSize-1。
                    // 旧代码在此分支后又用同一个 e(=N) 去设 end，把区间算成
                    // [total-N, N]：文件稍大（total > 2N）就 end<start → 误返 416，
                    // 小文件则返回错误切片，断点续传/尾部请求全错。
                    bool okTail = false;
                    const qint64 tail = e.toLongLong(&okTail);
                    if (okTail && tail > 0) {
                        start = qMax<qint64>(0, st->totalSize - tail);
                        okStart = true;
                    }
                }
            }
            if (!okStart)
                start = 0;
        }
    }
    if (start < 0 || start >= st->totalSize || end < start) {
        // 不可满足的区间
        QByteArray head = "HTTP/1.1 416 Range Not Satisfiable\r\n";
        head += "Content-Range: bytes */" + QByteArray::number(st->totalSize) + "\r\n";
        head += "Content-Length: 0\r\nConnection: close\r\n\r\n";
        delete st;
        socket->write(head);
        socket->flush();
        socket->disconnectFromHost();
        return;
    }
    end = qMin(end, st->totalSize - 1);
    partial = (start != 0 || end != st->totalSize - 1);
    if (st->tarStream) {
        // tar 流式生成不可随机寻址：忽略 Range，整体 200 下载
        start = 0;
        end = st->totalSize - 1;
        partial = false;
    }
    st->offset = start;
    st->end = end;
    if (st->file && !st->file->seek(start)) {
        delete st;
        fail(500, "Internal Server Error", "cannot seek file");
        return;
    }

    const QString safeName = httpSafeFilename(displayName);
    const QByteArray encodedName = QUrl::toPercentEncoding(displayName, "~-_./");

    QByteArray head;
    head += partial ? "HTTP/1.1 206 Partial Content\r\n" : "HTTP/1.1 200 OK\r\n";
    head += "Content-Type: application/octet-stream\r\n";
    head += "Content-Length: " + QByteArray::number(end - start + 1) + "\r\n";
    // 分块可寻址的单文件才声明支持 Range；目录 tar 是流式生成、不可随机寻址，
    // 声明 Accept-Ranges 会诱导浏览器/下载器发 Range 请求，随后被忽略（浪费一次往返，
    // 断点续传工具还会因"服务端忽略 Range"反复重试）。
    if (!st->tarStream)
        head += "Accept-Ranges: bytes\r\n";
    if (partial)
        head += "Content-Range: bytes " + QByteArray::number(start) + "-"
            + QByteArray::number(end) + "/" + QByteArray::number(st->totalSize) + "\r\n";
    head += "Content-Disposition: attachment; filename=\"" + safeName.toUtf8()
        + "\"; filename*=UTF-8''" + encodedName + "\r\n";
    head += "Cache-Control: no-store\r\n";
    head += "Access-Control-Allow-Origin: *\r\n";
    head += "Connection: close\r\n\r\n";

    // HEAD：只回元信息（用于前端探活/取大小），不写正文
    const bool headOnly = headerText.startsWith("HEAD");

    socket->write(head);
    if (headOnly) {
        // HEAD 没有正文，状态在这里就地释放
        delete st;
        socket->flush();
        socket->disconnectFromHost();
        return;
    }

    // 不能用 QHash::insert(key,value) 之外的按值存储：状态含独占句柄，这里存指针
    httpDownloadStates_.insert(socket, st);

    // 首泵：能一次写完就直接结束，写不完由 bytesWritten 续泵
    pumpFileDownload(socket);
}

void RDPServer::pumpFileDownload(QTcpSocket* socket)
{
    auto it = httpDownloadStates_.find(socket);
    if (it == httpDownloadStates_.end())
        return;
    if (!socket->isOpen()) {
        cleanupHttpDownload(socket);
        return;
    }

    static const qint64 kChunk = 256 * 1024;
    // 背压阈值：待写字节超过 1MB 就停手，等 bytesWritten 再继续，
    // 避免把数 GB 文件一次性塞进 Qt 的写队列（内存暴涨 + 主线程卡顿）。
    static const qint64 kBacklogLimit = 1024 * 1024;

    HttpDownloadState* st = it.value();
    while (st->offset <= st->end) {
        if (socket->bytesToWrite() > kBacklogLimit)
            return;
        const int n = (int)qMin(kChunk, st->end - st->offset + 1);
        QByteArray buf;
        if (st->tarStream)
            buf = generateTarChunk(st, n);
        else if (st->file)
            buf = st->file->read(n);
        else
            buf = st->payload.mid((int)st->offset, n);
        if (buf.isEmpty()) {
            // 读失败不是正常 EOF：不打日志的话表现为"浏览器下载截断"却无从排查
            if (st->file && st->file->error() != QFileDevice::NoError)
                qWarning() << "File download read error:" << st->file->errorString()
                           << "at offset" << st->offset << "/" << st->totalSize;
            break;
        }
        // 写失败必须立即终止：旧实现忽略 write() 返回值、无条件推进 offset，
        // 客户端断开后 QTcpSocket::write() 恒返回 -1，循环仍会把整个文件从磁盘
        // 读完（GB 级文件 = 磁盘空转 + 主线程长时间占用），且浏览器只收到截断内容。
        const qint64 written = socket->write(buf);
        if (written != buf.size()) {
            if (written < 0)
                qWarning() << "File download write failed:" << socket->errorString()
                           << "at offset" << st->offset << "/" << st->totalSize;
            else
                qWarning() << "File download short write:" << written << "/" << buf.size()
                           << "at offset" << st->offset << "/" << st->totalSize;
            cleanupHttpDownload(socket);
            socket->disconnectFromHost();
            return;
        }
        st->offset += written;
    }

    cleanupHttpDownload(socket);
    socket->disconnectFromHost();
}

// 目录 tar 流式生成：按需产出最多 maxBytes 字节，状态机推进 tarQueue。
// 产出总长度与 collectTarEntries 计算的 totalSize 严格一致——读失败/提前 EOF
// 用零补齐，绝不短于 Content-Length（否则浏览器会一直挂起等待）。
QByteArray RDPServer::generateTarChunk(HttpDownloadState* st, int maxBytes)
{
    // 结束块一次就要写 1024 字节（两个 512B 全零块），所以缓冲必须 ≥1024：
    // 旧实现是 char[512]，而 append(kZeros, tarPadRemaining=1024) 会越过数组
    // 读相邻 .rodata —— tar 尾部混入 512 字节只读数据（信息泄漏），归档也
    // 只剩一个合法结束块（tar 报 "lone zero block"）。take 再夹一层 sizeof
    // 兜底，防止日后调大 pad 时重新越界。
    static const char kZeros[1024] = { 0 };
    QByteArray out;
    out.reserve(maxBytes);

    while (out.size() < maxBytes) {
        if (!st->pendingHeader.isEmpty()) {
            const int take = qMin<int>(st->pendingHeader.size(), maxBytes - out.size());
            out.append(st->pendingHeader.constData(), take);
            st->pendingHeader.remove(0, take);
            continue;
        }
        if (st->tarPadRemaining > 0) {
            const int take = qMin(st->tarPadRemaining,
                                  qMin(maxBytes - out.size(), int(sizeof(kZeros))));
            out.append(kZeros, take);
            st->tarPadRemaining -= take;
            continue;
        }
        if (st->tarFileRemaining > 0) {
            const qint64 want = qMin<qint64>(maxBytes - out.size(), st->tarFileRemaining);
            QByteArray buf = st->file ? st->file->read(want) : QByteArray();
            if (buf.isEmpty())
                buf = QByteArray(int(want), '\0'); // 打不开/读失败：零补齐保长度
            out.append(buf);
            st->tarFileRemaining -= buf.size();
            // [C4] 数据段耗尽即计算 512B 对齐填充——不能放在"file 收尾"分支：
            // open 失败时 file 为空，填充会被跳过 → 后续成员整体错位、tar 流损坏
            if (st->tarFileRemaining <= 0)
                st->tarPadRemaining = int((512 - (st->curEntrySize % 512)) % 512);
            continue;
        }
        if (st->file) {
            // 当前成员文件读完 → 释放句柄（填充已在数据段耗尽处设置）
            st->file.reset();
            continue;
        }
        if (st->tarQueueIdx >= st->tarQueue.size()) {
            if (!st->tarEndSent) {
                st->tarEndSent = true;
                st->tarPadRemaining = 1024; // tar 结束块
                continue;
            }
            break; // 生成完毕
        }
        const FileTransferService::TarEntry& e = st->tarQueue[st->tarQueueIdx++];
        if (e.isDir) {
            st->pendingHeader = FileTransferService::tarHeaderFor(
                e.tarName + QLatin1Char('/'), 0, '5');
        } else {
            st->pendingHeader = FileTransferService::tarHeaderFor(e.tarName, e.size, '0');
            st->curEntrySize = e.size;
            st->tarFileRemaining = e.size;
            if (e.size > 0) {
                // 用 unique_ptr(new) 而非 std::make_unique：make_unique 是 C++14，
                // Qt5.7 时代的旧编译器（GCC4.9/MSVC2013）不可用；unique_ptr 构造为 C++11
                std::unique_ptr<QFile> f(new QFile(e.absPath));
                if (!f->open(QIODevice::ReadOnly)) {
                    qWarning() << "Tar stream: cannot open" << e.absPath
                               << "- zero-filling" << e.size << "bytes";
                } else {
                    st->file = std::move(f);
                }
            }
        }
    }
    return out;
}

void RDPServer::cleanupHttpDownload(QTcpSocket* socket)
{
    // 逐个 take：状态里含独占文件句柄，移除时必须真正释放，不能只摘 map 项
    HttpDownloadState* st = httpDownloadStates_.take(socket);
    delete st;
}

void RDPServer::serveLoginPage(QTcpSocket* socket)
{
    QByteArray html = loadLoginHtml();
    QByteArray resp = buildHttpResponse(200, "OK", "text/html; charset=utf-8", html);
    socket->write(resp);
    socket->flush();
    socket->disconnectFromHost();
}

void RDPServer::handleLoginPost(QTcpSocket* socket, const QByteArray& body)
{
    QJsonDocument doc = QJsonDocument::fromJson(body);
    QString error;
    QString token;

    if (doc.isObject()) {
        QJsonObject obj = doc.object();
        QString username = obj["username"].toString();
        QString password = obj["password"].toString();

        if (authManager_->validateUser(username, password)) {
            token = authManager_->createSession(username);
        } else {
            error = "用户名或密码错误";
        }
    } else {
        error = "无效的请求数据";
    }

    QJsonObject result;
    result["success"] = token.isEmpty() ? false : true;
    if (!token.isEmpty()) {
        result["token"] = token;
        // 无屏幕捕获 → 登录后直接跳转到 Shell 页面
        if (!captureAvailable_) {
            result["redirect"] = "/shell";
        }
    }
    if (!error.isEmpty())
        result["error"] = error;

    QByteArray jsonResp = QJsonDocument(result).toJson(QJsonDocument::Compact);
    // HttpOnly：会话令牌不允许 JS 读取，防 XSS 窃取（login.html 的 document.cookie
    // 兜底写法对 HttpOnly cookie 无效，不冲突——仅当服务器没发 Set-Cookie 时才有意义）
    QString cookie = QString("session=%1; path=/; max-age=86400; SameSite=Lax; HttpOnly").arg(token);
    if (useSsl_)
        cookie += "; Secure";
    QString extraHeaders = "Set-Cookie: " + cookie;
    QByteArray resp = buildHttpResponse(200, "OK", "application/json; charset=utf-8", jsonResp, extraHeaders);
    socket->write(resp);
    socket->flush();
    socket->disconnectFromHost();
}

QString RDPServer::extractSessionToken(const QByteArray& request)
{    QString req = QString::fromUtf8(request);
    // Check Cookie header
    int cookieIdx = req.indexOf("Cookie:", 0, Qt::CaseInsensitive);
    if (cookieIdx >= 0) {
        int lineEnd = req.indexOf('\n', cookieIdx);
        QString cookieLine = req.mid(cookieIdx, lineEnd - cookieIdx);
        // Extract session=...;
        int sessIdx = cookieLine.indexOf("session=", 0, Qt::CaseInsensitive);
        if (sessIdx >= 0) {
            sessIdx += 8; // skip "session="
            int endIdx = cookieLine.indexOf(';', sessIdx);
            if (endIdx < 0)
                endIdx = cookieLine.indexOf('\r', sessIdx);
            if (endIdx < 0)
                endIdx = cookieLine.indexOf('\n', sessIdx);
            if (endIdx < 0)
                endIdx = cookieLine.length();
            return cookieLine.mid(sessIdx, endIdx - sessIdx).trimmed();
        }
    }
    return QString();
}

void RDPServer::onHttpRequest()
{
    QTcpSocket* socket = qobject_cast<QTcpSocket*>(sender());
    if (!socket || !socket->isOpen())
        return;

    HttpParseState& st = httpParseState_[socket];

    // 首次见到该 socket 时注册销毁回调，socket 关闭后清理解析状态，
    // 避免 QHash 中残留悬空指针键导致内存泄漏。
    if (st.buffer.isEmpty() && !st.headerDone) {
        connect(socket, &QObject::destroyed, this, [this, socket]() {
            httpParseState_.remove(socket);
        });
    }

    // 累积本次 readyRead 到达的全部字节（不再逐行读取，便于跨 TCP 包重组）
    st.buffer += socket->readAll();

    // 收到新数据就续期，但**不能撤掉**：请求头/请求体只到一半、对端静默时
    // （半开连接），撤掉定时器会让这个 socket 永久驻留。直到请求完整后才
    // 停表删除（见下方 body 到齐处），下载路径由 pumpFileDownload 自己接管。
    if (QTimer* idle = socket->findChild<QTimer*>(QStringLiteral("httpIdle")))
        idle->start(30000);

    // 解析请求头（仅一次）：得到 Content-Length 与请求头文本
    if (!st.headerDone) {
        int headerEnd = st.buffer.indexOf("\r\n\r\n");
        int sepLen = 4;
        if (headerEnd < 0) {
            headerEnd = st.buffer.indexOf("\n\n");
            sepLen = 2;
        }
        if (headerEnd < 0) {
            // 请求头尚未到齐，等待下一次 readyRead；限制缓冲上限防止恶意请求耗尽内存
            if (st.buffer.size() > 65536) {
                QByteArray resp = buildHttpResponse(400, "Bad Request", "text/plain; charset=utf-8", "Header too large");
                socket->write(resp);
                socket->flush();
                socket->disconnectFromHost();
                httpParseState_.remove(socket);
            }
            return;
        }
        st.headerText = QString::fromUtf8(st.buffer.left(headerEnd));
        int clIdx = st.headerText.indexOf("Content-Length:", 0, Qt::CaseInsensitive);
        if (clIdx >= 0) {
            int colonIdx = st.headerText.indexOf(':', clIdx);
            int lineEnd = st.headerText.indexOf('\n', clIdx);
            st.contentLength = st.headerText.mid(colonIdx + 1, lineEnd - colonIdx - 1).trimmed().toInt();
        }
        if (st.contentLength < 0 || st.contentLength > 1048576) { // 负值或非数字会卡死 body 等待
            QByteArray resp = buildHttpResponse(400, "Bad Request", "text/plain; charset=utf-8", "invalid Content-Length");
            socket->write(resp);
            socket->flush();
            socket->disconnectFromHost();
            httpParseState_.remove(socket);
            return;
        }
        st.headerDone = true;
    }

    // 已派发过的重复 readyRead 直接返回（见下方说明），且必须在任何大块拷贝
    // 之前判断：旧代码先 mid() 复制一遍 body 才检查 dispatched，异常客户端
    // 触发 N 次 readyRead 就白做 N 次整段拷贝。
    if (st.dispatched)
        return;

    // 等待完整 POST body 到齐（不阻塞主线程，靠后续 readyRead 再次触发本函数）。
    // 只比较剩余字节数，不再每次 readyRead 用 mid() 复制已累积的 body：
    // body 分 N 段到达时旧写法共复制 O(n²) 字节（1MB 上限下可达数百 MB 拷贝，
    // 全部发生在主线程）。
    int headerEnd = st.buffer.indexOf("\r\n\r\n");
    int sepLen = 4;
    if (headerEnd < 0) {
        headerEnd = st.buffer.indexOf("\n\n");
        sepLen = 2;
    }
    int bodyOffset = headerEnd + sepLen;
    if (st.buffer.size() - bodyOffset < st.contentLength)
        return;
    QByteArray body = st.buffer.mid(bodyOffset, st.contentLength);

    // 一个请求只派发一次：响应头固定 Connection: close，但客户端若在 socket
    // 关闭前继续发数据（流水线/异常客户端），readyRead 会再次触发本函数，
    // 而 buffer 从未消费 → 同一请求被重复处理（下载状态被覆盖泄漏、登录/POST
    // 重复执行）。
    st.dispatched = true;

    // 请求已完整到达：撤掉空闲兜底。下载/上传后续字节由各自状态机驱动，
    // 不再有 readyRead，继续计时会把正常的大文件传输误杀。
    if (QTimer* idle = socket->findChild<QTimer*>(QStringLiteral("httpIdle"))) {
        idle->stop();
        idle->deleteLater();
    }

    QString requestStr = st.headerText;

    // request 供 extractSessionToken 与 GET 路径使用（只含请求头，token 在其中）。
    // 不再整份复制 st.buffer（含 body，最大 1MB）：token/path 解析只需要头部。
    QByteArray request = st.buffer.left(bodyOffset);

    // 请求头与 body 均已到齐：readPostBody 直接返回已累积的 body，不再同步阻塞
    auto readPostBody = [&](int& bodyLen) -> QByteArray {
        bodyLen = st.contentLength;
        if (bodyLen <= 0)
            return QByteArray();
        return body;
    };

    // Extract path for all requests
    int firstSpace = requestStr.indexOf(' ');
    int secondSpace = requestStr.indexOf(' ', firstSpace + 1);
    QString path = (firstSpace >= 0 && secondSpace > firstSpace) ? requestStr.mid(firstSpace + 1, secondSpace - firstSpace - 1) : "/";
    QString method = requestStr.startsWith("POST") ? "POST" : "GET";

    // === POST handlers ===
    if (method == "POST") {
        if (path == "/login" || path.startsWith("/login?")) {
            int bodyLen = 0;
            QByteArray body = readPostBody(bodyLen);
            if (body.isEmpty()) {
                QByteArray resp = buildHttpResponse(400, "Bad Request", "text/plain; charset=utf-8", "Missing request body");
                socket->write(resp);
                socket->flush();
                socket->disconnectFromHost();
                return;
            }
            handleLoginPost(socket, body);
            return;
        }

        if (path == "/api/users/add" || path.startsWith("/api/users/add?")) {
            // Require auth
            QString token = extractSessionToken(request);
            if (!authManager_->validateSession(token)) {
                QByteArray resp = buildHttpResponse(401, "Unauthorized", "application/json; charset=utf-8",
                    QJsonDocument(QJsonObject { { "success", false }, { "error", "未登录" } }).toJson(QJsonDocument::Compact));
                socket->write(resp);
                socket->flush();
                socket->disconnectFromHost();
                return;
            }
            int bodyLen = 0;
            QByteArray body = readPostBody(bodyLen);
            if (body.isEmpty()) {
                QByteArray resp = buildHttpResponse(400, "Bad Request", "application/json; charset=utf-8",
                    QJsonDocument(QJsonObject { { "success", false }, { "error", "Missing request body" } }).toJson(QJsonDocument::Compact));
                socket->write(resp);
                socket->flush();
                socket->disconnectFromHost();
                return;
            }
            handleApiAddUser(socket, body);
            return;
        }

        if (path == "/api/users/delete" || path.startsWith("/api/users/delete?")) {
            QString token = extractSessionToken(request);
            if (!authManager_->validateSession(token)) {
                QByteArray resp = buildHttpResponse(401, "Unauthorized", "application/json; charset=utf-8",
                    QJsonDocument(QJsonObject { { "success", false }, { "error", "未登录" } }).toJson(QJsonDocument::Compact));
                socket->write(resp);
                socket->flush();
                socket->disconnectFromHost();
                return;
            }
            int bodyLen = 0;
            QByteArray body = readPostBody(bodyLen);
            if (body.isEmpty()) {
                QByteArray resp = buildHttpResponse(400, "Bad Request", "application/json; charset=utf-8",
                    QJsonDocument(QJsonObject { { "success", false }, { "error", "Missing request body" } }).toJson(QJsonDocument::Compact));
                socket->write(resp);
                socket->flush();
                socket->disconnectFromHost();
                return;
            }
            handleApiDeleteUser(socket, body);
            return;
        }

        if (path == "/api/config" || path.startsWith("/api/config?")) {
            // [A8] 管理页（/shell，终端页面）里也有画质/帧率/缩放/分辨率下拉框，
            // 但它把消息发给了**终端** WebSocket（/api/shell/ws），服务端只认
            // "终端输入"和"粘贴"两种，配置消息被当成乱码静默丢弃 —— 用户点了
            // 一点反应都没有，也不报错。
            // 这里给终端页一个同源的 HTTP 入口，内部走与浏览器 WS 完全相同的
            // 处理路径（applyConfigFromHttp），配置真正生效。
            QString token = extractSessionToken(request);
            if (!authManager_->validateSession(token)) {
                QByteArray resp = buildHttpResponse(401, "Unauthorized", "application/json; charset=utf-8",
                    QJsonDocument(QJsonObject { { "success", false }, { "error", "未登录" } }).toJson(QJsonDocument::Compact));
                socket->write(resp);
                socket->flush();
                socket->disconnectFromHost();
                return;
            }
            int bodyLen = 0;
            const QByteArray body = readPostBody(bodyLen);
            const QJsonDocument doc = QJsonDocument::fromJson(body);
            if (!doc.isObject()) {
                QByteArray resp = buildHttpResponse(400, "Bad Request", "application/json; charset=utf-8",
                    QJsonDocument(QJsonObject { { "success", false }, { "error", "Invalid JSON body" } }).toJson(QJsonDocument::Compact));
                socket->write(resp);
                socket->flush();
                socket->disconnectFromHost();
                return;
            }
            QString error;
            if (!applyConfigFromHttp(doc.object(), &error)) {
                QByteArray resp = buildHttpResponse(400, "Bad Request", "application/json; charset=utf-8",
                    QJsonDocument(QJsonObject { { "success", false }, { "error", error } }).toJson(QJsonDocument::Compact));
                socket->write(resp);
                socket->flush();
                socket->disconnectFromHost();
                return;
            }
            QByteArray ok = buildHttpResponse(200, "OK", "application/json; charset=utf-8",
                QJsonDocument(QJsonObject { { "success", true } }).toJson(QJsonDocument::Compact));
            socket->write(ok);
            socket->flush();
            socket->disconnectFromHost();
            return;
        }

        if (path == "/api/shell/exec" || path.startsWith("/api/shell/exec?")) {
            QString token = extractSessionToken(request);
            if (!authManager_->validateSession(token)) {
                QByteArray resp = buildHttpResponse(401, "Unauthorized", "application/json; charset=utf-8",
                    QJsonDocument(QJsonObject { { "success", false }, { "error", "未登录" } }).toJson(QJsonDocument::Compact));
                socket->write(resp);
                socket->flush();
                socket->disconnectFromHost();
                return;
            }
            int bodyLen = 0;
            QByteArray body = readPostBody(bodyLen);
            if (body.isEmpty()) {
                QByteArray resp = buildHttpResponse(400, "Bad Request", "application/json; charset=utf-8",
                    QJsonDocument(QJsonObject { { "success", false }, { "error", "Missing request body" } }).toJson(QJsonDocument::Compact));
                socket->write(resp);
                socket->flush();
                socket->disconnectFromHost();
                return;
            }
            handleShellExec(socket, body, token);
            return;
        }

        // Unknown POST
        QByteArray resp = buildHttpResponse(404, "Not Found", "text/plain; charset=utf-8", "Not Found");
        socket->write(resp);
        socket->flush();
        socket->disconnectFromHost();
        return;
    }

    // === HEAD handlers ===
    // 只有 /api/file 支持 HEAD（前端用它探测直链是否可用/取文件大小），
    // 其余路径的 HEAD 按现状断开，避免其它路由被 HEAD 请求误触发。
    if (requestStr.startsWith("HEAD /api/file")) {
        QString token = extractSessionToken(request);
        if (!authManager_->validateSession(token)) {
            QByteArray resp = buildHttpResponse(401, "Unauthorized", "application/json; charset=utf-8",
                QJsonDocument(QJsonObject { { "success", false }, { "error", "未登录" } }).toJson(QJsonDocument::Compact));
            socket->write(resp);
            socket->flush();
            socket->disconnectFromHost();
            return;
        }
        handleApiFileDownload(socket, path, requestStr);
        return;
    }

    // === GET handlers ===
    if (!requestStr.startsWith("GET /")) {
        socket->disconnectFromHost();
        return;
    }

    // Read remaining data
    request += socket->readAll();

    // Unprotected GET routes
    if (path == "/login" || path.startsWith("/login?")) {
        QByteArray html = loadLoginHtml();
        QByteArray resp = buildHttpResponse(200, "OK", "text/html; charset=utf-8", html,
            "Cache-Control: no-cache");
        socket->write(resp);
        socket->flush();
        socket->disconnectFromHost();
        return;
    }

    // Static vendor assets (xterm.js 等)，免鉴权。JS/CSS 为静态库文件，与登录态无关
    if (path.startsWith("/vendor/")) {
        QString vendorRel = path.mid(1);
        int qIdx = vendorRel.indexOf('?');
        if (qIdx >= 0)
            vendorRel = vendorRel.left(qIdx);
        if (!vendorRel.contains("..") && !vendorRel.endsWith('/')) {
            QFile vendorFile(QString(":/res/%1").arg(vendorRel));
            if (vendorFile.open(QIODevice::ReadOnly)) {
                QString mime = "application/octet-stream";
                if (vendorRel.endsWith(".js"))
                    mime = "application/javascript; charset=utf-8";
                else if (vendorRel.endsWith(".css"))
                    mime = "text/css; charset=utf-8";
                else if (vendorRel.endsWith(".png"))
                    mime = "image/png";
                QByteArray vendorResp = buildHttpResponse(200, "OK", mime, vendorFile.readAll());
                socket->write(vendorResp);
                socket->flush();
                socket->disconnectFromHost();
                return;
            }
        }
        QByteArray nfResp = buildHttpResponse(404, "Not Found", "text/plain; charset=utf-8", "Not Found");
        socket->write(nfResp);
        socket->flush();
        socket->disconnectFromHost();
        return;
    }

    // 无屏幕捕获（无头模式）**同样要求登录**：页面、API、shell WebSocket 一律需要
    // 有效会话。此前 headless 放开静态页面免登录，但 /api/shell/ws 始终校验 token，
    // 导致无头模式下 shell 页能打开却永远连不上（前端“连接不上”）；统一走登录页
    // 拿 HttpOnly session cookie 后，同源 WS 握手自动携带即可通过校验。
    // （历史漏洞：/api/shell/exec、cwd 曾进过 skipAuth，无 DISPLAY 时免登录执行任意
    // 命令 = RCE，已由 ws/exec 侧强制鉴权封死，这里不再放行任何路径。）

    // Protected routes — require valid session
    QString token = extractSessionToken(request);
    if (!authManager_->validateSession(token)) {
        if (path.startsWith("/api/")) {
            QByteArray resp = buildHttpResponse(401, "Unauthorized", "application/json; charset=utf-8",
                QJsonDocument(QJsonObject { { "success", false }, { "error", "未登录" } }).toJson(QJsonDocument::Compact));
            socket->write(resp);
            socket->flush();
            socket->disconnectFromHost();
        } else {
            serveLoginPage(socket);
        }
        return;
    }

    // Serve API or page
    if (path == "/shell" || path.startsWith("/shell?")) {
        QFile shellFile(":/res/html/shell.html");
        QByteArray shellHtml;
        if (shellFile.open(QIODevice::ReadOnly))
            shellHtml = shellFile.readAll();
        else
            shellHtml = loadHtmlResource();
        QByteArray shellResp = buildHttpResponse(200, "OK", "text/html; charset=utf-8", shellHtml);
        socket->write(shellResp);
        socket->flush();
        socket->disconnectFromHost();
        return;
    }

    if (path == "/api/shell/cwd" || path.startsWith("/api/shell/cwd?")) {
        QJsonObject cwdResp;
        // [B9] 按会话返回各自的工作目录
        cwdResp["cwd"] = shellCwdFor(extractSessionToken(request));
        QByteArray jsonResp = QJsonDocument(cwdResp).toJson(QJsonDocument::Compact);
        QByteArray resp = buildHttpResponse(200, "OK", "application/json; charset=utf-8", jsonResp);
        socket->write(resp);
        socket->flush();
        socket->disconnectFromHost();
        return;
    }

    if (path == "/api/users" || path.startsWith("/api/users?")) {
        handleApiUsers(socket);
        return;
    }

    // 远程文件直链下载（拖拽到本机 / 直接下载均走此路由，已过上面的会话鉴权）
    if (path == "/api/file" || path.startsWith("/api/file?")) {
        handleApiFileDownload(socket, path, requestStr);
        return;
    }

    QByteArray html;
    if (path == "/admin/users" || path.startsWith("/admin/users?")) {
        QFile file(":/res/html/user-management.html");
        if (file.open(QIODevice::ReadOnly))
            html = file.readAll();
        else
            html = loadHtmlResource();
    } else if (!captureAvailable_ && (path == "/" || path.isEmpty())) {
        // 无屏幕捕获时，默认跳转到 Shell 页面
        QFile sf(":/res/html/shell.html");
        if (sf.open(QIODevice::ReadOnly))
            html = sf.readAll();
        else
            html = loadHtmlResource();
    } else {
        html = loadHtmlResource();
    }

    QByteArray resp = buildHttpResponse(200, "OK", "text/html; charset=utf-8", html,
        "Cache-Control: no-cache");
    socket->write(resp);
    socket->flush();
    socket->disconnectFromHost();
}

void RDPServer::handleApiUsers(QTcpSocket* socket)
{
    QJsonArray usersArr;
    QStringList usernames = authManager_->users();
    foreach (const QString& u, usernames) {
        QJsonObject uo;
        uo["username"] = u;
        // 如实上报"是否设置了密码"。这里曾经写死 true（"不通过 API 暴露空密码状态"），
        // 结果用户管理页对所有账号都显示"有密码"，管理员看不出哪个账号是空密码裸奔。
        // 该接口本身要求管理员会话，管理员本就能改任何人的密码，不存在额外的提权面。
        uo["hasPassword"] = !authManager_->hasEmptyPassword(u);
        usersArr.append(uo);
    }

    QJsonObject result;
    result["success"] = true;
    result["users"] = usersArr;
    result["canDelete"] = usernames.size() > 1;

    QByteArray jsonResp = QJsonDocument(result).toJson(QJsonDocument::Compact);
    QByteArray resp = buildHttpResponse(200, "OK", "application/json; charset=utf-8", jsonResp);
    socket->write(resp);
    socket->flush();
    socket->disconnectFromHost();
}

void RDPServer::handleApiAddUser(QTcpSocket* socket, const QByteArray& body)
{
    QJsonDocument doc = QJsonDocument::fromJson(body);
    QJsonObject result;

    if (doc.isObject()) {
        QJsonObject obj = doc.object();
        QString username = obj["username"].toString().trimmed();
        QString password = obj["password"].toString();

        if (username.isEmpty()) {
            result["success"] = false;
            result["error"] = "用户名不能为空";
        } else if (authManager_->addUser(username, password)) {
            result["success"] = true;
        } else {
            result["success"] = false;
            result["error"] = "用户名已存在或添加失败";
        }
    } else {
        result["success"] = false;
        result["error"] = "无效的请求数据";
    }

    QByteArray jsonResp = QJsonDocument(result).toJson(QJsonDocument::Compact);
    QByteArray resp = buildHttpResponse(200, "OK", "application/json; charset=utf-8", jsonResp);
    socket->write(resp);
    socket->flush();
    socket->disconnectFromHost();
}

void RDPServer::handleApiDeleteUser(QTcpSocket* socket, const QByteArray& body)
{
    QJsonDocument doc = QJsonDocument::fromJson(body);
    QJsonObject result;

    if (doc.isObject()) {
        QJsonObject obj = doc.object();
        QString username = obj["username"].toString();

        if (username.isEmpty()) {
            result["success"] = false;
            result["error"] = "用户名不能为空";
        } else if (authManager_->removeUser(username)) {
            result["success"] = true;
        } else {
            result["success"] = false;
            result["error"] = "删除失败：用户不存在或无法删除最后一个用户";
        }
    } else {
        result["success"] = false;
        result["error"] = "无效的请求数据";
    }

    QByteArray jsonResp = QJsonDocument(result).toJson(QJsonDocument::Compact);
    QByteArray resp = buildHttpResponse(200, "OK", "application/json; charset=utf-8", jsonResp);
    socket->write(resp);
    socket->flush();
    socket->disconnectFromHost();
}

void RDPServer::onShellConnected(QWebSocket* socket)
{
    // shell 通道认证：**始终**要求有效会话 token。
    // 旧实现在 captureAvailable_==false（无 DISPLAY/无头）时跳过校验，任何人连上
    // /api/shell/ws 就能拿到服务进程权限的 shell（常为 root/SYSTEM）。
    {
        QString token = QUrlQuery(socket->requestUrl()).queryItemValue(QStringLiteral("token"));
        if (token.isEmpty()) {
            // 兜底：session cookie 为 HttpOnly，前端 JS 读不到，无法拼 ?token=。
            // 浏览器同源 WS 握手自动携带 Cookie，直接解析 session=。
            const QByteArray cookieHeader =
                socket->request().rawHeader(QByteArrayLiteral("Cookie"));
            if (!cookieHeader.isEmpty()) {
                const QList<QByteArray> pairs = cookieHeader.split(';');
                for (const QByteArray& pair : pairs) {
                    const QByteArray trimmed = pair.trimmed();
                    if (trimmed.startsWith(QByteArrayLiteral("session="))) {
                        token = QString::fromUtf8(trimmed.mid(8));
                        break;
                    }
                }
            }
        }
        if (token.isEmpty() || !authManager_->validateSession(token)) {
            qWarning() << "Shell WS rejected: invalid or missing session token";
            socket->close();
            socket->deleteLater();
            return;
        }
    }
    InteractiveShell* shell = InteractiveShell::create(socket, this);
    if (!shell) {
        // 必须同时 deleteLater：此刻尚未连接 disconnected 清理钩子，
        // 只 close() 会把 QWebSocket 对象永久悬挂（内存泄漏 + 死连接）
        socket->close();
        socket->deleteLater();
        return;
    }

    // 发送配置：Windows 无远程回显，前端需做本地回显
    bool localEcho = !shell->hasRemoteEcho();
    socket->sendTextMessage(QStringLiteral("{\"type\":\"config\",\"localEcho\":%1}")
        .arg(localEcho ? QStringLiteral("true") : QStringLiteral("false")));

    connect(socket, &QWebSocket::textMessageReceived, this, [shell](const QString& msg) {
        QJsonDocument doc = QJsonDocument::fromJson(msg.toUtf8());
        if (doc.isObject()) {
            QJsonObject obj = doc.object();
            QString type = obj["type"].toString();
            if (type == "resize")
                shell->resize(obj["cols"].toInt(80), obj["rows"].toInt(24));
            else if (type == "input") {
                QByteArray raw = QByteArray::fromBase64(obj["data"].toString().toUtf8());
                shell->write(QString::fromUtf8(raw).toLocal8Bit());
            }
        }
    });

    connect(socket, &QWebSocket::disconnected, this, [shell, socket]() {
        shell->stop();
        InteractiveShell::sessions().remove(socket);
        shell->deleteLater();
        socket->deleteLater();
    });

    qInfo() << "Interactive shell session started";
}

// [B9] 按会话令牌隔离的 shell 工作目录
QString RDPServer::shellCwdFor(const QString& sessionToken)
{
    auto it = shellCwdByToken_.constFind(sessionToken);
    if (it != shellCwdByToken_.constEnd())
        return it.value();
    // 首次访问：以服务进程启动目录为默认 CWD（与旧 shellCurrentDir_ 语义一致）
    shellCwdByToken_.insert(sessionToken, shellCurrentDir_);
    return shellCurrentDir_;
}

void RDPServer::setShellCwd(const QString& sessionToken, const QString& path)
{
    // 防御无效/过期会话导致 map 无限增长：超上限整体重置（各会话回默认目录）
    if (shellCwdByToken_.size() > 256)
        shellCwdByToken_.clear();
    shellCwdByToken_.insert(sessionToken, path);
}

void RDPServer::handleShellExec(QTcpSocket* socket, const QByteArray& body, const QString& sessionToken)
{
    QJsonDocument doc = QJsonDocument::fromJson(body);
    QJsonObject result;

    if (!doc.isObject()) {
        result["success"] = false;
        result["error"] = "无效的请求数据";
        QByteArray jsonResp = QJsonDocument(result).toJson(QJsonDocument::Compact);
        QByteArray resp = buildHttpResponse(400, "Bad Request", "application/json; charset=utf-8", jsonResp);
        socket->write(resp);
        socket->flush();
        socket->disconnectFromHost();
        return;
    }

    QJsonObject obj = doc.object();
    QString command = obj["command"].toString().trimmed();

    if (command.isEmpty()) {
        result["success"] = false;
        result["error"] = "命令不能为空";
        QByteArray jsonResp = QJsonDocument(result).toJson(QJsonDocument::Compact);
        QByteArray resp = buildHttpResponse(400, "Bad Request", "application/json; charset=utf-8", jsonResp);
        socket->write(resp);
        socket->flush();
        socket->disconnectFromHost();
        return;
    }

    // [B9] 每会话独立 CWD：不再用进程级 QDir::setCurrent（会污染全进程工作目录，
    // 且多客户端/多标签页的 cd 互相干扰）。后续 QProcess 一律 setWorkingDirectory。
    const QString cwd = shellCwdFor(sessionToken);

    // 处理 cd 命令：服务器端切换目录，确保 cd 跨命令持久
    // 支持: cd, cd ~, cd path, cd "path", cd .., cd.., cd.
    // 不认识的 cd 变体（如 c:\ 等）会通过 QProcess 由原生 shell 处理
    if (command == "cd" || command == "cd.") {
        // cd / cd. -> stay in current dir, just return cwd
        result["success"] = true;
        result["exitCode"] = 0;
        result["cwd"] = cwd;
        QByteArray jsonResp = QJsonDocument(result).toJson(QJsonDocument::Compact);
        QByteArray resp = buildHttpResponse(200, "OK", "application/json; charset=utf-8", jsonResp);
        socket->write(resp);
        socket->flush();
        socket->disconnectFromHost();
        return;
    }

    if (command == "cd~") {
        setShellCwd(sessionToken, QDir::homePath());
        result["success"] = true;
        result["exitCode"] = 0;
        result["cwd"] = shellCwdFor(sessionToken);
        QByteArray jsonResp = QJsonDocument(result).toJson(QJsonDocument::Compact);
        QByteArray resp = buildHttpResponse(200, "OK", "application/json; charset=utf-8", jsonResp);
        socket->write(resp);
        socket->flush();
        socket->disconnectFromHost();
        return;
    }

    if (command == "cd.." || command.startsWith("cd.. ")) {
        // cd.. -> cd ..（基于本会话 CWD 解析，不动进程 CWD）
        setShellCwd(sessionToken, QDir::cleanPath(QDir(cwd).absoluteFilePath("..")));
        result["success"] = true;
        result["exitCode"] = 0;
        result["cwd"] = shellCwdFor(sessionToken);
        QByteArray jsonResp = QJsonDocument(result).toJson(QJsonDocument::Compact);
        QByteArray resp = buildHttpResponse(200, "OK", "application/json; charset=utf-8", jsonResp);
        socket->write(resp);
        socket->flush();
        socket->disconnectFromHost();
        return;
    }

    if (command.startsWith("cd ") || command.startsWith("cd\t") || command.startsWith("cd\"") || command.startsWith("cd'")) {
        int split = command.indexOf(' ');
        if (split < 0) split = command.indexOf('\t');
        if (split < 0) split = command.indexOf('"');
        if (split < 0) split = command.indexOf('\'');
        if (split < 0) split = 2; // fallback: "cd" length
        QString arg = command.mid(split + 1).trimmed();

        // Strip surrounding quotes
        if ((arg.startsWith('"') && arg.endsWith('"')) || (arg.startsWith('\'') && arg.endsWith('\'')))
            arg = arg.mid(1, arg.length() - 2);

#ifdef _WIN32
        // Windows cd /d D:\path
        if (arg.startsWith("/d ", Qt::CaseInsensitive))
            arg = arg.mid(3).trimmed();
#endif
        // ~ expansion
        if (arg == "~")
            arg = QDir::homePath();

        // 相对路径基于本会话 CWD 解析；绝对路径 absoluteFilePath 会原样返回
        QString target = QDir::cleanPath(QDir(cwd).absoluteFilePath(arg));
        bool ok = QFileInfo(target).isDir();
#ifdef _WIN32
        // Windows drive letter switch: "D:" -> switch to D:\ current dir
        if (!ok && arg.length() == 2 && arg[1] == ':') {
            target = arg + "\\";
            ok = QFileInfo(target).isDir();
        }
#endif
        if (ok) {
            setShellCwd(sessionToken, target);
            result["success"] = true;
            result["stdout"] = "";
            result["stderr"] = "";
            result["exitCode"] = 0;
        } else {
            result["success"] = false;
            result["error"] = "无法切换到目录: " + arg;
        }
        result["cwd"] = shellCwdFor(sessionToken);
        QByteArray jsonResp = QJsonDocument(result).toJson(QJsonDocument::Compact);
        QByteArray resp = buildHttpResponse(200, "OK", "application/json; charset=utf-8", jsonResp);
        socket->write(resp);
        socket->flush();
        socket->disconnectFromHost();
        return;
    }

    // 异步执行：避免在主线程用 waitForStarted/waitForFinished 同步阻塞（最长 5 分钟），
    // 否则会冻结整个服务（屏幕捕获、WebSocket、所有客户端全部卡死）。
    //
    // 并发上限：每个请求会常驻一个 QProcess + 一个 HTTP 连接（最长 5 分钟）。
    // 无上限时脚本/误操作可同时拉起任意多进程，吃光句柄与内存。
    static const int kMaxShellExec = 8;
    if (shellExecActive_ >= kMaxShellExec) {
        QJsonObject err;
        err["success"] = false;
        err["error"] = "并发命令数已达上限，请稍后重试";
        QByteArray jsonResp = QJsonDocument(err).toJson(QJsonDocument::Compact);
        QByteArray resp = buildHttpResponse(429, "Too Many Requests", "application/json; charset=utf-8", jsonResp);
        socket->write(resp);
        socket->flush();
        socket->disconnectFromHost();
        return;
    }
    ++shellExecActive_;

    QProcess* proc = new QProcess(this);
    proc->setWorkingDirectory(shellCwdFor(sessionToken));

    // 增量抽取输出：旧实现直到 finished 才 readAllStandardOutput/Error，进程
    // 持续输出（`cat /dev/zero`、误执行 `type big.log`、循环脚本）会把
    // QProcess 内部缓冲撑到 OOM。这里边到边取并各自封顶，超出部分丢弃并标记。
    static const int kMaxShellOutputBytes = 1024 * 1024;
    struct ShellOutput {
        QByteArray out;
        QByteArray err;
        bool truncated = false;
    };
    auto output = std::make_shared<ShellOutput>();
    auto drain = [output](QProcess* p) {
        const QByteArray o = p->readAllStandardOutput();
        if (!o.isEmpty()) {
            const int room = kMaxShellOutputBytes - output->out.size();
            if (room > 0)
                output->out.append(o.constData(), qMin<int>(room, o.size()));
            if (o.size() > room)
                output->truncated = true;
        }
        const QByteArray e = p->readAllStandardError();
        if (!e.isEmpty()) {
            const int room = kMaxShellOutputBytes - output->err.size();
            if (room > 0)
                output->err.append(e.constData(), qMin<int>(room, e.size()));
            if (e.size() > room)
                output->truncated = true;
        }
    };
    connect(proc, &QProcess::readyReadStandardOutput, this, [proc, drain]() { drain(proc); });
    connect(proc, &QProcess::readyReadStandardError, this, [proc, drain]() { drain(proc); });

    // 跟踪 socket 生命周期，防止重复响应/悬空写入
    QPointer<QTcpSocket> sockGuard(socket);
    std::shared_ptr<bool> responded = std::make_shared<bool>(false);
    std::shared_ptr<bool> cleaned = std::make_shared<bool>(false);

    // 仅响应一次：将结果写回 HTTP 连接并断开（socket 已关闭则跳过写，仅清理）
    auto respond = [this, sockGuard, responded](const QJsonObject& r) {
        if (*responded)
            return;
        *responded = true;
        if (sockGuard) {
            QByteArray jsonResp = QJsonDocument(r).toJson(QJsonDocument::Compact);
            QByteArray resp = buildHttpResponse(200, "OK", "application/json; charset=utf-8", jsonResp);
            sockGuard->write(resp);
            sockGuard->flush();
            sockGuard->disconnectFromHost();
        }
    };
    // 仅清理一次：删除 QProcess（定时器是其子对象，一并销毁），并归还并发配额
    auto cleanup = [this, proc, cleaned]() {
        if (*cleaned)
            return;
        *cleaned = true;
        --shellExecActive_;
        proc->deleteLater();
    };

    connect(proc, QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished),
            this, [this, proc, respond, cleanup, drain, output, sessionToken](int exitCode, QProcess::ExitStatus status) {
        Q_UNUSED(status);
        drain(proc); // 收尾：finished 前最后一批未读输出
        QJsonObject result;
        result["success"] = true;
        result["stdout"] = QString::fromLocal8Bit(output->out);
        result["stderr"] = QString::fromLocal8Bit(output->err);
        result["exitCode"] = exitCode;
        if (output->truncated)
            result["outputTruncated"] = true;
        result["cwd"] = shellCwdFor(sessionToken);
        respond(result);
        cleanup();
    });
    connect(proc, &QProcess::errorOccurred, this, [this, proc, respond, cleanup, sessionToken](QProcess::ProcessError) {
        QJsonObject result;
        result["success"] = false;
        result["error"] = "启动命令失败: " + proc->errorString();
        result["cwd"] = shellCwdFor(sessionToken);
        respond(result);
        cleanup();
    });

    // 超时 5 分钟：交互式命令（su/ssh）长时间无输出不应卡死服务，超时则杀掉并返回
    QTimer* killTimer = new QTimer(proc);
    killTimer->setSingleShot(true);
    connect(killTimer, &QTimer::timeout, proc, [this, proc, respond, cleanup, drain, output, sessionToken]() {
        drain(proc);
        QJsonObject result;
        result["success"] = false;
        result["error"] = "命令执行超时（5分钟）—— 注意：交互式命令（如 su/ssh）需要输入，当前 Web Shell 不支持";
        result["stdout"] = QString::fromLocal8Bit(output->out);
        result["stderr"] = QString::fromLocal8Bit(output->err);
        if (output->truncated)
            result["outputTruncated"] = true;
        result["cwd"] = shellCwdFor(sessionToken);
        respond(result);
        proc->kill();
        cleanup();
    });
    killTimer->start(300000);

    // 客户端断开时终止进程，避免孤儿进程（finished/errorOccurred 仍会负责 respond+cleanup）
    connect(socket, &QTcpSocket::disconnected, proc, [proc]() {
        proc->kill();
    });

#ifdef _WIN32
    proc->start("cmd.exe", QStringList() << "/c" << command);
#else
    proc->start("/bin/sh", QStringList() << "-c" << command);
#endif
}

bool RDPServer::isCaptureSourceConnected() const
{
    return wsServer_ && wsServer_->isCaptureSourceConnected();
}

void RDPServer::start()
{
    // 订阅 logind 休眠信号（服务模式/前台模式都生效；仅 Linux）
    connectSleepSignals();

    if (serviceMode_) {
        isRunning_ = true;
        // 服务模式初始为图片模式；Linux 本地捕获可用时前端会发起 WebRTC，
        // startWebRtcSession 内部再切到视频模式
        currentMode_ = ServerMode::Image;
#ifdef Q_OS_WIN32
        // [Win-登录] 开机停在登录界面时没有 helper，也没人上报 screen_locked。
        // 周期探测"控制台会话无用户 + LogonUI 在跑"，据此拉起 secure-input
        // 并通知前端显示「远程登录」入口。
        startLoginScreenWatcher();
#endif
        qInfo() << "RDP Server started in service mode (waiting for helper connection)";
        return;
    }

    // 先启动屏幕捕获：编码器必须按**采集到的真实像素尺寸**初始化。
    // start() 之前 screenCapturer_->width() 在 Linux/Windows 高 DPI 下取的是
    // QScreen 逻辑尺寸，与 X11/DXGI 的实际像素不一致；按逻辑尺寸建好的编码器
    // 每帧都会触发 sizeMismatch 重建（周期性卡顿/黑帧）。
    if (!screenCapturer_->start(configFps_)) {
        qCritical() << "Failed to start screen capture";
        captureAvailable_ = false;
        // 采集起不来就没有任何原始帧：若保持默认的 Video 模式，客户端连上后
        // 第一条 mode_changed 会声称"视频模式"，前端随即请求关键帧，服务端在
        // 无捕获源的情况下重建编码器必然失败再切回图片（日志刷 "no IDR"/
        // "Encoder not found"，画面还会黑一下）。启动失败就直接落图片模式，
        // 与 startCapture() 的失败路径保持一致；后续 restartCapture() 成功时
        // 会按采集尺寸重建编码器并恢复视频。
        currentMode_ = ServerMode::Image;
        return;
    }

    // 检测捕获是否可用（无 X 时 width/height 为 0）
    captureAvailable_ = screenCapturer_->width() > 0 && screenCapturer_->height() > 0;
    if (!captureAvailable_)
        qWarning() << "Screen capture unavailable (no display), serving shell-only mode";

    // 尝试初始化视频编码器（按缩放档位编码，缩放由编码线程 sws 完成）
#ifdef USE_FFMPEG
    {
        int sw = screenCapturer_->width(), sh = screenCapturer_->height();
        int ew = sw, eh = sh;
        if (configScale_ < 100) {
            int tw = ew * configScale_ / 100, th = eh * configScale_ / 100;
            if (tw > 0 && th > 0) { ew = tw; eh = th; }
        }
        if (videoEncoder_->initialize(configCodec_, sw, sh, ew, eh,
                configFps_, videoBitrateFor(ew, eh, configFps_, configCodec_), configHwEncodeMode_)) {
            currentMode_ = ServerMode::Video;
            videoBaseBitrate_ = videoBitrateFor(ew, eh, configFps_, configCodec_);
            qInfo() << "Video encoder initialized, using video mode.";
        } else
#endif
        {
            qWarning() << "Video encoder initialization failed, falling back to image mode.";
            switchToImageMode();
        }
#ifdef USE_FFMPEG
    }
#endif

    // 如果没有客户端连接，暂停捕获以降低 CPU
    if (wsServer_->clientCount() == 0)
        screenCapturer_->suspend();

    isRunning_ = true;
    qInfo() << "RDP Server started, mode:" << (currentMode_ == ServerMode::Video ? "video" : "image");
}

void RDPServer::broadcastScreenLocked(const QJsonObject& msg)
{
    // 去重：screen_locked 的权威来源已收敛为 ScreenCapturer::pollRealLockState()
    // （logind LockedHint + 桌面/前台 VT + greeter VT 交叉证据）与命令侧
    // setLockedExternally()，但多条路径仍可能对同一状态重复触发（例如命令确认
    // 与下一次采集周期、采集重启后的重新判定）。状态量不变就不重复下发。
    // 新客户端接入时由 onClientConnected 单独补发快照，不依赖这里。
    if (!wsServer_)
        return;
    const int v = msg.value(QStringLiteral("locked")).toBool() ? 1 : 0;
    if (lastBroadcastLocked_ == v)
        return;
    lastBroadcastLocked_ = v;
    wsServer_->broadcastJson(msg);
}

bool RDPServer::startCapture()
{
    // 采集重启（休眠恢复 / display 切换 / 采集失败重试）会重建 ScreenCapturer，
    // 旧编码器绑定的源尺寸随之失效。重建前若处于视频模式，用户意图应保留：
    // 先切图片释放旧编码器/WebRTC，采集成功后按新模式恢复视频，而不是把
    // 正在观看的客户端降级回图片模式。
    const ServerMode modeBeforeRestart = currentMode_;
    // 重入保护：Wayland 初始化在 ScreenCapturer::start() 里跑最长 5s 的嵌套
    // 事件循环（QCoreApplication::processEvents），Linux 服务的 3s display 巡检
    // 定时器会在其中被派发 → restartCapture() 会 reset() 掉"start() 还在栈上"的
    // 采集器（use-after-free）；startCapture() 重入则会先 cleanupPlatform()
    // 销毁正在使用的 waylandCapturer_/x11Capturer_。启动期间一律拒绝重入，
    // 巡检侧（isCaptureStarting()）与 set_resolution 也据此跳过。
    if (captureStarting_) {
        qWarning() << "startCapture: capture start already in progress, ignoring reentrant call";
        return false;
    }
    QScopedValueRollback<bool> startingGuard(captureStarting_, true);
#if defined(Q_OS_LINUX) && !defined(Q_OS_ANDROID)
    // Linux: DISPLAY may not have been set at init, recreate input manager
    inputManager_ = std::unique_ptr<InputManager>(new InputManager(this));
#endif

    if (!screenCapturer_) {
        screenCapturer_ = std::unique_ptr<ScreenCapturer>(new ScreenCapturer(this));
        connect(screenCapturer_.get(), &ScreenCapturer::frameCaptured,
            this, &RDPServer::onFrameCaptured);
#if defined(Q_OS_ANDROID)
        // 旋转/分辨率变化：帧尺寸变了，输入换算基准与前端 canvas 都要同步，
        // 否则点击会按旧尺寸映射到错误位置。
        connect(screenCapturer_.get(), &ScreenCapturer::androidFrameSizeChanged,
            this, [this](int w, int h) {
                if (inputManager_)
                    inputManager_->setScreenSize(w, h);
                if (wsServer_) {
                    QJsonObject info;
                    info["type"] = "screen_info";
                    info["width"] = w;
                    info["height"] = h;
                    info["platform"] = platformTag();
                    wsServer_->broadcastJson(info);
                }
            });
#endif
        connect(screenCapturer_.get(), &ScreenCapturer::screenLocked,
            this, [this](bool locked) {
                screenLocked_ = locked;
                if (!locked) {
                    // [C] 与前台路径一致：解锁即作废本解锁序列的延时回调/待发 blocked。
                    ++unlockSeq_;
                    secureUnlockTyped_ = false;
                    secureUnlockDefinite_ = false;
                    secureUnlockBlockedReported_ = false;
                    secureInputUnlockClientId_.clear();
                }
                // 输入坐标归一化基准与前端 canvas（screen_info）保持一致
                if (inputManager_) {
                    inputManager_->setScreenSize(screenCapturer_->width(), screenCapturer_->height());
                    inputManager_->setScreenOffset(screenCapturer_->originX(), screenCapturer_->originY());
#if defined(Q_OS_LINUX) && !defined(Q_OS_ANDROID)
                    // 锁屏时才允许 focusLockScreenWindow 抢焦点投递密码输入，
                    // 阻止普通桌面下强设焦点到全屏/置顶窗口而丢焦点（键盘失焦 bug）。
                    inputManager_->setScreenLocked(locked);
#endif
                }
                broadcastScreenLocked(QJsonObject {
                    { "type", "screen_locked" },
                    { "locked", locked },
                    { "platform", platformTag() },
#ifdef _WIN32
                    { "isXP", !isVistaPlusOs() },
#endif
                    { "hint", locked ? QString::fromUtf8(
#ifdef Q_OS_WIN
                        "Windows 锁屏在安全桌面上，点「解锁」由服务端注入密码"
#elif defined(Q_OS_ANDROID)
                        "Android 锁屏输入暂不支持"
#elif defined(Q_OS_LINUX)
                        "如需在锁屏界面输入密码，请先执行："
                        "sudo usermod -aG input $USER && "
                        "sudo modprobe uinput && sudo chmod 666 /dev/uinput"
#elif defined(Q_OS_MACOS)
                        "macOS 锁屏状态输入需要辅助功能权限："
                        "系统偏好设置 → 隐私与安全性 → 辅助功能 → 添加此应用"
#else
                        "锁屏状态下输入可能受限"
#endif
                                           )
                                     : QString() } });
                if (locked) {
                    qInfo() << "Screen locked";
#if defined(Q_OS_LINUX) && !defined(Q_OS_ANDROID)
                    if (inputManager_ && !inputManager_->initUinput())
                        qWarning() << "uinput unavailable, XTest fallback";
#endif
                } else {
                    qInfo() << "Screen unlocked, restoring normal input";
#if defined(Q_OS_LINUX) && !defined(Q_OS_ANDROID)
                    if (inputManager_)
                        inputManager_->destroyUinput();
#endif
                }
            });

        jpegCompressor_ = std::unique_ptr<JpegCompressor>(new JpegCompressor(nullptr));
        jpegCompressor_->setQuality(configQuality_);
        jpegCompressor_->setScalePercent(configScale_);
        connect(jpegCompressor_.get(), &JpegCompressor::jpegCompressed,
            this, &RDPServer::onJpegCompressed, Qt::QueuedConnection);
        jpegCompressor_->start();
    }

    switchToImageMode();
    if (!screenCapturer_->start(configFps_)) {
        qCritical() << "startCapture: failed to start screen capture";
        captureAvailable_ = false;
        // 必须告知前端：否则表现为"连上了、一直黑、没有任何解释"。
        // Linux 服务模式每 3 秒会重试（DISPLAY 是 WSL 启动后才就绪的常见情形），
        // 一旦成功这里会再广播 available=true 把提示撤掉。
        if (wsServer_) {
            QJsonObject st;
            st["type"] = "capture_status";
            st["available"] = false;
            st["hint"] = QString::fromUtf8(
#if defined(Q_OS_LINUX) && !defined(Q_OS_ANDROID)
                "未检测到可用的图形显示（X11/Wayland），正在后台自动重试。"
                "WSL 请确认已启用 WSLg（Windows 11 默认）；"
                "无头环境可安装 Xvfb 后重启服务。"
#else
                "屏幕采集当前不可用，正在后台自动重试。"
#endif
                );
            wsServer_->broadcastJson(st);
        }
        return false;
    }
#if defined(Q_OS_LINUX) && !defined(Q_OS_ANDROID)
    // 预热焦点窗口：首次按键不再触发 X 窗口树遍历，降低首键延迟
    inputManager_->primeFocusWindow();
    // Wayland 桌面的输入必须走 uinput，而设备创建含 udev 就绪等待（键盘+指针+
    // 滚轮共约 0.6s）。在启动期建好，避免首次按键/移动时主线程明显卡顿。
    if (inputManager_->isWaylandInput() && !inputManager_->isUinputActive())
        inputManager_->initUinput();
#endif
    captureAvailable_ = screenCapturer_->width() > 0 && screenCapturer_->height() > 0;
    if (!captureAvailable_)
        qWarning() << "startCapture: screen capture unavailable";
    if (wsServer_->clientCount() == 0)
        screenCapturer_->suspend();
    qInfo() << "startCapture: screen capturer started";

    // 重启前是视频模式且采集可用：按新采集源的尺寸重建编码器并恢复视频，
    // 否则休眠恢复/display 切换会把在线客户端永久踢回图片模式。
    if (modeBeforeRestart == ServerMode::Video && captureAvailable_) {
        if (switchToVideoMode())
            qInfo() << "startCapture: restored video mode after capture restart";
        else
            qWarning() << "startCapture: failed to restore video mode after capture restart";
    }

    // 捕获变为可用（如 Linux 服务进程启动后 DISPLAY 才出现），通知已连接客户端
    // 重新评估模式：捕获可用 + 编码器 → webrtc=true，客户端随即升级到 WebRTC
    if (captureAvailable_) {
        // 撤掉之前可能显示过的"采集不可用"提示
        if (wsServer_) {
            QJsonObject st;
            st["type"] = "capture_status";
            st["available"] = true;
            wsServer_->broadcastJson(st);
        }
        QJsonObject mode;
        mode["type"] = "mode_changed";
        mode["mode"] = (currentMode_ == ServerMode::Video) ? "video" : "image";
        mode["hwEncode"] = hwEncodeAvailable();
        mode["videoSupported"] = videoModeSupported();
        mode["codec"] = codecToString(configCodec_);
#if defined(USE_WEBRTC) && defined(USE_FFMPEG)
        mode["webrtc"] = true;
#else
        mode["webrtc"] = false;
#endif
        wsServer_->broadcastJson(mode);
    }
    return true;
}

bool RDPServer::isCaptureConnected() const
{
    return screenCapturer_ != nullptr && captureAvailable_;
}

bool RDPServer::restartCapture()
{
    // 与 startCapture 共用重入守卫：采集启动期间（含其内部的嵌套事件循环）
    // reset() 当前采集器会把正在 start() 的对象释放掉。
    if (captureStarting_) {
        qWarning() << "restartCapture: capture start in progress, skipping restart";
        return false;
    }
    // Tear down the existing capturer (drops the X11 connection / Display*),
    // then start fresh so the new ScreenCapturer picks up the current DISPLAY /
    // XAUTHORITY environment. Used when the active X session changes at runtime
    // (e.g. xrdp session appears after the service started on the GDM greeter).
    const bool wasLocked = screenLocked_;
    if (screenCapturer_) {
        screenCapturer_->stop();
        screenCapturer_.reset();  // ~ScreenCapturer() -> cleanupPlatform()
    }
    captureAvailable_ = false;
    screenLocked_ = false;
#if defined(Q_OS_LINUX) && !defined(Q_OS_ANDROID)
    // Linux 锁屏会把前台切到 greeter VT，detectUserX11Env 也可能把采集源从
    // 用户桌面 :1 切到 lightdm 的 :0（"display changed" 触发本函数）。所以
    // "重启采集"绝不是解锁证据：乐观广播 locked:false 会让前端遮罩闪断。
    // 重启后立刻用 logind + VT 交叉判定重新落地真实状态。
    if (!startCapture())
        return false;
    if (screenCapturer_)
        screenCapturer_->refreshLockState();
    if (wasLocked && !screenLocked_) {
        // 只有在重新判定确认已不在锁屏时才通知前端收起遮罩。
        ++unlockSeq_;
        secureUnlockTyped_ = false;
        secureUnlockDefinite_ = false;
        secureUnlockBlockedReported_ = false;
        secureInputUnlockClientId_.clear();
        broadcastScreenLocked(QJsonObject {
            { "type", "screen_locked" }, { "locked", false } });
    }
    return true;
#else
    // 重启采集会丢掉旧的锁屏状态，但已连接前端的遮罩不会自动收起（新捕获器
    // 只在状态"变化"时才发 screen_locked）→ 必须主动广播一次解锁。
    if (wasLocked) {
        // [C] 状态已离开锁屏：作废当前解锁序列的全部延时回调与 blocked 转发，
        // 否则"重启采集"这种非解锁路径也可能在 8s/15s 后弹出解锁失败。
        ++unlockSeq_;
        secureUnlockTyped_ = false;
        secureUnlockDefinite_ = false;
        secureUnlockBlockedReported_ = false;
        secureInputUnlockClientId_.clear();
        broadcastScreenLocked(QJsonObject {
            { "type", "screen_locked" }, { "locked", false } });
    }
    return startCapture();
#endif
}

void RDPServer::updateSleepInhibit(bool active)
{
    if (active == sleepInhibitActive_)
        return;
    sleepInhibitActive_ = active;

#if defined(Q_OS_LINUX) && !defined(Q_OS_ANDROID)
    // [P1 perf] 原 DBus 后端链为三个 BlockWithGui(3000) 同步调用，最坏阻塞主线程
    // 9s（期间采集/WS/输入处理全部停摆）→ 改为 QDBusPendingCallWatcher 异步链：
    // 非阻塞逐级回退；每个回调里复查 sleepInhibitActive_（等待期间会话可能已
    // 断开，此时中止后续后端，避免"已断开仍持有 inhibit"泄漏）。
    if (active) {
        if (sleepInhibitBackend_ != InhibitBackend::None)
            return;
        inhibitAcquireSessionManager();
    } else if (sleepInhibitBackend_ != InhibitBackend::None) {
        inhibitRelease();
    }
#elif defined(_WIN32)
    // ES_SYSTEM_REQUIRED 阻止睡眠、ES_DISPLAY_REQUIRED 阻止熄屏
    if (active)
        SetThreadExecutionState(ES_CONTINUOUS | ES_SYSTEM_REQUIRED | ES_DISPLAY_REQUIRED);
    else
        SetThreadExecutionState(ES_CONTINUOUS);
#elif defined(Q_OS_MACOS)
    // macOS 用系统自带 caffeinate（-d 防熄屏 -s 防睡眠 -i 防空闲），断开时终止
    if (active && !sleepInhibitProcess_) {
        sleepInhibitProcess_ = new QProcess(this);
        sleepInhibitProcess_->start(QStringLiteral("caffeinate"), {
            QStringLiteral("-d"), QStringLiteral("-s"), QStringLiteral("-i") });
        qInfo() << "caffeinate started (sleep inhibit)";
    } else if (!active && sleepInhibitProcess_) {
        sleepInhibitProcess_->terminate();
        sleepInhibitProcess_->waitForFinished(1000);
        sleepInhibitProcess_->deleteLater();
        sleepInhibitProcess_ = nullptr;
        qInfo() << "caffeinate stopped (sleep inhibit released)";
    }
#endif
}

#if defined(Q_OS_LINUX) && !defined(Q_OS_ANDROID)
// ---- 防睡眠 DBus 后端异步链（[P1 perf]，原同步 BlockWithGui 链最坏阻塞主线程 9s）----
void RDPServer::inhibitAcquireSessionManager()
{
    // GNOME SessionManager.Inhibit（Electron powerSaveBlocker 同款）：
    // flags: 4=InhibitSuspend, 8=InhibitIdle（阻止挂起与熄屏）。
    QDBusMessage msg = QDBusMessage::createMethodCall(
        QStringLiteral("org.gnome.SessionManager"),
        QStringLiteral("/org/gnome/SessionManager"),
        QStringLiteral("org.gnome.SessionManager"),
        QStringLiteral("Inhibit"));
    msg << QStringLiteral("QtRemoteDesktop")
        << quint32(0)
        << QStringLiteral("Remote desktop session active")
        << quint32(12);
    auto* watcher = new QDBusPendingCallWatcher(
        QDBusConnection::sessionBus().asyncCall(msg), this);
    connect(watcher, &QDBusPendingCallWatcher::finished, this,
            [this](QDBusPendingCallWatcher* w) {
        w->deleteLater();
        if (!sleepInhibitActive_ || sleepInhibitBackend_ != InhibitBackend::None)
            return; // 会话已结束或已由其他后端接管
        QDBusPendingReply<quint32> reply = *w;
        if (!reply.isError()) {
            sleepInhibitCookie_ = reply.value();
            sleepInhibitBackend_ = InhibitBackend::SessionManager;
            qInfo() << "Sleep inhibit acquired (gnome SessionManager cookie" << sleepInhibitCookie_ << ")";
            return;
        }
        qWarning() << "SessionManager Inhibit failed:"
                   << reply.error().name() << reply.error().message();
        inhibitAcquireScreenSaver(); // 回退 2) 通用 ScreenSaver 接口
    });
}

void RDPServer::inhibitAcquireScreenSaver()
{
    QDBusMessage msg = QDBusMessage::createMethodCall(
        QStringLiteral("org.freedesktop.ScreenSaver"),
        QStringLiteral("/org/freedesktop/ScreenSaver"),
        QStringLiteral("org.freedesktop.ScreenSaver"),
        QStringLiteral("Inhibit"));
    msg << QStringLiteral("QtRemoteDesktop")
        << QStringLiteral("Remote desktop session active");
    auto* watcher = new QDBusPendingCallWatcher(
        QDBusConnection::sessionBus().asyncCall(msg), this);
    connect(watcher, &QDBusPendingCallWatcher::finished, this,
            [this](QDBusPendingCallWatcher* w) {
        w->deleteLater();
        if (!sleepInhibitActive_ || sleepInhibitBackend_ != InhibitBackend::None)
            return;
        QDBusPendingReply<quint32> reply = *w;
        if (!reply.isError()) {
            sleepInhibitCookie_ = reply.value();
            sleepInhibitBackend_ = InhibitBackend::ScreenSaver;
            qInfo() << "Sleep inhibit acquired (ScreenSaver cookie" << sleepInhibitCookie_ << ")";
            return;
        }
        qWarning() << "ScreenSaver Inhibit failed:"
                   << reply.error().name() << reply.error().message();
        inhibitAcquireLogin1(); // 回退 3) login1（多数无活跃会话场景被 polkit 拒绝）
    });
}

void RDPServer::inhibitAcquireLogin1()
{
    QDBusMessage msg = QDBusMessage::createMethodCall(
        QStringLiteral("org.freedesktop.login1"),
        QStringLiteral("/org/freedesktop/login1"),
        QStringLiteral("org.freedesktop.login1.Manager"),
        QStringLiteral("Inhibit"));
    msg << QStringLiteral("sleep:idle")
        << QStringLiteral("QtRemoteDesktop")
        << QStringLiteral("Remote desktop session active")
        << QStringLiteral("block");
    auto* watcher = new QDBusPendingCallWatcher(
        QDBusConnection::systemBus().asyncCall(msg), this);
    connect(watcher, &QDBusPendingCallWatcher::finished, this,
            [this](QDBusPendingCallWatcher* w) {
        w->deleteLater();
        if (!sleepInhibitActive_ || sleepInhibitBackend_ != InhibitBackend::None)
            return;
        QDBusPendingReply<QDBusUnixFileDescriptor> reply = *w;
        if (!reply.isError()) {
            const QDBusUnixFileDescriptor fd = reply.value();
            if (fd.isValid()) {
                int dupFd = ::dup(fd.fileDescriptor());
                if (dupFd >= 0) {
                    sleepInhibitFd_ = dupFd;
                    sleepInhibitBackend_ = InhibitBackend::Login1;
                    qInfo() << "Sleep inhibit acquired (login1 fd" << dupFd << ")";
                    return;
                }
            }
        }
        qWarning() << "All sleep-inhibit backends failed (login1 last)";
    });
}

void RDPServer::inhibitRelease()
{
    if (sleepInhibitBackend_ == InhibitBackend::SessionManager) {
        QDBusMessage msg = QDBusMessage::createMethodCall(
            QStringLiteral("org.gnome.SessionManager"),
            QStringLiteral("/org/gnome/SessionManager"),
            QStringLiteral("org.gnome.SessionManager"),
            QStringLiteral("Uninhibit"));
        msg << sleepInhibitCookie_;
        QDBusConnection::sessionBus().asyncCall(msg);
        qInfo() << "Sleep inhibit released (SessionManager cookie" << sleepInhibitCookie_ << ")";
    } else if (sleepInhibitBackend_ == InhibitBackend::ScreenSaver) {
        QDBusMessage msg = QDBusMessage::createMethodCall(
            QStringLiteral("org.freedesktop.ScreenSaver"),
            QStringLiteral("/org/freedesktop/ScreenSaver"),
            QStringLiteral("org.freedesktop.ScreenSaver"),
            QStringLiteral("UnInhibit"));
        msg << sleepInhibitCookie_;
        QDBusConnection::sessionBus().asyncCall(msg);
        qInfo() << "Sleep inhibit released (ScreenSaver cookie" << sleepInhibitCookie_ << ")";
    } else if (sleepInhibitBackend_ == InhibitBackend::Login1 && sleepInhibitFd_ >= 0) {
        ::close(sleepInhibitFd_);
        sleepInhibitFd_ = -1;
        qInfo() << "Sleep inhibit released (login1 fd)";
    }
    sleepInhibitCookie_ = 0;
    sleepInhibitBackend_ = InhibitBackend::None;
}
#endif

void RDPServer::connectSleepSignals()
{
#if defined(Q_OS_LINUX) && !defined(Q_OS_ANDROID)
    if (sleepSignalsConnected_)
        return;
    // 订阅 logind 的 PrepareForSleep(bool)：挂起/恢复时重建捕获流
    sleepSignalsConnected_ = QDBusConnection::systemBus().connect(
        QStringLiteral("org.freedesktop.login1"),
        QStringLiteral("/org/freedesktop/login1"),
        QStringLiteral("org.freedesktop.login1.Manager"),
        QStringLiteral("PrepareForSleep"),
        this, SLOT(onPrepareForSleep(bool)));
    if (sleepSignalsConnected_)
        qInfo() << "Connected login1 PrepareForSleep signal";
    else
        qWarning() << "Failed to connect login1 PrepareForSleep signal (no logind?)";
#else
    Q_UNUSED(sleepSignalsConnected_);
#endif
}

void RDPServer::onPrepareForSleep(bool sleeping)
{
#if defined(Q_OS_LINUX) && !defined(Q_OS_ANDROID)
    qInfo() << "PrepareForSleep:" << (sleeping ? "going to sleep" : "resumed");
    if (sleeping) {
        preparedForSleep_ = true;
        // 挂起期间 ScreenCast/PipeWire 流会失效，先暂停捕获
        if (screenCapturer_)
            screenCapturer_->suspend();
    } else {
        preparedForSleep_ = false;
        // 休眠恢复：采集流已断，若仍有客户端在线则重建捕获
        if (wsServer_->clientCount() != 0) {
            if (screenCapturer_) {
                qInfo() << "Re-establishing screen capture after sleep";
                restartCapture();
            }
            if (serviceMode_ && wsServer_->isCaptureSourceConnected()) {
                QJsonObject msg;
                msg["type"] = "capture_control";
                msg["action"] = "resume";
                wsServer_->sendToCaptureSource(msg);
            }
        }
    }
#endif
}

// 输入法切换：循环 preload-engines 列表。
// 背景：ibus panel 的 hotkey（默认 <Super>space）通过 X grab 实现，对 XTEST 注入的
// 合成按键事件不响应（已在 RK3588/LXDE 实测：注入事件到达 X 但 panel 零回调），
// 所以远程端"注入 Super+space"永远无法切换输入法。这里绕开键盘事件，直接用
// `ibus engine <name>` 切换 —— ibus CLI 通过 ~/.config/ibus/bus/ 下的地址文件连接
// ibus-daemon，不依赖 X grab，也不依赖 DISPLAY。
// 注意：Linux 上 systemd 服务模式以 root 跑（--service），root 下没有桌面用户的
// session bus，ibus/gsettings 必须以 X 会话用户身份执行（sudo -H -u <user> env ...）。
void RDPServer::handleImeCycle(const QString& clientId)
{
#if defined(Q_OS_LINUX) && !defined(Q_OS_ANDROID)
    if (serviceMode_ && wsServer_->isCaptureSourceConnected()) {
        // 服务模式且有 helper 连接：helper 侧（用户会话）的 ibus 切换尚未实现
        wsServer_->sendJson(clientId, QJsonObject {
            { "type", "ime_state" },
            { "error", "unsupported" },
        });
        return;
    }

    // 命令前缀：root 时定位桌面用户（/run/user/ 下最小的非 0 uid），包装 sudo -u 执行
    QStringList prefix;
    if (::getuid() == 0) {
        quint64 target = 0;
        bool found = false;
        const QStringList ids = QDir(QStringLiteral("/run/user"))
                .entryList(QDir::AllEntries | QDir::NoDotAndDotDot);
        for (const QString& id : ids) {
            bool okNum = false;
            const quint64 v = id.toULongLong(&okNum);
            if (okNum && v != 0 && (!found || v < target)) {
                target = v;
                found = true;
            }
        }
        if (!found) {
            wsServer_->sendJson(clientId, QJsonObject {
                { "type", "ime_state" }, { "error", "no_user_session" } });
            return;
        }
        const passwd* pw = ::getpwuid(static_cast<uid_t>(target));
        if (!pw) {
            wsServer_->sendJson(clientId, QJsonObject {
                { "type", "ime_state" }, { "error", "no_user_session" } });
            return;
        }
        if (QStandardPaths::findExecutable(QStringLiteral("sudo")).isEmpty()) {
            wsServer_->sendJson(clientId, QJsonObject {
                { "type", "ime_state" }, { "error", "no_sudo" } });
            return;
        }
        const QString user = QString::fromLatin1(pw->pw_name);
        prefix = QStringList {
            QStringLiteral("sudo"), QStringLiteral("-H"),
            QStringLiteral("-u"), user, QStringLiteral("env"),
            QStringLiteral("DBUS_SESSION_BUS_ADDRESS=unix:path=/run/user/%1/bus").arg(target),
            QStringLiteral("XDG_RUNTIME_DIR=/run/user/%1").arg(target),
        };
    }

    // [P1 perf] 命令链（ibus/gsettings 各 2~3s，最坏 2+2+3+2=9s）原先在主线程
    // waitForFinished 同步等待，期间采集/WS/输入全部停摆 → 整体移到工作线程；
    // 结果经 imeResultReady 信号（auto=queued）回主线程发送（sendJson 操作
    // QWebSocket，必须留在主线程）。
    // [B8] 线程生命周期由 imeWorker_ 成员持有、析构时 join（原 detach 后无法
    // 回收，退出瞬间线程仍在 emit → use-after-free）。同刻只允许一个在飞。
    if (imeWorker_ && imeWorker_->joinable()) {
        wsServer_->sendJson(clientId, QJsonObject {
            { "type", "ime_state" }, { "error", "busy" } });
        return;
    }
    QPointer<RDPServer> self(this);
    const QString cid = clientId;
    imeWorker_.reset(new std::thread([self, cid, prefix]() {
        // 所有返回路径（含提前 return）都要通知主线程回收线程对象：
        // 线程结束后 joinable() 仍为 true，不复位 imeWorker_ 会让之后每次
        // ime_switch 都命中上面的 busy 分支，输入法切换永久失效。
        struct FinishNotifier {
            QPointer<RDPServer> s;
            ~FinishNotifier() { if (s) emit s->imeWorkerFinished(); }
        } finishNotifier { self };
        // 执行一条完整命令（prefix 非空时包装为 sudo -u ... env ...），返回 exit code
        auto runCmd = [&prefix](const QStringList& cmd, int timeoutMs, QString* out) -> int {
            QProcess p;
            if (prefix.isEmpty())
                p.start(cmd.first(), cmd.mid(1));
            else
                p.start(prefix.first(), prefix.mid(1) + cmd);
            p.waitForFinished(timeoutMs);
            if (out)
                *out = QString::fromUtf8(p.readAllStandardOutput());
            return (p.exitStatus() == QProcess::NormalExit) ? p.exitCode() : -1;
        };

        // 1) 当前 engine（无焦点窗口时可能为空）
        QString cur;
        runCmd({ QStringLiteral("ibus"), QStringLiteral("engine") }, 2000, &cur);
        cur = cur.trimmed();

        // 2) preload-engines 列表（gsettings 输出形如 ['libpinyin', 'pinyin']）
        QString raw;
        runCmd({ QStringLiteral("gsettings"), QStringLiteral("get"),
                 QStringLiteral("org.freedesktop.ibus.general"),
                 QStringLiteral("preload-engines") }, 2000, &raw);
        QStringList engines;
        raw.remove(QLatin1Char('[')).remove(QLatin1Char(']'))
           .remove(QLatin1Char('\'')).remove(QLatin1Char('"'));
        const QStringList parts = raw.split(QLatin1Char(','));
        for (const QString& item : parts) {
            const QString e = item.trimmed();
            if (!e.isEmpty())
                engines << e;
        }

        if (engines.size() < 2) {
            qInfo() << "IME switch: no switchable engines (preload =" << engines << ")";
            if (self)
                emit self->imeResultReady(cid, QJsonObject {
                    { "type", "ime_state" },
                    { "engine", cur },
                    { "error", engines.isEmpty() ? "no_engines" : "single_engine" },
                });
            return;
        }

        // 3) 计算下一个 engine（循环切换；当前 engine 不在列表时落到第一个）
        const int idx = engines.indexOf(cur);
        const QString next = engines[(idx + 1) % engines.size()];

        // 4) 应用切换并读回验证（实测 ibus CLI set 后 exit code 可能为非 0 但切换
        //    实际生效，故以读回结果为准，不用 exit code 判定）
        runCmd({ QStringLiteral("ibus"), QStringLiteral("engine"), next }, 3000, nullptr);
        QString verify;
        runCmd({ QStringLiteral("ibus"), QStringLiteral("engine") }, 2000, &verify);
        const QString now = verify.trimmed();
        const bool ok = !now.isEmpty();

        qInfo() << "IME switch:" << cur << "->" << next << "now =" << now;
        if (self)
            emit self->imeResultReady(cid, QJsonObject {
                { "type", "ime_state" },
                { "engine", now.isEmpty() ? next : now },
                { "ok", ok },
            });
    }));
#else
    Q_UNUSED(clientId);
#endif
}

void RDPServer::onImeWorkerFinished()
{
    // 信号在本线程函数返回前发出，这里 join 只会等待最后几个指令周期。
    // join 后再 reset，下一条 ime_switch 才能真正启动新线程。
    if (imeWorker_ && imeWorker_->joinable())
        imeWorker_->join();
    imeWorker_.reset();
}

void RDPServer::onImeResultReady(const QString& clientId, const QJsonObject& state)
{
    wsServer_->sendJson(clientId, state);
}

#if defined(Q_OS_LINUX) && !defined(Q_OS_ANDROID)
// X11 EWMH：向 root 窗口发送 _NET_SHOWING_DESKTOP client message（wmctrl -k on
// 同款协议），不依赖 wmctrl/xdotool 是否安装；GNOME/XFCE/KDE/openbox/LXDE 等支持
// EWMH 的 WM 都会响应。注意：GNOME(mutter) 不响应 XTEST 合成的 Super+D 快捷键
// （实测 xdotool key super+d 无效），因此该协议是 Linux 显示桌面最可靠的方式。
static void x11ShowDesktopOnce(bool show)
{
    static bool threadsInit = false;
    if (!threadsInit) { XInitThreads(); threadsInit = true; }
    Display* dpy = XOpenDisplay(nullptr);
    if (!dpy) return;
    Atom a = XInternAtom(dpy, "_NET_SHOWING_DESKTOP", False);
    XEvent ev;
    memset(&ev, 0, sizeof(ev));
    ev.xclient.type = ClientMessage;
    ev.xclient.window = DefaultRootWindow(dpy);
    ev.xclient.message_type = a;
    ev.xclient.format = 32;
    ev.xclient.data.l[0] = show ? 1 : 0;
    ev.xclient.data.l[1] = 0;
    ev.xclient.data.l[2] = 0;
    ev.xclient.data.l[3] = 0;
    ev.xclient.data.l[4] = 0;
    XSendEvent(dpy, DefaultRootWindow(dpy), False,
               SubstructureRedirectMask | SubstructureNotifyMask, &ev);
    XSync(dpy, False);
    XCloseDisplay(dpy);
}
#endif

#ifdef _WIN32
// [LOCK] helper 回执窗口：3s 内既没有 system_action_result，也没有 screen_locked
// 才算"没回执"，随后用 console 会话令牌补投一次。
static const int kLockAckTimeoutMs = 3000;

// [LOCK] Windows 的锁屏：三态收敛，绝不再"没锁上却报已锁"。
//   ① 非服务模式：本进程就在用户会话里 → 直接 LockWorkStation()，返回值即真相；
//   ② 服务模式 + helper 在：转发给用户会话的 helper，等 system_action_result ack
//      或 screen_locked=true；3s 都没有 → 用 console 会话令牌在用户会话里跑一次性
//      --lock-once 进程，其退出码（LockWorkStation 的 BOOL）就是这一轮的 ack；
//   ③ 服务模式 + helper 不在：**立即**回 ok:false/no_helper，绝不退回 Session 0
//      的 rundll32（那正是"锁定空操作"的根因）。
// XP（isVistaPlusOs()==false）同样走 --lock-once：XP 的服务与用户同处一个会话，
// 一次性进程里的 LockWorkStation() 返回值就是真实 BOOL，失败如实回
// {ok:false, reason:"lock_failed"}，不再"发个 rundll32 就报 requested"。
void RDPServer::handleWindowsLock(const QString& clientId)
{
    auto reply = [this, clientId](bool ok, const QString& reason, const QString& message) {
        wsServer_->sendJson(clientId, QJsonObject {
            { "type", "lock_result" }, { "ok", ok },
            { "reason", reason }, { "message", message } });
    };

    // [B4] 已有一个请求在等回执：绝不能用新请求覆盖它的 pending —— 被覆盖的那个
    // 客户端再也收不到 lock_result，前端只能等安全网回滚。如实回 busy。
    if (lockPending_) {
        qInfo() << "Lock: busy, previous request from" << lockPendingClientId_.left(8)
                << "still pending (seq" << lockPendingSeq_ << ")";
        reply(false, QStringLiteral("busy"),
              QStringLiteral("上一轮锁定请求仍在等待结果，请稍后再试"));
        return;
    }

    if (screenLocked_) {
        // 幂等：已经锁着就不用再发，前端据此显示"已锁定"。
        reply(true, QStringLiteral("already_locked"), QStringLiteral("屏幕已经是锁定状态"));
        return;
    }

    if (!serviceMode_) {
        // 非服务模式：进程本身就在用户会话（绿色版/调试运行），直接执行。
        const BOOL ok = ::LockWorkStation();
        const DWORD err = ok ? 0 : ::GetLastError();
        qInfo() << "Lock: local LockWorkStation ok =" << bool(ok) << "err =" << err;
        reply(ok != FALSE, ok ? QStringLiteral("locked") : QStringLiteral("lock_failed"),
              ok ? QStringLiteral("已锁屏")
                 : QStringLiteral("LockWorkStation 调用失败，错误码 %1")
                       .arg(static_cast<qulonglong>(err)));
        return;
    }

    const int seq = ++lockSeq_;
    lockPending_ = true;
    lockPendingClientId_ = clientId;
    lockPendingSeq_ = seq;

    if (!isVistaPlusOs()) {
        // [B5] XP：服务与用户同会话，--lock-once 的退出码就是 LockWorkStation 的
        // 真实 BOOL。走和 Vista+ 兜底同一条异步通路（不阻塞主线程），失败回 lock_failed。
        qInfo() << "Lock: XP -> --lock-once (real BOOL), seq =" << seq;
        startLockFallback(seq, clientId, /*helperTimedOut=*/false);
        return;
    }

    if (!wsServer_->isCaptureSourceConnected()) {
        // 服务模式且 helper 不在：**绝不**再退回 Session 0 的 rundll32 —— 它在隔离
        // 会话里空转，屏幕没锁、日志无异常、前端却显示"已锁"（用户反馈的"锁定空操作"）。
        // 如实回失败，前端按 reason 回滚按钮并提示未锁定。
        lockPending_ = false;
        lockPendingClientId_.clear();
        qWarning() << "Lock: no helper in user session, reject (Session-0 fallback removed)";
        reply(false, QStringLiteral("no_helper"),
              QStringLiteral("用户会话 helper 未连接，锁屏未执行"));
        return;
    }

    qInfo() << "Lock: forwarded to helper, seq =" << seq;
    wsServer_->sendToCaptureSource(QJsonObject {
        { "type", "system_action" }, { "action", "lock" } });

    QTimer::singleShot(kLockAckTimeoutMs, this, [this, seq, clientId]() {
        if (!lockPending_ || seq != lockPendingSeq_ || lockPendingClientId_ != clientId)
            return;   // 已被 ack / screen_locked / 新请求收尾
        // helper 在但 3s 内不回执：用 console 会话令牌把本程序以 --lock-once
        // 投到用户会话，由它执行 LockWorkStation() 并用退出码回报成败 ——
        // 这个退出码就是这一次的 ack，不再"投递成功就当锁上了"。
        // [B3] 等待在 LockOnceThread 里做，主线程不会被 WaitForSingleObject 卡住。
        qWarning() << "Lock: helper did not ack in" << kLockAckTimeoutMs
                   << "ms, trying console-session token fallback";
        startLockFallback(seq, clientId, /*helperTimedOut=*/true);
    });
}

// [SYS-ACT] logout 完成性确认用到的进程探测（Windows 专用）。
// 只看"跨进程快照里有没有这个 exe"：不按会话过滤是刻意的 —— XP 上 console、
// 服务、helper 全在 session 0，按会话过滤反而会把 LogonUI 滤掉。产品是单用户
// console 场景；配合 baseline 的"从无到有/从有到无"翻转判断，不会把本来就
// 在跑的 LogonUI（锁屏界面）误当成注销信号。
static bool rdProcessRunning(const wchar_t* exeName)
{
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE)
        return false;
    PROCESSENTRY32W pe;
    ZeroMemory(&pe, sizeof(pe));
    pe.dwSize = sizeof(pe);
    bool found = false;
    if (Process32FirstW(snap, &pe)) {
        do {
            if (_wcsicmp(pe.szExeFile, exeName) == 0) {
                found = true;
                break;
            }
        } while (Process32NextW(snap, &pe));
    }
    CloseHandle(snap);
    return found;
}

// [SYS-ACT] logout：helper 已 ack（shutdown 退出码 0）之后，再等 ≤3s 看会话是否
// 真的开始销毁。证据：登录 UI 出现（LogonUI 从无到有）或用户 shell 退出
// （explorer 从有到无）。两条都拿不到就回 unconfirmed —— 绝不凭退出码报成功。
void RDPServer::beginLogoutConfirmation(const QString& clientId, int seq)
{
    logoutConfirmPending_ = true;
    logoutConfirmSeq_ = seq;
    logoutConfirmClientId_ = clientId;
    logoutConfirmTries_ = 0;
    QTimer::singleShot(300, this, [this]() { tickLogoutConfirmation(); });
}

void RDPServer::tickLogoutConfirmation()
{
    if (!logoutConfirmPending_ || !sysActionPending_
        || sysActionPendingSeq_ != logoutConfirmSeq_)
        return;   // 已被其它路径收尾/作废
    const bool logonUiNow = rdProcessRunning(L"LogonUI.exe");
    const bool explorerNow = rdProcessRunning(L"explorer.exe");
    const bool uiAppeared = !logoutLogonUiBefore_ && logonUiNow;
    const bool shellGone = logoutExplorerBefore_ && !explorerNow;
    if (uiAppeared || shellGone) {
        logoutConfirmPending_ = false;
        sysActionPending_ = false;
        sysActionPendingClientId_.clear();
        sysActionPendingAction_.clear();
        qInfo() << "SysAction: logout confirmed by"
                << (uiAppeared ? "logon UI appeared" : "user shell exited")
                << "seq =" << logoutConfirmSeq_;
        wsServer_->sendJson(logoutConfirmClientId_, QJsonObject {
            { "type", "logout_result" }, { "ok", true },
            { "reason", QStringLiteral("confirmed") },
            { "message", QStringLiteral("已注销（已确认会话退出，进入登录界面）") } });
        return;
    }
    if (++logoutConfirmTries_ >= 10) {   // 10 × 300ms = 3s
        logoutConfirmPending_ = false;
        sysActionPending_ = false;
        sysActionPendingClientId_.clear();
        sysActionPendingAction_.clear();
        qWarning() << "SysAction: logout unconfirmed after 3s, seq =" << logoutConfirmSeq_;
        wsServer_->sendJson(logoutConfirmClientId_, QJsonObject {
            { "type", "logout_result" }, { "ok", false },
            { "reason", QStringLiteral("unconfirmed") },
            { "message", QStringLiteral("注销命令已发出（shutdown 退出码 0），但 3s 内"
                                        "未检测到会话退出（登录界面未出现），结果未知") } });
        return;
    }
    QTimer::singleShot(300, this, [this]() { tickLogoutConfirmation(); });
}

// [SYS-ACT] Windows 的注销/重启/关机回执。
// 为什么必须分三态（与 lock 同一范式）：服务进程在 Session 0，shutdown 在隔离会话
// 里执行只会影响不存在的服务桌面（实测：界面提示"已关机"、机器纹丝不动）。所以
//   ① 非服务模式（进程就在用户会话）→ 本进程直接跑 shutdown，等它的退出码；
//   ② 服务模式 + helper 在 → 转发给 helper，3s 内等 system_action_result；
//   ③ 服务模式 + helper 不在 → 立即 no_helper，不执行任何东西。reboot/poweroff
//      是破坏性动作，必须"先确认 helper 在，再执行"，绝不能先执行再找 helper。
// reboot/poweroff 带 /t 5：既给回执留出送达时间，也给用户一个 shutdown /a 的
// 取消窗口（与 Windows 自身的关机提示语义一致）。
void RDPServer::handleWindowsSysAction(const QString& action, const QString& clientId)
{
    const QString resultType = action + QStringLiteral("_result");
    const QString label = action == QLatin1String("logout")
                              ? QStringLiteral("注销")
                              : action == QLatin1String("reboot")
                                    ? QStringLiteral("重启")
                                    : QStringLiteral("关机");
    auto reply = [this, clientId, resultType](bool ok, const QString& reason,
                                              const QString& message) {
        wsServer_->sendJson(clientId, QJsonObject {
            { "type", resultType }, { "ok", ok }, { "reason", reason },
            { "message", message } });
    };

    if (sysActionPending_) {
        // 同一时刻只允许一个系统操作在途：连着点"重启"和"关机"时，结果会互串。
        reply(false, QStringLiteral("busy"),
              QStringLiteral("上一轮系统操作仍在等待结果，请稍后再试"));
        return;
    }

    QStringList args;
    if (action == "logout") {
        args << QStringLiteral("/l") << QStringLiteral("/f");
    } else {
        args << (action == "reboot" ? QStringLiteral("/r") : QStringLiteral("/s"))
             << QStringLiteral("/t") << QStringLiteral("5") << QStringLiteral("/f");
    }

    if (!serviceMode_) {
        // 本进程就在用户会话里：直接执行，shutdown 的退出码就是这次的真实结果。
        QProcess p;
        p.start(QStringLiteral("shutdown"), args);
        const bool started = p.waitForStarted(1500);
        const bool finished = started && p.waitForFinished(2000);
        const int code = finished ? p.exitCode() : -1;
        const bool ok = started && finished && p.exitStatus() == QProcess::NormalExit
                        && code == 0;
        qInfo() << "SysAction:" << action << "local shutdown ok =" << ok
                << "exit code =" << code << "err =" << p.errorString();
        reply(ok, ok ? QStringLiteral("initiated")
                     : QStringLiteral("%1_failed").arg(action),
              ok ? QStringLiteral("已发起%1").arg(label)
                 : QStringLiteral("%1命令失败（shutdown 退出码 %2）").arg(label).arg(code));
        return;
    }

    if (!wsServer_->isCaptureSourceConnected()) {
        reply(false, QStringLiteral("no_helper"),
              QStringLiteral("用户会话 helper 未连接，%1未执行").arg(label));
        return;
    }

    if (action == QLatin1String("logout")) {
        // 下发前记录 baseline：此时若已锁屏，LogonUI 可能本来就在跑，
        // 只有"从无到有"才算注销信号。
        logoutConfirmPending_ = false;
        logoutConfirmSeq_ = 0;
        logoutLogonUiBefore_ = rdProcessRunning(L"LogonUI.exe");
        logoutExplorerBefore_ = rdProcessRunning(L"explorer.exe");
        qInfo() << "SysAction: logout baseline logonui =" << logoutLogonUiBefore_
                << "explorer =" << logoutExplorerBefore_;
    }

    const int seq = ++sysActionSeq_;
    sysActionPending_ = true;
    sysActionPendingClientId_ = clientId;
    sysActionPendingAction_ = action;
    sysActionPendingSeq_ = seq;
    qInfo() << "SysAction:" << action << "forwarded to helper, seq =" << seq;
    wsServer_->sendToCaptureSource(QJsonObject {
        { "type", "system_action" }, { "action", action }, { "seq", seq } });

    QTimer::singleShot(kLockAckTimeoutMs, this,
                       [this, seq, clientId, action, resultType, label]() {
        if (!sysActionPending_ || seq != sysActionPendingSeq_
            || sysActionPendingClientId_ != clientId)
            return;   // 已被 helper ack 收尾
        if (logoutConfirmPending_ && action == QLatin1String("logout"))
            return;   // 已 ack 成功，确认轮询接管结果（它自己有 3s 预算）
        sysActionPending_ = false;
        sysActionPendingClientId_.clear();
        sysActionPendingAction_.clear();
        qWarning() << "SysAction:" << action << "helper did not ack in"
                   << kLockAckTimeoutMs << "ms";
        wsServer_->sendJson(clientId, QJsonObject {
            { "type", resultType }, { "ok", false },
            { "reason", QStringLiteral("timeout") },
            { "message", QStringLiteral("用户会话没有在 3s 内确认「%1」，操作结果未知")
                             .arg(label) } });
    });
}
#endif

void RDPServer::handleSystemAction(const QString& action, const QString& clientId,
                                   const QString& password, const QString& username)
{
    qInfo() << "System action requested:" << action << "from" << clientId.left(8);

#ifdef Q_OS_WIN32
    if (action == "login_screen") {
        // 登录界面远程登录：与 unlock_screen 是两条链路。
        // unlock_screen 针对"已有用户会话的锁屏"；login_screen 针对
        // "注销后/开机停登录界面（无任何用户会话）"，不依赖 helper 与用户令牌。
        handleWindowsLoginScreen(clientId, username, password);
        return;
    }
    if (action == "unlock_screen") {
        // Windows 免密解锁：锁屏 UI 运行在 winlogon 安全桌面，普通进程（含
        // helper，同在用户会话）的 SendInput 到不了那里，唯一通路是
        // secure-input 进程（服务端在收到 screen_locked=true 时自动拉起）。
        // 密码优先用前端这次输入的（前端「输入密码解锁」走的就是这条路），
        // 没给才回退到配置里的 unlockPassword（「一键解锁」按钮）。
        const QString pwd = password.isEmpty() ? unlockPassword_ : password;
        if (serviceMode_ && !screenLocked_ && !wsServer_->isCaptureSourceConnected()) {
            // 开机锁屏（区别于"屏保恢复锁屏"）：helper 是服务用登录用户令牌拉起来的，
            // 它没连上 = 会话里根本没人登录。这时屏幕上的是首次登录界面，Windows 不允许
            // 任何远程首登（不做 GINA 替换的话），报"屏幕未锁定"会把人引到死路上。
            wsServer_->sendJson(clientId, QJsonObject {
                { "type", "unlock_result" }, { "ok", false },
                { "reason", "no_user_session" },
                { "message", "被控机还没有已登录的用户会话（开机锁屏）："
                             "首次登录无法远程完成，请在机器前登录一次" } });
        } else if (!screenLocked_) {
            wsServer_->sendJson(clientId, QJsonObject {
                { "type", "unlock_result" }, { "ok", true },
                { "reason", "not_locked" },
                { "message", "屏幕未锁定" } });
        } else if (pwd.isEmpty()) {
            wsServer_->sendJson(clientId, QJsonObject {
                { "type", "unlock_result" }, { "ok", false },
                { "reason", "no_password" },
                { "message", "未配置解锁密码：在 server_config.json 中设置 unlockPassword（明文，局域网自用）" } });
        } else {
            // XP 与 Vista+ 走同一条通路，差异全在 secure-input 里：
            //   XP 屏保恢复锁屏 → 先结束 .scr 让 Winlogon 把桌面切回登录桌面，
            //   再点屏幕中偏右的用户磁贴把密码框展开，最后键入密码回车。
            // XP 的输入桌面在锁屏期间会变（Screen-saver → Winlogon），所以不要在这里
            // 提前发 SAS —— SAS 会把欢迎屏幕重排回屏保形态，反而打断已经点开的密码框。
            bool justStarted = false;
            if (!wsServer_->isSecureInputConnected()) {
                startSecureInputProcess();
                justStarted = true; // 进程刚拉起，连接建立需要时间，注入整体延后
            }
            if (!justStarted && !wsServer_->isSecureInputConnected()) {
                wsServer_->sendJson(clientId, QJsonObject {
                    { "type", "unlock_result" }, { "ok", false },
                    { "reason", "channel_not_ready" },
                    { "message", "安全输入通道未就绪，请稍后重试" } });
                return;
            }
            const QString cid = clientId;
            // [C] 新一轮解锁：seq 自增会作废上一轮所有延时回调（连点两次不会叠加
            // 两套重试/终判），同时清空上一轮的"已键入/已明确拒绝"状态。
            const int seq = ++unlockSeq_;
            secureUnlockTyped_ = false;
            secureUnlockDefinite_ = false;
            secureUnlockBlockedReported_ = false;
            auto sendUnlock = [this, pwd, seq]() {
                wsServer_->sendToSecureInput(QJsonObject {
                    { "type", "unlock" }, { "password", pwd }, { "seq", seq } });
            };
            // 解锁序列期间禁止按"桌面不符"重投 secure-input（SAS 一定会换桌面）；
            // 30s = 本函数下面两轮重试 + 终判的最长链路，到期自动失效，不留永久门禁。
            secureInputUnlockUntilMs_ = monoNowMs() + 30000;
            // [B] 记下这次解锁是哪个客户端发起的：secure-input 的注入前校验拒绝注入时，
            // 拦截原因要回到这个客户端（见构造里的 secureInputMessageReceived 连接）。
            secureInputUnlockClientId_ = clientId;
            const int base = justStarted ? 1500 : 0;
            // 把密码交给 secure-input 进程，由它在安全桌面上判断界面形态后自己处理：
            //   传统登录对话框 → 直接写控件 + 提交；
            //   XP 屏保桌面   → 先结束 .scr，桌面切回登录桌面后点用户磁贴 + 键入密码；
            //   欢迎屏幕      → 点用户磁贴把密码框展开 + 键入密码。
            // 这里不要再无条件先发 SAS：若对话框已就绪，多按一次 Ctrl+Alt+Del 会把它切走。
            QTimer::singleShot(base, this, [this, seq, sendUnlock]() {
                if (seq != unlockSeq_)
                    return;
                sendUnlock();
            });
            // [C] 第二轮改成**有条件**的：只有"这一轮确实没键入"才补发。
            //   Vista+：secure-input 报了 typed → 不补发（补发会落在已登录的桌面上，
            //           把明文密码敲进普通窗口）；报了 blocked(not_logon_ui) → 明确拒绝，
            //           也不补发（弹窗态下再发一次只会再来一条 blocked）。
            //   报了 uncertain（UIA 超时）→ 允许补发一次，这正是"再试一次"有用的场景。
            //   XP：secure-input 不上报进度，条件永远成立，所以仍是 8s 补发 + 15s 终判
            //       （安全屏保 SAS→桌面切换整链实测 8~12s，不能更早）。
            QTimer::singleShot(base + 8000, this, [this, seq, sendUnlock]() {
                if (seq != unlockSeq_ || !screenLocked_)
                    return;
                if (secureUnlockTyped_ || secureUnlockDefinite_) {
                    qInfo() << "Unlock: skip 8s retry (typed =" << secureUnlockTyped_
                            << ", definite block =" << secureUnlockDefinite_ << ")";
                    return;
                }
                qInfo() << "Unlock: still locked and nothing typed - retrying once";
                sendUnlock();
                QTimer::singleShot(15000, this, [this, seq]() {
                    if (seq != unlockSeq_ || !screenLocked_)
                        return;
                    // 已经上报过"拒绝注入"就别再补一句"密码未通过验证"（自相矛盾）。
                    if (secureUnlockTyped_ || secureUnlockBlockedReported_)
                        return;
                    wsServer_->sendJson(secureInputUnlockClientId_, QJsonObject {
                        { "type", "unlock_result" }, { "ok", false },
                        { "reason", "wrong_password" },
                        { "message", "密码未通过验证（unlockPassword 与 Windows 登录密码不一致？）" } });
                });
            });
            wsServer_->sendJson(clientId, QJsonObject {
                { "type", "unlock_result" }, { "ok", true }, { "pending", true },
                { "reason", "injecting" },
                { "message", "正在注入解锁密码…" } });
        }
        return;
    }

    // [LOCK] 锁屏单独成支：Windows 不再走下面"服务模式无脑转发 / 本地 rundll32"
    // 的通用路径（那条路径在 Session 0 上是空操作，且没有任何结果回报）。
    if (action == "lock") {
        handleWindowsLock(clientId);
        return;
    }

    // [SYS-ACT] 注销/重启/关机同样单独成支：通用路径只会转发给 helper 且
    // **没有任何回执**，前端无法区分"已发起"和"根本没执行"；helper 不在时
    // 还会落到 Session 0 的本地执行（对注销/关机是彻底的空操作）。
    if (action == "logout" || action == "reboot" || action == "poweroff") {
        handleWindowsSysAction(action, clientId);
        return;
    }
#endif

    // 服务模式（有 helper，如 Windows）：系统操作必须在用户会话执行。
    // 服务进程运行在 Session 0（Windows）或独立会话，直接执行只会作用于
    // 不可见的服务会话（显示桌面/任务管理器/注销均无效）。转发 helper。
    // unlock_screen 例外：helper 侧没有这个分支（Windows 的解锁在上面已按
    // secure-input 路径返回），Linux/Android 的解锁靠本进程下面的 xdotool/
    // dm-tool 分支完成 —— 一律转发等于没人执行，前端点了「解锁」什么也不会发生。
    if (serviceMode_ && wsServer_->isCaptureSourceConnected()
        && action != QLatin1String("unlock_screen")) {
        wsServer_->sendToCaptureSource(QJsonObject {
            { "type", "system_action" },
            { "action", action },
        });
        return;
    }

#if defined(Q_OS_LINUX) && !defined(Q_OS_ANDROID)
    // ================= Linux 系统操作：多环境通用处理 =================
    // 不硬编码单一桌面环境。按“可用命令 → 桌面环境 → X11/Wayland”逐级探测，
    // 覆盖 LXDE / GNOME / XFCE / KDE / MATE 与 X11 / Wayland。
    // 子进程继承服务进程的 DISPLAY/XAUTHORITY（detectUserX11Env 已设置），GUI 工具可正常弹出。
    auto findBin = [](const char* name) -> QString {
        return QStandardPaths::findExecutable(QString::fromLatin1(name));
    };
    auto firstOf = [](std::initializer_list<const char*> names) -> QString {
        for (const char* n : names) {
            QString b = QStandardPaths::findExecutable(QString::fromLatin1(n));
            if (!b.isEmpty())
                return b;
        }
        return QString();
    };
    auto launch = [](const QString& bin, const QStringList& args = {}) {
        if (!bin.isEmpty())
            QProcess::startDetached(bin, args);
    };

    if (action == "lock") {
        // 锁屏链：每步执行后都验证 LockedHint，命令“退出 0 但没锁上”不算成功。
        //  1) loginctl lock-session <桌面会话> —— DE 无关，GNOME/light-locker/
        //     xfce4-screensaver 注册 logind 后由此触发。
        //  2) xdg-screensaver / gnome-screensaver-command / xflock4 —— 以会话用户
        //     身份（runuser + DISPLAY/XAUTHORITY/DBUS_SESSION_BUS_ADDRESS）执行，
        //     覆盖 locker 未注册 logind 的环境。
        //  3) dm-tool lock（lightdm，需 XDG_SEAT_PATH）—— 兜底：GNOME 40 的
        //     ScreenShield 在部分发行版（Anolis 8.10 实测）是空实现，前两步全部
        //     静默失败，只有 lightdm 切 greeter VT 能真正锁住屏幕。
        //     对应解锁走 unlock_screen 的 unlock-session + chvt。
        QString sh = findBin("sh");
        if (sh.isEmpty()) {
            wsServer_->sendJson(clientId, QJsonObject {
                { "type", "lock_result" }, { "ok", false },
                { "message", "系统缺少 sh，无法执行锁屏" } });
            return;
        }
        // [LINUX-LOCK-SERIAL] 锁屏串行化：并发两路 dm-tool/loginctl 时，第二路
        // 可能在前台已切到 greeter 之后再发一次，命令返回 0 但实际无状态变化。
        // 复用 lockPending_（Windows 下用于等待 helper 回执）：第一路占位到子进程
        // 结束，并发的第二路直接回 busy，不再并发下发锁屏命令。
        if (lockPending_) {
            wsServer_->sendJson(clientId, QJsonObject {
                { "type", "lock_result" }, { "ok", false },
                { "reason", "busy" },
                { "message", "另一个锁屏请求正在执行，请稍候" } });
            return;
        }
        if (screenLocked_) {
            wsServer_->sendJson(clientId, QJsonObject {
                { "type", "lock_result" }, { "ok", true },
                { "reason", "already_locked" },
                { "message", "屏幕已锁定" } });
            return;
        }
        lockPending_ = true;
        QStringList lines;
        lines << "SESS=''; UID_=''; VT=''; DISP=''";
        lines << "for s in $(loginctl list-sessions --no-legend 2>/dev/null | awk '{print $1}'); do";
        lines << "  t=$(loginctl show-session \"$s\" -p Type --value 2>/dev/null)";
        lines << "  seat=$(loginctl show-session \"$s\" -p Seat --value 2>/dev/null)";
        lines << "  cls=$(loginctl show-session \"$s\" -p Class --value 2>/dev/null)";
        lines << "  act=$(loginctl show-session \"$s\" -p Active --value 2>/dev/null)";
        // Class=user 排除 lightdm greeter 会话（同为 x11/seat0，但它不是桌面）
        lines << "  if { [ \"$t\" = x11 ] || [ \"$t\" = wayland ]; } && [ \"$seat\" = seat0 ] && [ \"$cls\" = user ]; then";
        lines << "    SESS=\"$s\"";
        lines << "    UID_=$(loginctl show-session \"$s\" -p User --value 2>/dev/null)";
        lines << "    VT=$(loginctl show-session \"$s\" -p VTNr --value 2>/dev/null)";
        lines << "    DISP=$(loginctl show-session \"$s\" -p Display --value 2>/dev/null)";
        lines << "    [ \"$act\" = yes ] && break";
        lines << "  fi";
        lines << "done";
        lines << "DISP_USE=\"${DISP:-$DISPLAY}\"";
        lines << "XAUTH_USE=\"${XAUTHORITY:-}\"";
        lines << "HOW=''";
        lines << "if [ -n \"$SESS\" ]; then";
        lines << "  loginctl lock-session \"$SESS\" >/dev/null 2>&1";
        lines << "  sleep 2";
        lines << "  [ \"$(loginctl show-session \"$SESS\" -p LockedHint --value 2>/dev/null)\" = yes ] && HOW=loginctl";
        lines << "fi";
        lines << "if [ -z \"$HOW\" ]; then";
        lines << "  RU=''";
        lines << "  [ \"$(id -u)\" = 0 ] && [ -n \"$UID_\" ] && RU=\"runuser -u $UID_ --\"";
        lines << "  DBUS=\"unix:path=/run/user/$UID_/bus\"";
        lines << "  for c in \"xdg-screensaver lock\" \"gnome-screensaver-command -l\" \"xflock4\"; do";
        lines << "    bin=${c%% *}";
        lines << "    command -v \"$bin\" >/dev/null 2>&1 || continue";
        lines << "    $RU env DISPLAY=\"$DISP_USE\" XAUTHORITY=\"$XAUTH_USE\" DBUS_SESSION_BUS_ADDRESS=\"$DBUS\" $c >/dev/null 2>&1";
        lines << "    sleep 2";
        lines << "    if [ -n \"$SESS\" ] && [ \"$(loginctl show-session \"$SESS\" -p LockedHint --value 2>/dev/null)\" = yes ]; then HOW=\"$bin\"; break; fi";
        lines << "  done";
        lines << "fi";
        lines << "if [ -z \"$HOW\" ] && [ \"$(id -u)\" = 0 ] && command -v dm-tool >/dev/null 2>&1 && pgrep -x lightdm >/dev/null 2>&1; then";
        lines << "  XDG_SEAT_PATH=/org/freedesktop/DisplayManager/Seat0 dm-tool lock >/dev/null 2>&1 && HOW=dm-tool";
        lines << "fi";
        lines << "if [ -n \"$HOW\" ]; then sleep 1; fi";
        lines << "CUR_VT=$(cat /sys/class/tty/tty0/active 2>/dev/null)";
        lines << "LOCKED=''; GREETER=no; GREETER_VT=''";
        lines << "if [ -n \"$SESS\" ]; then LOCKED=$(loginctl show-session \"$SESS\" -p LockedHint --value 2>/dev/null); fi";
        lines << "for s in $(loginctl list-sessions --no-legend 2>/dev/null | awk '{print $1}'); do";
        lines << "  cls=$(loginctl show-session \"$s\" -p Class --value 2>/dev/null)";
        lines << "  st=$(loginctl show-session \"$s\" -p State --value 2>/dev/null)";
        lines << "  if [ \"$cls\" = greeter ] && [ \"$st\" = active ]; then";
        lines << "    GREETER=yes; GREETER_VT=$(loginctl show-session \"$s\" -p VTNr --value 2>/dev/null)";
        lines << "  fi";
        lines << "done";
        lines << "echo \"HOW=$HOW SESS=$SESS DESKTOP_VT=$VT CUR_VT=$CUR_VT LOCKED=$LOCKED GREETER=$GREETER GREETER_VT=$GREETER_VT\"";

        QProcess* proc = new QProcess(this);
        const QString cid = clientId;
        connect(proc, QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished),
                this, [this, proc, cid](int, QProcess::ExitStatus) {
            const QString out = QString::fromLocal8Bit(proc->readAllStandardOutput()).trimmed();
            QString how, desktopVt, curVt, lockedHint, greeter, greeterVt;
            const QStringList toks = out.split(QLatin1Char(' '));
            for (const QString& tok : toks) {
                if (tok.startsWith(QLatin1String("HOW=")))             how = tok.mid(4);
                else if (tok.startsWith(QLatin1String("DESKTOP_VT="))) desktopVt = tok.mid(11);
                else if (tok.startsWith(QLatin1String("CUR_VT=")))     curVt = tok.mid(7);
                else if (tok.startsWith(QLatin1String("LOCKED=")))     lockedHint = tok.mid(7);
                else if (tok.startsWith(QLatin1String("GREETER_VT="))) greeterVt = tok.mid(11);
                else if (tok.startsWith(QLatin1String("GREETER=")))    greeter = tok.mid(8);
            }
            // 三路交叉证据，任一成立才算真的锁上（不能只信命令退出码）：
            //  1) logind LockedHint=yes —— DE locker 注册了 logind 的路径；
            //  2) 前台 VT 从桌面会话的 VT 切走 —— lightdm greeter / chvt 型锁屏。
            //     这类锁屏下原桌面的 X server 仍在跑、XGetImage 依然成功，
            //     所以"采集正常"不能作为"未锁屏"的证据；
            //  3) 当前 VT 上有一个 active 的 greeter 会话 —— 登录器确实接管了屏幕。
            const bool howOk = !how.isEmpty();
            const bool locked = lockedHint == QLatin1String("yes");
            const bool vtChanged = !desktopVt.isEmpty() && !curVt.isEmpty()
                                   && curVt != (QStringLiteral("tty") + desktopVt);
            const bool greeterVisible = !greeterVt.isEmpty() && !curVt.isEmpty()
                                        && greeter == QLatin1String("yes")
                                        && curVt == (QStringLiteral("tty") + greeterVt);
            const bool confirmed = howOk && (locked || vtChanged || greeterVisible);
            QString msg, reason;
            if (confirmed) {
                if (how == QLatin1String("dm-tool"))
                    msg = QStringLiteral("已锁屏（登录器接管，画面为黑属正常，用「解锁屏幕」切回）");
                else if (how == QLatin1String("loginctl"))
                    msg = QStringLiteral("已锁屏");
                else
                    msg = QStringLiteral("已锁屏（%1）").arg(how);
            } else if (!howOk) {
                reason = QStringLiteral("lock_failed");
                msg = QStringLiteral("未能锁定屏幕：无可用的锁屏机制");
            } else {
                // 命令执行了但状态没有变化：如实回报，不乐观成功。
                reason = QStringLiteral("lock_unconfirmed");
                msg = QStringLiteral("锁屏命令已执行（%1），但未观测到 VT/登录器/LockedHint 变化").arg(how);
            }
            qInfo() << "lock result:" << confirmed << "how=" << how
                    << "vt:" << desktopVt << "->" << curVt
                    << "lockedHint=" << lockedHint << "greeter=" << greeter
                    << "greeterVt=" << greeterVt << msg;
            wsServer_->sendJson(cid, QJsonObject {
                { "type", "lock_result" }, { "ok", confirmed },
                { "reason", reason }, { "message", msg } });
            if (confirmed) {
#if defined(Q_OS_LINUX) && !defined(Q_OS_ANDROID)
                // 采集不依赖"锁屏后必然失败"，这里把已确认的锁定状态直接落地并
                // 广播，让前端立刻出现遮罩（不依赖下一次抓帧或 VT 判定缓存）。
                if (screenCapturer_) {
                    screenCapturer_->setLockedExternally(true);
                } else if (!screenLocked_) {
                    screenLocked_ = true;
                    broadcastScreenLocked(QJsonObject {
                        { "type", "screen_locked" }, { "locked", true },
                        { "platform", platformTag() },
                        { "hint", QStringLiteral("屏幕已锁屏，点「解锁屏幕」切回桌面") } });
                }
#endif
            }
            lockPending_ = false;
            proc->deleteLater();
        });
        connect(proc, &QProcess::errorOccurred, this, [this, proc, cid](QProcess::ProcessError) {
            lockPending_ = false;
            wsServer_->sendJson(cid, QJsonObject {
                { "type", "lock_result" }, { "ok", false },
                { "reason", "lock_failed" },
                { "message", "锁屏命令启动失败: " + proc->errorString() } });
            proc->deleteLater();
        });
        proc->start(sh, { QStringLiteral("-c"), lines.join(QLatin1Char('\n')) });
        // 命令挂死（runuser/lock 工具卡在会话总线上）时不留常驻子进程：
        // 15s 后强杀，finished 处理器会照常回一条 "未锁定" 结果。
        QTimer::singleShot(15000, proc, [proc]() {
            if (proc->state() != QProcess::NotRunning)
                proc->kill();
        });
    } else if (action == "unlock_screen") {
        // 免密解锁：让被控端从锁屏界面切回桌面（方案A）。
        //  1) loginctl unlock-session —— 解开 light-locker / gnome-screensaver 这类
        //     “同一 VT 上把屏幕遮住”的锁屏（会话没切换，只是屏幕被盖住）。
        //  2) chvt <桌面VT> —— 解开 lightdm greeter 型锁屏：lightdm 锁屏时会在另一
        //     个 VT（如 tty8）另起一个 greeter 并把前台 VT 切过去，桌面会话仍在原
        //     VT（如 tty7），X :0 画面被置黑，采集到的就是全黑帧。实测对这种情况
        //     loginctl unlock-session / dm-tool switch-to-user 都无效，只有把前台 VT
        //     切回桌面 VT 才能恢复画面。
        // 桌面 VT 由 loginctl 中的图形会话（Type=x11/wayland @ seat0）推导，不硬编码。
        // chvt 需要 root（服务以 root 运行时可直接执行）。
        QString sh = findBin("sh");
        if (sh.isEmpty()) {
            wsServer_->sendJson(clientId, QJsonObject {
                { "type", "unlock_result" }, { "ok", false },
                { "message", "系统缺少 sh，无法执行解锁" } });
            return;
        }
        QStringList lines;
        lines << "VT=''; SESS=''";
        lines << "for s in $(loginctl list-sessions --no-legend 2>/dev/null | awk '{print $1}'); do";
        lines << "  t=$(loginctl show-session \"$s\" -p Type --value 2>/dev/null)";
        lines << "  seat=$(loginctl show-session \"$s\" -p Seat --value 2>/dev/null)";
        lines << "  cls=$(loginctl show-session \"$s\" -p Class --value 2>/dev/null)";
        // VTNr 而非 TTY：GNOME 等会话 TTY 可能为空（.242 实测），VTNr 始终有效；
        // Class=user 排除 lightdm greeter 会话（同为 x11/seat0 但 VT 是 greeter 的）。
        lines << "  if { [ \"$t\" = x11 ] || [ \"$t\" = wayland ]; } && [ \"$seat\" = seat0 ] && [ \"$cls\" = user ]; then";
        lines << "    VT=$(loginctl show-session \"$s\" -p VTNr --value 2>/dev/null)";
        lines << "    SESS=\"$s\"; break";
        lines << "  fi";
        lines << "done";
        lines << "[ -n \"$SESS\" ] && loginctl unlock-session \"$SESS\" >/dev/null 2>&1";
        lines << "CUR=$(cat /sys/class/tty/tty0/active 2>/dev/null)";
        lines << "if [ -n \"$VT\" ] && [ \"$CUR\" != \"tty$VT\" ]; then";
        lines << "  chvt \"$VT\" >/dev/null 2>&1";
        lines << "  CUR=$(cat /sys/class/tty/tty0/active 2>/dev/null)";
        lines << "fi";
        lines << "echo \"VT=tty$VT CUR=$CUR\"";

        QProcess* proc = new QProcess(this);
        const QString cid = clientId;
        connect(proc, QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished),
                this, [this, proc, cid](int, QProcess::ExitStatus) {
            QString vt, cur;
            const QStringList toks = QString::fromLocal8Bit(proc->readAllStandardOutput()).trimmed().split(QLatin1Char(' '));
            for (const QString& tok : toks) {
                if (tok.startsWith(QLatin1String("VT=")))       vt  = tok.mid(3);
                else if (tok.startsWith(QLatin1String("CUR="))) cur = tok.mid(4);
            }
            const bool ok = !vt.isEmpty() && vt == cur;
            QString msg;
            if (ok)
                msg = QStringLiteral("已切回桌面 %1").arg(vt);
            else if (vt.isEmpty())
                msg = QStringLiteral("未识别到图形会话所在 VT");
            else
                msg = QStringLiteral("未切换成功（当前 %1 / 目标 %2），解锁需 root 权限").arg(cur, vt);
            qInfo() << "unlock_screen result:" << ok << msg;
            wsServer_->sendJson(cid, QJsonObject {
                { "type", "unlock_result" }, { "ok", ok }, { "message", msg } });
            if (ok) {
#if defined(Q_OS_LINUX) && !defined(Q_OS_ANDROID)
                // 解锁后必须回到 locked:false（并恢复采集画面）：把确认结果直接
                // 落地并广播，避免前端遮罩因判定缓存/时序残留。
                if (screenCapturer_) {
                    screenCapturer_->setLockedExternally(false);
                } else if (screenLocked_) {
                    screenLocked_ = false;
                    broadcastScreenLocked(QJsonObject {
                        { "type", "screen_locked" }, { "locked", false },
                        { "platform", platformTag() }, { "hint", QString() } });
                }
#endif
            }
            proc->deleteLater();
        });
        connect(proc, &QProcess::errorOccurred, this, [this, proc, cid](QProcess::ProcessError) {
            wsServer_->sendJson(cid, QJsonObject {
                { "type", "unlock_result" }, { "ok", false },
                { "message", "解锁命令启动失败: " + proc->errorString() } });
            proc->deleteLater();
        });
        proc->start(sh, { QStringLiteral("-c"), lines.join(QLatin1Char('\n')) });
        QTimer::singleShot(15000, proc, [proc]() {
            if (proc->state() != QProcess::NotRunning)
                proc->kill();
        });
    } else if (action == "show_desktop") {
        // 显示桌面链：
        //  0) X11 EWMH 直发 _NET_SHOWING_DESKTOP —— 最可靠，不依赖外部工具；
        //     GNOME(mutter) 实测不响应 XTEST 合成的 Super+D，必须走此协议。
        //  1) wmctrl -k on —— EWMH 标准，openbox/LXDE、GNOME、XFCE、KDE 的 X11 会话均支持；
        //  2) xdotool 模拟 Super+D（GNOME/XFCE/KDE 快捷键；openbox 若未绑定可改 Ctrl+Alt+D）；
        //  3) GNOME Wayland（无 EWMH）→ Shell D-Bus 显示桌面。
        x11ShowDesktopOnce(true);
        QString sh = findBin("wmctrl");
        if (!sh.isEmpty()) {
            launch(sh, { "-k", "on" });
        } else if (!(sh = findBin("xdotool")).isEmpty()) {
            launch(sh, { "key", "--clearmodifiers", "super+d" });
        } else if (!(sh = findBin("gdbus")).isEmpty()) {
            launch(sh, { "call", "--session", "--dest", "org.gnome.Shell",
                         "--object-path", "/org/gnome/Shell",
                         "--method", "org.gnome.Shell.Eval",
                         "global.activate_action('show-desktop', null)" });
        } else {
            qWarning() << "show_desktop: no wmctrl/xdotool/gdbus available on this system";
        }
    } else if (action == "task_manager") {
        // 任务管理器链：按 DE 依次探测（LXDE/XFCE/MATE/GNOME/KDE）
        QString tm = firstOf({ "lxtask", "xfce4-taskmanager", "mate-system-monitor",
                               "gnome-system-monitor", "ksysguard", "plasma-systemmonitor" });
        if (!tm.isEmpty())
            launch(tm);
        else
            qWarning() << "task_manager: no supported task manager found";
    } else if (action == "logout") {
        // 注销链：lxsession/lxde → gnome-session-quit → xfce4-session-logout → KDE qdbus
        QString lo = firstOf({ "lxsession-logout", "lxde-logout", "gnome-session-quit",
                               "xfce4-session-logout" });
        if (lo.endsWith(QStringLiteral("gnome-session-quit")))
            launch(lo, { "--logout", "--force", "--no-prompt" });
        else if (lo.endsWith(QStringLiteral("xfce4-session-logout")))
            launch(lo, { "--logout" });
        else if (!lo.isEmpty())
            launch(lo);
        else {
            QString q = firstOf({ "qdbus6", "qdbus" });
            if (!q.isEmpty())
                launch(q, { "org.kde.ksmserver", "/KSMServer",
                            "org.kde.KSMServerInterface.logout", "0", "0", "0" });
            else
                qWarning() << "logout: no supported session manager found";
        }
    } else if (action == "reboot") {
        // systemd → 传统 shutdown
        QString b = findBin("systemctl");
        if (!b.isEmpty())
            launch(b, { "reboot" });
        else
            launch(findBin("shutdown"), { "-r", "now" });
    } else if (action == "poweroff") {
        QString b = findBin("systemctl");
        if (!b.isEmpty())
            launch(b, { "poweroff" });
        else
            launch(findBin("shutdown"), { "-h", "now" });
    } else {
        qWarning() << "Unknown system action:" << action;
    }
#elif defined(_WIN32)
    // [LOCK] lock 已在 handleSystemAction 开头单独成支（handleWindowsLock）。
    // 这里保留旧的 rundll32 就等于把"锁定空操作"留给没被拦住的路径：
    // Session 0 上的 LockWorkStation 只作用于不可见的服务会话，屏幕不会锁，
    // 却没有任何失败回报。所以 lock 分支已从这里删除。
    if (action == "show_desktop") {
        // 显示桌面：Shell COM 接口 ToggleDesktop（Win2000+ 的 Shell.Application 均有）。
        // Win7+ 用 PowerShell；WinXP 无 powershell.exe → 用 cscript 执行同款 COM 调用。
        QString ps = QStandardPaths::findExecutable(QStringLiteral("powershell.exe"));
        if (!ps.isEmpty()) {
            QProcess::startDetached(ps, {
                QStringLiteral("-NoProfile"), QStringLiteral("-WindowStyle"), QStringLiteral("Hidden"),
                QStringLiteral("-Command"),
                QStringLiteral("(New-Object -ComObject Shell.Application).ToggleDesktop()") });
        } else {
            QString vbs = QDir::tempPath() + QStringLiteral("/rd_toggle_desktop.vbs");
            QFile f(vbs);
            if (f.open(QIODevice::WriteOnly)) {
                f.write("Set sh = CreateObject(\"Shell.Application\")\r\nsh.ToggleDesktop\r\n");
                f.close();
                QProcess::startDetached(QStringLiteral("cscript.exe"),
                    { QStringLiteral("//nologo"), vbs });
            }
        }
    } else if (action == "task_manager") {
        QProcess::startDetached(QStringLiteral("taskmgr"));
    } else if (action == "logout") {
        QProcess::startDetached(QStringLiteral("shutdown"), { QStringLiteral("/l"), QStringLiteral("/f") });
    } else if (action == "reboot") {
        QProcess::startDetached(QStringLiteral("shutdown"), { QStringLiteral("/r"), QStringLiteral("/t"), QStringLiteral("0") });
    } else if (action == "poweroff") {
        QProcess::startDetached(QStringLiteral("shutdown"), { QStringLiteral("/s"), QStringLiteral("/t"), QStringLiteral("0") });
    } else {
        qWarning() << "Unknown system action:" << action;
    }
#elif defined(Q_OS_MACOS)
    if (action == "lock") {
        QProcess::startDetached(QStringLiteral(
            "/System/Library/CoreServices/Menu Extras/User.menu/Contents/Resources/CGSession"),
            { QStringLiteral("-suspend") });
    } else if (action == "show_desktop") {
        // 显示桌面：模拟 F11 "显示桌面" 热键（尽力而为，需辅助功能授权）
        QProcess::startDetached(QStringLiteral("osascript"), { QStringLiteral("-e"),
            QStringLiteral("tell application \"System Events\" to key code 103") });
    } else if (action == "task_manager") {
        QProcess::startDetached(QStringLiteral("open"), { QStringLiteral("-a"), QStringLiteral("Activity Monitor") });
    } else if (action == "logout") {
        QProcess::startDetached(QStringLiteral("osascript"), { QStringLiteral("-e"),
            QStringLiteral("tell app \"System Events\" to log out") });
    } else if (action == "reboot") {
        QProcess::startDetached(QStringLiteral("osascript"), { QStringLiteral("-e"),
            QStringLiteral("tell app \"System Events\" to restart") });
    } else if (action == "poweroff") {
        QProcess::startDetached(QStringLiteral("osascript"), { QStringLiteral("-e"),
            QStringLiteral("tell app \"System Events\" to shut down") });
    } else {
        qWarning() << "Unknown system action:" << action;
    }
#endif
}

void RDPServer::onClientConnected(const QString& clientId)
{
    // Validate auth token
    QString token = wsServer_->clientToken(clientId);
    // 不回显 token 内容（即使是前缀）——日志文件可能被多人访问，避免会话令牌泄漏
    qDebug() << "Client connecting, token present:" << !token.isEmpty();
    if (token.isEmpty() || !authManager_->validateSession(token)) {
        qWarning() << "Client rejected (invalid" << (token.isEmpty() ? "empty" : "bad") << "token):" << clientId;
        wsServer_->dropClient(clientId);
        return;
    }
    qInfo() << "Client connected:" << clientId;

    // 首个客户端连接 → 阻止被控端睡眠/熄屏（远程会话期间保持唤醒）
    if (wsServer_->clientCount() == 1)
        updateSleepInhibit(true);
    // [A17] 有观众了 → 解除"无观众"导致的剪贴板轮询挂起（helper 通道活跃时仍挂起）
    updateClipboardSuspend();

    // 有客户端连接 → 恢复屏幕捕获
    if (screenCapturer_)
        screenCapturer_->resume();
    if (serviceMode_ && wsServer_->isCaptureSourceConnected()) {
        QJsonObject msg;
        msg["type"] = "capture_control";
        msg["action"] = "resume";
        wsServer_->sendToCaptureSource(msg);
    }

    // 发送当前剪贴板内容（若有，文本/图片）。读取是异步的，结果回来后
    // 由 contentReady 处理器按 pendingClipboardClientIds_ 推送。
    // [N6] 服务模式下 helper 通道活跃时跳过：上方 capture_control resume 会让
    // helper 上报初始剪贴板（否则 xclip 轮询 + helper 双路各推一份）
    if (clipboardService_ && !(serviceMode_ && wsServer_->isCaptureSourceConnected())) {
        pendingClipboardClientIds_.append(clientId);
        clipboardService_->requestContent();
    }

    // 补发当前远端拖拽状态：remote_drag 只在状态变化时广播，后接入的
    // 客户端必须拿到"此刻正在拖拽"的快照，否则错过边缘下载时机。
    if (!lastRemoteDragMsg_.isEmpty())
        wsServer_->sendJson(clientId, lastRemoteDragMsg_);

    // 发送屏幕分辨率
    if (screenCapturer_) {
        // 新连接的初始归一化基准/多屏偏移也要立刻同步（首次 startCapture 后
        // 没有锁屏事件的话，旧代码要等到 set_output/set_resolution 才设置）。
        if (inputManager_) {
            inputManager_->setScreenSize(screenCapturer_->width(), screenCapturer_->height());
            inputManager_->setScreenOffset(screenCapturer_->originX(), screenCapturer_->originY());
        }
        QJsonObject info;
        info["type"] = "screen_info";
        info["width"] = screenCapturer_->width();
        info["height"] = screenCapturer_->height();
        info["platform"] = platformTag();
        wsServer_->sendJson(clientId, info);
    } else if (serviceMode_ && !lastScreenInfo_.isEmpty()) {
        wsServer_->sendJson(clientId, lastScreenInfo_);
    }

#ifdef Q_OS_WIN32
    // [Win-登录] login_screen_state 只在状态切换时广播；后接入的客户端必须补一份
    // 快照，否则刷新页面后「远程登录」入口消失（界面停在登录界面却无从操作）。
    if (serviceMode_ && loginScreenActive_) {
        QJsonObject st;
        st["type"] = "login_screen_state";
        st["active"] = true;
        st["username"] = effectiveLoginUsername();
        st["platform"] = QStringLiteral("windows");
        st["isXP"] = !isVistaPlusOs();
        wsServer_->sendJson(clientId, st);
    }
#endif

    // 发送当前画质/帧率/分辨率配置（前端据此显示正确的标签）
    {
        QJsonObject cfg;
        cfg["type"] = "server_config";
        cfg["fps"] = configFps_;
        // [A9] 必须下发**档位名**而不是内部数字：前端下拉框按
        // high/medium/low/verylow 匹配，拿不到就回落显示"中"；而 next 次提交
        // 又把这个不认识的值当档位名回传，服务端匹配不上 → 画质设置被静默丢弃
        // （典型现象：选"极低"后刷新变"中"，再动一下 FPS 画质就永久回中档）。
        cfg["quality"] = qualityName();
        cfg["scale"] = configScale_;
        // 计算机名称：systemName 是原始配置（可能为空 = 跟随主机名），
        // effectiveName 是实际显示值，前端输入框用前者、<title> 用后者。
        cfg["systemName"] = systemName_;
        cfg["effectiveName"] = effectiveSystemName();

        // 获取支持的分辨率列表和当前分辨率
        if (screenCapturer_) {
            // 非服务模式：直接枚举
            cfg["resolutions"] = ScreenCapturer::enumerateSupportedResolutions();
            cfg["currentResolution"] = QString("%1x%2")
                .arg(screenCapturer_->width())
                .arg(screenCapturer_->height());
            // 连接时刷新一次输出列表（热插拔感知），再随配置下发
            screenCapturer_->refreshOutputs();
            cfg["outputs"] = screenCapturer_->enumerateOutputs();
            cfg["currentOutput"] = screenCapturer_->currentOutputIndex();
        } else {
            // 服务模式：分辨率列表从配置文件读取（由 helper 写入）；多屏输出列表
            // 用 helper 上报的快照 —— 服务进程在 Session 0 枚举不到用户会话的显示器，
            // 前端多屏下拉只能靠这份数据，否则永远是空的。
            QFile file(serverConfigDirectory() + "/server_config.json");
            if (file.open(QIODevice::ReadOnly)) {
                QJsonDocument doc = QJsonDocument::fromJson(file.readAll());
                if (doc.isObject()) {
                    QJsonObject root = doc.object();
                    if (root.contains("resolutions"))
                        cfg["resolutions"] = root["resolutions"].toArray();
                    if (root.contains("currentResolution"))
                        cfg["currentResolution"] = root["currentResolution"].toString();
                }
                file.close();
            }
            if (!lastCaptureOutputs_.isEmpty()) {
                cfg["outputs"] = lastCaptureOutputs_;
                cfg["currentOutput"] = lastCaptureOutputIdx_;
            }
        }

        wsServer_->sendJson(clientId, cfg);
    }

    // 发送当前工作模式
    QJsonObject mode;
    mode["type"] = "mode_changed";
    mode["mode"] = (currentMode_ == ServerMode::Video) ? "video" : "image";
    mode["hwEncode"] = hwEncodeAvailable();
    mode["videoSupported"] = videoModeSupported();
#if defined(USE_WEBRTC) && defined(USE_FFMPEG)
    // WebRTC 需要 H.264 编码帧。直接模式/ Linux 服务模式由服务进程自身编码；
    // 服务模式下若 helper 具备视频能力，则由 helper 侧编码并经 /capture 上报。
    bool rawCaptureSupported = (screenCapturer_ && captureAvailable_
            && screenCapturer_->width() > 0 && screenCapturer_->height() > 0)
            || remoteVideoSourceAvailable();
    mode["webrtc"] = rawCaptureSupported;
#else
    mode["webrtc"] = false;
#endif
    wsServer_->sendJson(clientId, mode);

    // 新客户端从 GOP 中间加入：补充 SPS/PPS 配置并强制下一个 IDR，
    // 否则解码器会等下一个关键帧(最长 ~1s)才出画面(黑屏)，或从中间帧解码出花屏。
    if (currentMode_ == ServerMode::Video) {
        if (!lastCodecExtra_.isEmpty()) {
            QJsonObject cfg;
            cfg["type"] = "codec_config";
            cfg["extradata"] = QString::fromLatin1(lastCodecExtra_.toBase64());
            wsServer_->sendJson(clientId, cfg);
        }
        // 立即回放最近关键帧（codec_config 已先下发，MSE 可直接解码），
        // 再强制抓取当前屏幕出一帧新关键帧：解决静态桌面抓取去重导致
        // 新客户端永远等不到关键帧而黑屏的问题。
#ifdef USE_FFMPEG
        // [A2] 带 TTL 回放：只在缓存帧足够新（默认 1.5s）时先铺一张图让客户端
        // 立刻出画面；更旧的缓存意义不大——下面的 pumpKeyframe 会立刻产一帧当前
        // 画面，先铺旧帧等于让用户先看一眼过时内容再重绘，还多花几十 KB。
        replayCachedKeyframe(clientId, 1500);
#endif
        // allowStaticReuse：这里的目标是"让新客户端拿到一帧可解码的 IDR"。桌面静止
        // 且有缓存 IDR 时直接复用（零成本），否则产一帧真 IDR。**不能**走旧的
        // forceNextFrame 路径——那产出的是 P 帧，既不能满足"从 IDR 起解"，还会白烧
        // 一次全屏抓屏 + 编码。
        pumpKeyframe(false, /*allowStaticReuse=*/true);
    }

    // 发送当前锁屏状态（客户端可能在屏幕已锁时连接/重连）。未锁时也必须
    // 显式下发 locked=false：浏览器页面跨服务重启重连后，前端遮罩状态是
    // 上一次会话残留的，若服务端不补发解锁消息，锁屏遮罩将永远无法收起。
    {
        QJsonObject lock;
        lock["type"] = "screen_locked";
        lock["locked"] = screenLocked_;
        // 平台标识：前端据此决定锁屏密码是「逐字注入」还是「交给服务端写安全桌面」。
        // 缺了它前端只能瞎猜（旧实现两种都试不出来，表现为输入密码解不开锁）。
        lock["platform"] = platformTag();
#ifdef _WIN32
        lock["isXP"] = !isVistaPlusOs();
        if (screenLocked_) {
            // 回放 helper 的原始判定（哪种锁屏 + 该给用户哪句话），而不是服务端再猜一次：
            // helper 在用户会话里看得见真实输入桌面，它报的 kind/hint 才是准的。
            if (!lockedKind_.isEmpty())
                lock["kind"] = lockedKind_;
            lock["hint"] = lockedHint_.isEmpty()
                ? QStringLiteral("Windows 锁屏在安全桌面上，点「解锁」由服务端注入密码")
                : lockedHint_;
        }
#endif
        wsServer_->sendJson(clientId, lock);
    }

    // 发送当前帧给新客户端（静态屏幕时 checksum 过滤会跳过广播）
    if (currentMode_ == ServerMode::Image && jpegCompressor_ && !lastCapturedFrame_.isNull()) {
        jpegCompressor_->enqueue(lastCapturedFrame_);
    }
}

bool RDPServer::canInjectLocally() const
{
    if (!inputManager_)
        return false;
#ifdef _WIN32
    // Vista+ 服务进程在 Session 0：本地注入落到服务自己的桌面，用户看不见，
    // 还会污染本侧修饰键记账。XP 的服务与 console 同处 Session 0（同一桌面），
    // 本地注入有效，不能一起禁掉。
    return !serviceMode_ || !isVistaPlusOs();
#else
    // Linux/Android/macOS：没有 Session 0 隔离，服务进程与用户桌面共用同一
    // X 显示/VT（Linux 也没有 helper），XTEST/uinput 注入直接作用到当前桌面。
    return true;
#endif
}

// 释放某客户端所有已按下的键：断线、以及进入观看模式时立即调用。
// 回放 keyup 带全 false 修饰键顺带复位修饰键状态；保留原 keycode 供 Windows VK 注入。
void RDPServer::releasePressedKeys(const QString& clientId)
{
    const QHash<QString, QJsonObject> pressed = pressedKeysByClient_.take(clientId);
    if (pressed.isEmpty())
        return;
    qInfo() << "Releasing" << pressed.size() << "stuck key/button(s) for client" << clientId;
    for (auto it = pressed.constBegin(); it != pressed.constEnd(); ++it) {
        QJsonObject up = it.value();
        const bool isMouse = up.contains(QLatin1String("button"));
        up["type"] = isMouse ? QStringLiteral("mouseup") : QStringLiteral("keyup");
        up["ctrl"] = false;
        up["alt"] = false;
        up["shift"] = false;
        up["meta"] = false;
        const bool viaSecure = screenLocked_ && secureInputRunning_;
        if (serviceMode_) {
            if (wsServer_->isCaptureSourceConnected())
                wsServer_->sendToCaptureSource(up); // helper 消息为扁平结构，与转发路径一致
            else
                // helper 恰好掉线：不能丢，记入待补发队列，重连后按序补发。
                enqueuePendingKeyRelease(clientId, it.key(), up, viaSecure);
        }
        // 锁屏时输入只走 secure-input：helper 的 locked 门会丢弃转发来的 keyup，
        // 若回放不补发 secure-input，winlogon 桌面会被残留的 Shift/Ctrl 卡住。
        if (viaSecure)
            wsServer_->sendToSecureInput(up);
        // 本地注入是否有效由 canInjectLocally() 统一判断（Linux 服务模式没有
        // Session 0 隔离，keyup 必须本地落地，否则客户端断开后键卡住）。服务模式
        // 下若已交给 helper / secure-input，就不能再在本地重复注入。
        const bool forwarded = serviceMode_ && wsServer_->isCaptureSourceConnected();
        if (canInjectLocally() && !forwarded && !viaSecure) {
            if (isMouse)
                inputManager_->injectMouseButton(up["x"].toInt(), up["y"].toInt(),
                                                 up["button"].toInt(), false);
            else
                inputManager_->injectKeyboard(up["keycode"].toInt(), up["code"].toString(), false,
                                              false, false, false, false,
                                              up["isChar"].toBool(), false);
        }
    }
}

void RDPServer::enqueuePendingKeyRelease(const QString& clientId, const QString& trackKey,
                                         const QJsonObject& payload, bool viaSecureInput)
{
    // 只有"keyup 必须经 helper 才能送达"时才入队：Windows 服务模式（Session 0
    // 不能直接注入用户桌面）且 canInjectLocally() 为假。该谓词同时覆盖了
    // Linux 服务模式与 XP 服务模式——它们能本地落地释放（Linux 无 Session 0
    // 隔离、XP 有兼容注入），若仍入队就会在 helper 重连时把已经释放过的键再补发
    // 一遍（幂等但冗余）；非服务模式（直连）也不入队。
    if (!serviceMode_ || trackKey.isEmpty() || canInjectLocally())
        return;
    const QString key = clientId + QLatin1Char('|') + trackKey;
    if (!pendingKeyReleases_.contains(key)) {
        // 上限：异常连接（如狂发 keydown 后掉线）不能把队列撑爆。
        static const int kMaxPendingKeyReleases = 256;
        if (pendingKeyReleases_.size() >= kMaxPendingKeyReleases) {
            qWarning() << "Pending key release queue full, dropping" << key;
            return;
        }
    }
    PendingKeyRelease& entry = pendingKeyReleases_[key];
    entry.payload = payload;
    entry.viaSecureInput = viaSecureInput;
    // 重复入队只更新载荷，不清零 attempts：helper 反复掉线时仍受重试上限约束。
}

void RDPServer::flushPendingKeyReleases()
{
    if (pendingKeyReleases_.isEmpty())
        return;
    if (!serviceMode_ || !wsServer_ || !wsServer_->isCaptureSourceConnected())
        return;
    static const int kMaxAttempts = 8;
    QStringList sentKeys;
    int dropped = 0;
    for (auto it = pendingKeyReleases_.begin(); it != pendingKeyReleases_.end(); ++it) {
        PendingKeyRelease& p = it.value();
        if (p.attempts >= kMaxAttempts) {
            dropped++;
            continue;
        }
        // 补发过程中 helper 再次掉线：保留剩余项，等下次重连。
        if (!wsServer_->isCaptureSourceConnected())
            break;
        wsServer_->sendToCaptureSource(p.payload);
        if (p.viaSecureInput && wsServer_->isSecureInputConnected())
            wsServer_->sendToSecureInput(p.payload);
        p.attempts++;
        sentKeys << it.key();
    }
    for (const QString& key : sentKeys)
        pendingKeyReleases_.remove(key);
    // attempts 达上限的项：下一轮重连时直接清掉，避免永久驻留。
    if (dropped > 0) {
        for (auto it = pendingKeyReleases_.begin(); it != pendingKeyReleases_.end();) {
            if (it.value().attempts >= kMaxAttempts)
                it = pendingKeyReleases_.erase(it);
            else
                ++it;
        }
    }
    qInfo() << "Replayed" << sentKeys.size() << "pending key release(s) after helper reconnect,"
            << "dropped" << dropped;
}

void RDPServer::movePressedKeysToPending()
{
    if (!serviceMode_ || pressedKeysByClient_.isEmpty())
        return;
    // [改1 配套] 能本地注入的平台（XP 服务模式、Linux）不能在这里搬运：本函数把
    // pressedKeysByClient_ 清空后并不做任何本地注入，而 enqueuePendingKeyRelease()
    // 已按 canInjectLocally() 早退，照搬就等于把这些 keyup 直接丢掉——客户端随
    // 后断开时 releasePressedKeys() 已经看不到这些键，它们会一直停在按下状态
    // （真正的卡键），同时日志还会打出一条不实的 "queued N"。
    // 因此把这些键保留在原地：客户端断开时 releasePressedKeys() 会本地注入释放，
    // helper 重连后客户端自己的 keyup 也会正常转发。入队只属于"本地注入够不到
    // 用户桌面"的模式（Vista+ Windows 服务模式）。
    if (canInjectLocally())
        return;
    // 掉线瞬间的锁屏状态决定补发目标：锁屏按键实际注入在安全桌面上。
    const bool viaSecure = screenLocked_ && secureInputRunning_;
    int moved = 0;
    for (auto cit = pressedKeysByClient_.constBegin();
         cit != pressedKeysByClient_.constEnd(); ++cit) {
        for (auto it = cit.value().constBegin(); it != cit.value().constEnd(); ++it) {
            QJsonObject up = it.value();
            const bool isMouse = up.contains(QLatin1String("button"));
            up["type"] = isMouse ? QStringLiteral("mouseup") : QStringLiteral("keyup");
            up["ctrl"] = false;
            up["alt"] = false;
            up["shift"] = false;
            up["meta"] = false;
            enqueuePendingKeyRelease(cit.key(), it.key(), up, viaSecure);
            ++moved;
        }
    }
    pressedKeysByClient_.clear();
    qInfo() << "Helper lost: queued" << moved << "pressed key/button release(s) for replay";
}

void RDPServer::setClientViewOnly(const QString& clientId, bool enabled)
{
    if (enabled) {
        viewOnlyClients_.insert(clientId);
        // 开启瞬间必须释放已按下的键：此后该连接的 keyup 会被入口拦掉，
        // 残留按键会在远端无限自动重复 / 修饰键卡死全部组合键。
        releasePressedKeys(clientId);
        qInfo() << "Client switched to view-only:" << clientId;
    } else {
        viewOnlyClients_.remove(clientId);
        qInfo() << "Client left view-only:" << clientId;
    }
    // 回执：前端以服务端状态为准，重连/异常时静默对齐。
    wsServer_->sendJson(clientId, QJsonObject {
        { "type", "view_only_state" },
        { "enabled", enabled },
    });
}

void RDPServer::setClientChineseInput(const QString& clientId, bool enabled)
{
    // 中文输入（IME 上屏）开关：按连接记录。关闭（默认）时该连接发来的 isChar
    // 在 onInputReceived 入口被静默丢弃 —— 不借键码、不改远端键盘映射、不进
    // sendUnicodeChar 的定时器队列，也不打日志（关闭态打中文属预期行为）。
    if (enabled) {
        chineseInputClients_.insert(clientId);
        qInfo() << "Client enabled Chinese input (Unicode injection):" << clientId;
    } else {
        chineseInputClients_.remove(clientId);
        qInfo() << "Client disabled Chinese input (keycode-only):" << clientId;
    }
    // 回执：前端以服务端状态为准，重连/异常时静默对齐。
    wsServer_->sendJson(clientId, QJsonObject {
        { "type", "chinese_input_state" },
        { "enabled", enabled },
    });
}

void RDPServer::onClientDisconnected(const QString& clientId)
{
    qInfo() << "Client disconnected:" << clientId;

    // 剪贴板读取是异步的：等待期间断线的客户端必须从待推送列表移除，
    // 否则 contentReady 回来后会向已不存在的 clientId 发送（静默失败），
    // 且列表随每次连接/断线累积。
    if (!pendingClipboardClientIds_.isEmpty())
        pendingClipboardClientIds_.removeAll(clientId);

    // 清理该客户端在途的文件上传：上传会话 key 含每连接唯一的 clientId，断线后
    // 该 key 永不再命中 → 不主动清理就会残留裸 QFile 句柄与半个 .part 临时文件。
    // FileTransferService 跑在 transferThread_，所以用队列连接投递过去执行。
    if (fileTransferService_) {
        QMetaObject::invokeMethod(fileTransferService_, "abortUploadsForClient",
                                  Qt::QueuedConnection, Q_ARG(QString, clientId));
    }

    // [B-1] 释放该客户端按下的所有键：断网/关标签页时前端 blur 兜底收不到，
    // 远端按键会永久卡死（无限自动重复 / 修饰键卡住）。回放 keyup 带全 false
    // 修饰键顺带复位修饰键状态；保留原 keycode 供 Windows VK 注入。
    releasePressedKeys(clientId);

    // 观看模式按连接记录，断线即清理（clientId 可能被复用）。
    viewOnlyClients_.remove(clientId);
    // 中文输入开关同样按连接记录，断线即清理（默认即"关闭"）。
    chineseInputClients_.remove(clientId);
    // 队列/映射清理：最后一个开了中文输入的连接断开后，把还没注入的字符作废，
    // 并还原借用的键码映射 —— 否则断开后本机物理键盘按下那颗键会继续出中文。
    // 还有别的中文输入连接在用同一个队列时不能清（那会把别人的输入一起丢掉）。
#if defined(Q_OS_LINUX) && !defined(Q_OS_ANDROID)
    if (chineseInputClients_.isEmpty() && inputManager_)
        inputManager_->flushPendingInput();
#endif
    // [A3] 关键帧请求冷却时刻按连接记录，clientId 可能被复用，断线必须清。
    kfRequestMsByClient_.remove(clientId);


#ifdef USE_WEBRTC
    // 清理该客户端的 WebRTC 会话（并退出 WS 帧广播排除列表）
    if (webrtcSessions_.contains(clientId))
        stopWebRtcSession(clientId);
#endif

    // 没有客户端了 → 暂停屏幕捕获，降低 CPU
    if (wsServer_->clientCount() == 0) {
#if defined(Q_OS_LINUX) && !defined(Q_OS_ANDROID)
        // 最后一个客户端也断了：把逐键 auto-repeat 的"欠账"全部还清。有些 keyup
        // 可能从未送达（浏览器崩溃/网络瞬断），按 clientId 的按键记账只能覆盖
        // "服务端确实收到过 keydown"的那部分。此时没有任何客户端还按着键，全量
        // 恢复是安全的（也不会短暂放开别人按住的键）。
        if (inputManager_)
            inputManager_->restoreAutoRepeatKeycodes();
#endif
        updateClipboardSuspend();   // [A17] 无观众时停掉 xclip 轮询
        updateSleepInhibit(false); // 远程会话结束，解除睡眠抑制
        if (screenCapturer_)
            screenCapturer_->suspend();
        if (serviceMode_ && wsServer_->isCaptureSourceConnected()) {
            QJsonObject msg;
            msg["type"] = "capture_control";
            msg["action"] = "pause";
            wsServer_->sendToCaptureSource(msg);
        }
    }
}

void RDPServer::onInputReceived(const QString& clientId, const QJsonObject& input)
{
    // config 和 set_resolution 需要在 service mode 转发前先本地处理
    QString type = input["type"].toString();

    // ---- 观看模式（只读） ----
    // 开关本身不能受拦截影响，必须放在最前面。
    if (type == QLatin1String("set_view_only")) {
        setClientViewOnly(clientId, input["enabled"].toBool());
        return;
    }
    // 中文输入开关（纯偏好，不是操作被控端）：同样必须放在观看模式拦截之前，
    // 否则只读连接改不了这个开关、重连后状态对不上。
    if (type == QLatin1String("set_chinese_input")) {
        setClientChineseInput(clientId, input["enabled"].toBool());
        return;
    }
    // 权威兜底：观看模式下丢弃一切"能操作被控端"的消息（前端 send() 出口已拦一层，
    // 这里按 clientId 再判一次，防止改过的客户端/其它注入路径绕过）。
    // 纯显示设置（config/set_output/set_resolution/request_keyframe、WebRTC 信令）不拦。
    if (viewOnlyClients_.contains(clientId) && isRemoteControlType(type)) {
        // 静默丢弃会让"按键没反应"无从排查；mousemove 是 120Hz，必须限流。
        static qint64 lastDropLogMs = 0;
        const qint64 nowMs = QDateTime::currentMSecsSinceEpoch();
        if (nowMs - lastDropLogMs >= 5000) {
            lastDropLogMs = nowMs;
            qInfo() << "View-only: dropping remote-control input, type =" << type
                    << "client =" << clientId;
        }
        return;
    }

    // ---- 中文输入（IME 上屏）门禁 ----
    // 只有前端声明过的连接（set_chinese_input:true）才允许 isChar。默认关闭，
    // 老客户端不带该字段 → 同样按关闭处理，行为与旧版一致（纯键码注入）。
    // 关闭态收到 isChar 直接静默丢弃，理由：
    //   · 不借键码、不改远端键盘映射、不进 sendUnicodeChar 的定时器队列 ——
    //     B 组队列"与同一时刻其它 XTest 按键没有严格先后顺序"的风险在关闭态
    //     自然不存在（这条路径根本不会被走到）；
    //   · 不打日志：中文输入关闭时打中文是预期行为，逐条 qWarning 会刷爆日志；
    //   · 不按 keycode 注入：前端关闭态不会发 isChar，这里只做权威兜底，
    //     把码点当键码注入只会产生随机字符。
    // 例外：锁屏密码逐字注入（前端带 lock:true）以及锁屏期间的 isChar，必须放行，
    // 否则开关关闭的用户永远解不开锁。lock 字段比 screenLocked_ 更可靠：Linux 的
    // 锁屏有时由外部程序（xscreensaver 等）呈现，服务端不一定及时探到。
    if ((type == QLatin1String("keydown") || type == QLatin1String("keyup"))
            && input["isChar"].toBool()
            && !input["lock"].toBool()
            && !chineseInputClients_.contains(clientId)
            && !screenLocked_) {
        return;
    }

#ifdef Q_OS_ANDROID
    // Android：无障碍服务未开启时，所有会真正作用到手机的操作都必须明确失败。
    // 静默丢弃只会让用户看到"点了没反应"；这里回执 input_status（限流 2s），
    // 前端据此显示原因和处理路径。
    if ((type == QLatin1String("mousedown") || type == QLatin1String("mouseup")
         || type == QLatin1String("wheel") || type == QLatin1String("global_action"))
            && inputManager_ && !inputManager_->androidInputReady()) {
        static qint64 lastAndroidDenyMs = 0;
        const qint64 nowDenyMs = monoNowMs();
        if (lastAndroidDenyMs == 0 || nowDenyMs - lastAndroidDenyMs >= 2000) {
            lastAndroidDenyMs = nowDenyMs;
            qWarning() << "Android input denied: accessibility service not enabled, type =" << type;
            wsServer_->sendJson(clientId, QJsonObject {
                { "type", "input_status" },
                { "ok", false },
                { "reason", "accessibility_disabled" },
                { "hint", QStringLiteral("远程控制需要在手机系统设置中开启无障碍服务：设置 → 无障碍 → RemoteDesk 输入服务") }
            });
        }
        return;
    }
    // 无障碍全局动作（返回/主页）：与鼠标注入同一条路径，直接执行后返回。
    if (type == QLatin1String("global_action")) {
        if (inputManager_)
            inputManager_->injectGlobalAction(input["action"].toString());
        return;
    }
#endif

    // 长按 repeat：所有平台统一翻译成"先松再按"（见文件开头注释）。
    // 一次 repeat = 一条 keyup + 原 keydown，不依赖任何平台的自动重复：
    // Windows/macOS 的合成注入没有 typematic；X 的 auto-repeat 又是显示级全局状态
    // （可能被遗留的 off 或 `xset r off` 关掉，长按会静默失效）。
    if (type == QLatin1String("keydown") && input["repeat"].toBool()) {
        QJsonObject release = input;
        release["type"] = QLatin1String("keyup");
        release["repeat"] = false;
        // 复用同一入口：观看模式/锁屏/中文开关/按键跟踪/helper 转发全走原有逻辑，
        // 不再复制一份"直连 vs helper"的分发代码（那正是旧版 helper 路径漏翻译的根因）。
        onInputReceived(clientId, release);
        // 随后照常处理原 keydown，形成完整的 up -> down 对。
    }

    // [B-1] 记录本客户端按下的键（code 为唯一标识，存最近 keydown 完整载荷，
    // Windows 端 keyup 需要原 keycode 作 VK）。isChar 的 Unicode 注入键也可能
    // 卡住，一并跟踪。所有模式（直连/转发 helper）统一在此记录，断开时回放 keyup。
    // 鼠标按钮同理由一并跟踪：前端 releaseHeldRemoteInput 只在失焦/切观看模式时补发，
    // 浏览器崩溃或网络掉线时没人补发，远端会一直保持按下状态（拖拽选中、按住 Ctrl 的
    // 表现和按键卡死一样让人困惑）。
    const bool isMouseEvt = type == QLatin1String("mousedown") || type == QLatin1String("mouseup");
    if (type == QLatin1String("keydown") || type == QLatin1String("keyup") || isMouseEvt) {
        const QString code = input["code"].toString();
        // isChar 事件前端固定发 code:''（index.html 的输入法/锁屏密码路径），
        // 直接用 code 当跟踪键会全部落空，断线时这些真正按下过的键不会被释放。
        // Linux 端 isChar 会映射成真实 keysym 注入，卡住即无限自动重复。
        const int keycode = input["keycode"].toInt();
        const bool isChar = input["isChar"].toBool();
        QString trackKey;
        if (isMouseEvt)
            trackKey = QStringLiteral("btn:%1").arg(input["button"].toInt(-1));
        else
            trackKey = !code.isEmpty()
                ? code
                : (isChar && keycode > 0 ? QStringLiteral("char:%1").arg(keycode) : QString());
        const bool down = isMouseEvt ? (type == QLatin1String("mousedown"))
                                     : (type == QLatin1String("keydown"));
        if (!trackKey.isEmpty() && trackKey != QLatin1String("btn:-1")) {
            if (down) {
                pressedKeysByClient_[clientId][trackKey] = input;
            } else {
                auto it = pressedKeysByClient_.find(clientId);
                if (it != pressedKeysByClient_.end()) {
                    it->remove(trackKey);
                    if (it->isEmpty())
                        pressedKeysByClient_.erase(it);
                }
            }
        }
    }

#ifdef USE_WEBRTC
    // WebRTC 信令（signal_*）：需要 H.264 编码帧来源。直接模式 / Linux 服务模式由
    // 服务进程自身编码；服务模式下若 helper 具备视频能力，则由 helper 侧编码并上报。
    // 两者皆无（如 helper 未连）时拒绝，前端据 signal_state=unsupported 降级。
    if (type.startsWith("signal_")) {
        bool rawCaptureSupported = (screenCapturer_ && captureAvailable_
                && screenCapturer_->width() > 0 && screenCapturer_->height() > 0)
                || remoteVideoSourceAvailable();
        if (serviceMode_ && !rawCaptureSupported) {
            wsServer_->sendJson(clientId, QJsonObject {
                { "type", "signal_state" },
                { "state", "unsupported" },
            });
        } else {
            onWebRtcMessage(clientId, input);
        }
        return;
    }
#endif

    if (type == "clipboard") {
        // 内容类型：文本（text，兼容旧协议）或图片（mime:"image/png" + data:base64）
        const bool isImage = input.contains("mime") && input["mime"].toString() == "image/png";
        const QString mime = isImage ? QStringLiteral("image/png") : QStringLiteral("text/plain");
        QByteArray clipData;
        if (isImage) {
            clipData = QByteArray::fromBase64(input["data"].toString().toLatin1());
        } else {
            clipData = input["text"].toString().toUtf8();
        }
        // 单条剪贴板消息上限：广播是同步写给所有客户端，且服务端还要转发/落剪贴板。
        // 没有上限时一个恶意的超大 base64/文本帧会让广播路径长时间占用主线程。
        // 正常剪贴板（文本或截图 PNG）远小于 8MB。
        static const int kMaxClipboardBytes = 8 * 1024 * 1024;
        if (clipData.size() > kMaxClipboardBytes) {
            qWarning() << "Clipboard payload too large, ignored:" << clipData.size();
            return;
        }
        if (serviceMode_ && wsServer_->isCaptureSourceConnected()) {
            // 服务模式：剪贴板在用户会话的 helper 进程中读写，转发给 helper；
            // 同时广播给所有客户端（含发送者），让多端剪贴板保持一致，发送者前端按内容去重
            wsServer_->sendToCaptureSource(clipboardPayload(mime, clipData));
            wsServer_->broadcastJson(clipboardPayload(mime, clipData));
            return;
        }
        // 直接模式：浏览器粘贴/同步 → 写入远端系统剪贴板，并在远端触发一次 Ctrl+V 粘贴。
        // 写入是异步的（CLI 模式走工作线程），成功后由 contentApplied 处理器广播 + 注入按键。
        if (clipboardService_) {
            clipboardService_->requestSetContent(mime, clipData);
        }
        return;
    }

    if (type == "system_action") {
        // 系统操作（快捷键面板）：由服务端直接执行，不转发给 helper。
        // password 只有「解锁屏幕」用得上：Windows 侧必须交给 secure-input 去写
        // 安全桌面上的登录框（浏览器逐字发 keydown 到不了 winlogon 桌面）。
        handleSystemAction(input["action"].toString(), clientId,
                           input["password"].toString(),
                           input["username"].toString());
        return;
    }

    if (type == "ime_cycle") {
        // 输入法切换：ibus hotkey grab 对 XTEST 合成事件不响应（实测 Super+space /
        // Ctrl+Alt+space 注入均无法触发 panel），故由服务端直接切 ibus engine
        handleImeCycle(clientId);
        return;
    }

    if (type == "request_keyframe") {
        // 前端 MSE 自愈重启 / WebCodecs 解码重建后会请求一帧全新关键帧，
        // 以便从 IDR 重新同步参考帧。必须立即回放最近缓存的关键帧（让客户端
        // 立刻有可解码的 IDR），并强制抓取当前屏幕出一帧新关键帧——否则静态
        // 桌面下抓取去重（XDamage/checksum）会一直抑制出帧，requestKeyframe
        // 标志永远等不到原始帧而失效，客户端只收到 P 帧无法解码，画面黑屏。
#ifdef USE_FFMPEG
        // 先回放缓存 IDR（客户端立刻有可解起点），但仅对新加入的客户端足够；
        // 若客户端已丢弃此前的帧（等帧门被清空），缓存帧可能被其按"旧序列"
        // 忽略，故下方仍要产一帧全新 IDR。回放时间戳由 replayCachedKeyframe
        // 抬到"上一帧之后"，不再沿用缓存帧的旧 ts（否则时间线倒退）。
        //
        // 服务模式**不在本地回放**：编码在 helper 侧，"桌面变没变"和最新的码流都在
        // 那边，转过去的 request_keyframe 会由 helper 自己重发缓存 IDR 或重建。
        // 两边都回放会让每个请求多收一帧重复 IDR（实测 PLI 1Hz 时"每请求 2 帧、
        // 20 秒 2.0MB"，而 p 帧只有 4 个）。
        if (!remoteVideoSourceAvailable())
            replayCachedKeyframe(clientId, 0);
#endif
        // requestNewIdr=true：强制产**新** IDR。仅靠回放缓存帧救不了已重置等帧门
        // 的客户端（它需要"新"关键帧才放行后续 P 帧），也救不了静态桌面下
        // 编码器已 hasIdr_=true 后不再出 IDR 的情况（ffmpeg/MSE 冻结的主因）。
        //
        // [A3] 但每帧回调里都无条件走一次"请求新 IDR"会被前端重试放大：缓冲溢出 /
        // 配额满时前端每 500ms 发一轮，服务端直接转发、helper 收到就重建编码器。
        // 这里按客户端加冷却：冷却内的重复请求不再索要新 IDR（上面的缓存回放
        // 已经让该客户端拿到一帧可解码的 IDR），把重建频率钉在 ≤1 次/700ms。
        {
            constexpr qint64 kClientKfCooldownMs = 700;
            const qint64 nowKf = monoNowMs();
            const qint64 lastKf = kfRequestMsByClient_.value(clientId, 0);
            if (lastKf > 0 && nowKf - lastKf < kClientKfCooldownMs) {
                if ((++kfCooldownSkips_ % 20) == 1)
                    qInfo() << "[KF-COOLDOWN] suppressed client keyframe request"
                            << "client=" << clientId << "since=" << (nowKf - lastKf) << "ms"
                            << "skipped=" << kfCooldownSkips_;
                return;
            }
            kfRequestMsByClient_[clientId] = nowKf;
        }
        pumpKeyframe(true);
        return;
    }

    if (serviceMode_) {
        if (wsServer_->isCaptureSourceConnected()) {
            if (isInputEventLogged(type))
                qDebug() << "Service: forwarding input to helper, type =" << type;
            
            // [B17] config 也要转发：服务模式下 JPEG 质量/缩放/帧率都由 helper
            // 侧执行，本地 jpegCompressor_ 不在视频路径上，旧代码跳过 config
            // → 画质/帧率热更新静默失效。helper 对未知类型安全忽略。
            // [A15] 视频参数变化 ⇒ helper 会按新参数重建编码器（新尺寸/新 SPS），
            // 本进程缓存的 IDR/SPS 立刻过期。不清的话，此窗口内新接入的浏览器
            // 会先收到一条参数集不匹配的旧码流 → 花屏，直到 helper 的新 IDR 到达。
            // 只带 systemName 的 config 不涉及编码器，不能误清。
            // set_output 同样会换捕获尺寸（多屏切换），缓存 IDR 一起作废。
            if (type == "set_resolution" || type == "set_output"
                    || (type == "config" && (input.contains("quality") || input.contains("fps")
                                             || input.contains("scale") || input.contains("codec")
                                             || input.contains("hw_encode")))) {
                invalidateCachedIdr();
            }
            wsServer_->sendToCaptureSource(input);
            if ((screenLocked_ || loginScreenActive_) && secureInputRunning_)
                wsServer_->sendToSecureInput(input);
            // config/set_resolution 跳过转发，由下面的本地逻辑处理
        } else {
            // 直接处理分支：鼠标/键盘等高频事件不写日志（避免同步文件 I/O 占用主线程 CPU）
            if (isInputEventLogged(type))
                qDebug() << "Service: no helper, handling input directly, type =" << type;
            // [Win-登录] 停在登录界面（无用户会话）时，键盘/鼠标必须进 secure-input
            // （winsta0\Winlogon）；本地 inputManager 只在 session 0 自己的桌面生效。
            const bool isUserInputEvent = (type == "mousemove" || type == "mousedown"
                                           || type == "mouseup" || type == "keydown"
                                           || type == "keyup" || type == "wheel");
            if (loginScreenActive_ && secureInputRunning_ && isUserInputEvent) {
                wsServer_->sendToSecureInput(input);
                return;
            }
            // 没有 helper（如 Linux 无头模式），直接在本地注入输入。
            // 与断连回放共用同一个能力判断：Windows Vista+ Session 0 本地注入无效，
            // Linux 服务模式必须能注入（否则键盘/鼠标整体失效）。
            if (!canInjectLocally())
                return;
            if (type == "mousemove") {
                int x = input["x"].toInt();
                int y = input["y"].toInt();
                inputManager_->injectMouseMove(x, y);
                if (screenCapturer_) screenCapturer_->forceNextFrame();
            } else if (type == "mousedown" || type == "mouseup") {
                int x = input["x"].toInt();
                int y = input["y"].toInt();
                int button = input["button"].toInt();
                bool isDown = (type == "mousedown");
                inputManager_->injectMouseButton(x, y, button, isDown);
                if (screenCapturer_) screenCapturer_->forceNextFrame();
            } else if (type == "keydown" || type == "keyup") {
                int keycode = input["keycode"].toInt();
                QString code = input["code"].toString();
                bool isDown = (type == "keydown");
                bool ctrl = input["ctrl"].toBool();
                bool alt = input["alt"].toBool();
                bool shift = input["shift"].toBool();
                bool isChar = input["isChar"].toBool();
                bool meta = input["meta"].toBool();
                // 按键逐条打日志会刷爆日志文件，不再记录（需要排查时临时加回）
                // 中文输入开关打开时，该连接的**所有**键都走 InputManager 的同一条
                // FIFO（含 ASCII/Enter）：否则同步注入的 ASCII 会超车排在队列里的
                // 中文字符（"中a" 落成 "a中"）。关闭态（默认）直接同步注入，零延迟。
#if defined(Q_OS_LINUX) && !defined(Q_OS_ANDROID)
                if (chineseInputClients_.contains(clientId)) {
                    inputManager_->enqueueKeyboard(keycode, code, isDown, ctrl, alt,
                                                   shift, isChar, meta);
                } else
#endif
                {
                    inputManager_->injectKeyboard(keycode, code, isDown, ctrl, alt,
                                                  shift, false, isChar, meta);
                }
                if (screenCapturer_) screenCapturer_->forceNextFrame();
            } else if (type == "wheel") {
                int delta = input["delta"].toInt();
                inputManager_->injectWheel(delta);
                if (screenCapturer_) screenCapturer_->forceNextFrame();
            }
        }
    }

    if (screenLocked_ && !serviceMode_) {
#ifdef _WIN32
        if (input["isChar"].toBool() && input["keycode"].toInt() > 0) {
            wchar_t ch = static_cast<wchar_t>(input["keycode"].toInt());
            bool isDown = (type == "keydown");
            INPUT in = {};
            in.type = INPUT_KEYBOARD;
            in.ki.dwFlags = KEYEVENTF_UNICODE;
            in.ki.wScan = ch;
            if (!isDown)
                in.ki.dwFlags |= KEYEVENTF_KEYUP;
            SendInput(1, &in, sizeof(INPUT));
            return;
        }
        if ((type == "keydown" || type == "keyup") && input["keycode"].toInt() == 13) {
            INPUT in = {};
            in.type = INPUT_KEYBOARD;
            in.ki.wVk = VK_RETURN;
            if (type == "keyup")
                in.ki.dwFlags = KEYEVENTF_KEYUP;
            SendInput(1, &in, sizeof(INPUT));
            return;
        }
        // 其他键盘事件在锁屏时忽略，但鼠标/滚轮落到正常输入路径
        if (type == "keydown" || type == "keyup")
            return;
#else
        // 非 Windows（Linux）锁屏时无需额外处理：输入已在上方 helper/直连路径投递。
        // 这里不再打日志——该分支对每个输入事件都会走到（含前端 60Hz mousemove），
        // 逐条 qInfo 会刷爆日志文件并造成主线程 IO。
        (void)input;
#endif
    }

    // 屏保活跃时先发 ESC 退出，再投递用户实际输入（service 模式下 helper 处理）
#ifndef _WIN32
    (void)input;
#else
    if (!serviceMode_) {
        // [P6] SystemParametersInfo 是每输入事件一次的同步系统调用（含 60Hz
        // mousemove），主线程白白消耗；屏保状态变化是秒级事件，500ms 节流足够。
        static DWORD lastSpiCheckMs = 0;
        const DWORD nowMs = ::GetTickCount();
        if (nowMs - lastSpiCheckMs >= 500) {
            lastSpiCheckMs = nowMs;
            BOOL ssRunning = FALSE;
            SystemParametersInfo(0x0072, 0, &ssRunning, 0);
            if (ssRunning) {
                INPUT esc[2] = {};
                esc[0].type = INPUT_KEYBOARD; esc[0].ki.wVk = VK_ESCAPE;
                esc[1].type = INPUT_KEYBOARD; esc[1].ki.wVk = VK_ESCAPE; esc[1].ki.dwFlags = KEYEVENTF_KEYUP;
                SendInput(2, esc, sizeof(INPUT));
            }
        }
    }
#endif

    if (!serviceMode_) {
        if (type == "mousemove") {
            int x = input["x"].toInt();
            int y = input["y"].toInt();
            inputManager_->injectMouseMove(x, y);
        } else if (type == "mousedown" || type == "mouseup") {
            int x = input["x"].toInt();
            int y = input["y"].toInt();
            int button = input["button"].toInt();
            bool isDown = (type == "mousedown");
            inputManager_->injectMouseButton(x, y, button, isDown);
        } else if (type == "keydown" || type == "keyup") {
            int keycode = input["keycode"].toInt();
            QString code = input["code"].toString();
            bool isDown = (type == "keydown");
            bool ctrl = input["ctrl"].toBool();
            bool alt = input["alt"].toBool();
            bool shift = input["shift"].toBool();
            bool isChar = input["isChar"].toBool();
            bool meta = input["meta"].toBool();
#if defined(Q_OS_LINUX) && !defined(Q_OS_ANDROID)
            // 与直连路径同一规则：中文输入打开时走 FIFO，保证锁屏密码这类
            // "字符与键码混排"的输入不会互相超车。
            if (chineseInputClients_.contains(clientId) || isChar)
                inputManager_->enqueueKeyboard(keycode, code, isDown, ctrl, alt,
                                               shift, isChar, meta);
            else
                inputManager_->injectKeyboard(keycode, code, isDown, ctrl, alt,
                                              shift, false, isChar, meta);
#else
            inputManager_->injectKeyboard(keycode, code, isDown, ctrl, alt, shift, false, isChar, meta);
#endif
        } else if (type == "wheel") {
            int delta = input["delta"].toInt();
            inputManager_->injectWheel(delta);
        }
    }
    if (type == "config") {
        // 计算机名称（页面标题）：与画质/帧率互不相干，也不触发编码器重建。
        // 只在真的变了才落盘 + 广播，避免一条 config 消息里顺带的空字段把
        // 别人的自定义名字刷掉。
        if (input.contains("systemName")) {
            // 去掉首尾空白并截断：名字只用于 <title>，超长没有意义，
            // 但也不能让一条消息把配置写成几 KB。
            const QString name = input.value("systemName").toString().trimmed().left(64);
            if (name != systemName_) {
                systemName_ = name;
                scheduleSaveConfig();
                broadcastSystemName();   // 所有已打开的标签页一起改标题
                { QMutexLocker locker(&htmlCacheMutex_); htmlCache_.clear(); }  // 标题烤进了 HTML，下次 GET / 重新生成
                qInfo() << "System name changed to"
                        << (name.isEmpty() ? QStringLiteral("(default: hostname)") : name);
            }
        }
        // 画质/帧率/缩放配置
        // 一条 config 消息常同时带 quality/fps/scale/codec/hw_encode。旧实现每命中一项
        // 就立即重建一次编码器（每次几十到上百毫秒，且要重发 IDR），一次切换画质会重建
        // 3~5 次 → 明显卡顿。这里统一收集：帧率/尺寸/协议/硬编变化最多重建一次；
        // 仅画质变化时直接改码率，不重建。
        bool needEncoderRebuild = false;
        bool needBitrateRefresh = false;
        const int scaleBeforeQuality = configScale_;
        QString qname = input["quality"].toString();
        int jpegQ = -1;
        if (qname == "high") {
            jpegQ = 80;
            configScaleWish_ = 100;
            configScale_ = cappedScale(configScaleWish_);
        } else if (qname == "medium") {
            jpegQ = 60;
            configScaleWish_ = userScale_;
            configScale_ = cappedScale(configScaleWish_);
        } else if (qname == "low") {
            jpegQ = 35;
            configScaleWish_ = userScale_;
            configScale_ = cappedScale(configScaleWish_);
        } else if (qname == "verylow") {
            // 极低：在 low 基础上进一步大幅降码率，并强制降分辨率（封顶 50%），
            // 让静止桌面也肉眼可见地变模糊。原因：openh264 在静止内容上会大量
            // 跳 P 帧（仅周期传清晰 I 帧），仅靠降码率（low=0.08）在静态 UI 上几乎
            // 看不出差异，必须叠加降分辨率才能体现“极低”。
            jpegQ = 20;
            configScaleWish_ = qMin(userScale_, 50);
            configScale_ = cappedScale(configScaleWish_);
        }
        // 视频码率档位：默认与 JPEG 质量一致；verylow 单独用更低档位（videoBitrateFor
        // 的 case 15 = 0.035 bpp），使“极低”相较“低”有额外可见降级，而非等价。
        int videoQualityLevel = jpegQ;
        if (qname == "verylow")
            videoQualityLevel = 15;
        if (jpegQ > 0) {
            // configQuality_ 与落盘不能依赖 jpegCompressor_ 存在：服务模式下
            // 压缩在 helper 侧，本进程可能没有压缩器，旧写法会让画质设置
            // 永远不更新也不持久化（重启后仍旧值）。
            configQuality_ = videoQualityLevel;
            scheduleSaveConfig();
            if (jpegCompressor_) {
                jpegCompressor_->setQuality(jpegQ);
                jpegCompressor_->setScalePercent(configScale_);
#ifdef USE_FFMPEG
                // 画质档位决定目标码率，但编码器本身（协议/尺寸/帧率）没变，
                // 只需动态改码率，不走重建（重建会丢 IDR、卡顿）。
                // 例外：verylow/high 会联动改 configScale_（编码分辨率变化），
                // 仅刷码率会让编码器停在旧分辨率 → 必须走重建。
                if (currentMode_ == ServerMode::Video && videoEncoder_) {
                    if (configScale_ != scaleBeforeQuality)
                        needEncoderRebuild = true;
                    else
                        needBitrateRefresh = true;
                }
#endif
                // 画面静止时帧校验和去重会跳过重编码/广播，导致切换画质后
                // 仍用旧质量显示当前画面（看起来“没效果”）。强制下一帧通过去重，
                // 让当前画面立即按新质量/新缩放重新压缩并发送。
                if (screenCapturer_)
                    screenCapturer_->forceNextFrame();
            }
        }
        int newFps = input["fps"].toInt();
        // [H22] 配置侧同步钳制（configFps_ 也参与编码器 GOP/间隔计算）
        if (newFps > 60) newFps = 60;
        // 慢机自适应上限：机器只能跑 10fps 时，用户设 15 也不必按 15 重建编码器
        // （那是白付一次 shutdown/initialize + 丢 IDR）。
        if (newFps >= 1) {
            // 用户意图先原样记住（落盘写 userFps_），生效值再压自适应上限：
            // 旧写法把压过的值当唯一真相，降级后用户再设 15 也只是"15→10"，
            // 连切回小分辨率都恢复不了。
            userFps_ = newFps;
            scheduleSaveConfig(); // [P2] 去抖落盘（生效值没变时也要记住这次意图）
            newFps = cappedFps(newFps);
        }
        // 只在帧率**真的变化**时才处理：前端切画质也顺手带上了当前 fps
        // （index.html 的 quality 分支），无条件重建会让"只改画质"变成
        // shutdown/initialize + 丢 IDR + 客户端等关键帧，画面卡一下。
        if (newFps >= 1 && newFps != configFps_) {
            configFps_ = newFps;
            if (screenCapturer_)
                screenCapturer_->setFps(newFps);
            scheduleSaveConfig(); // [P2] 去抖落盘
#ifdef USE_FFMPEG
            // 帧率改变需按新 fps 重建编码器，否则 time_base/gop_size 仍是旧值，
            // 导致 IDR 间隔与捕获帧率不匹配
            if (currentMode_ == ServerMode::Video && videoEncoder_)
                needEncoderRebuild = true;
#endif
        }
        int newScale = input["scale"].toInt();
        if (newScale >= 10 && newScale <= 100) {
            // 用户意图原样记住（落盘写 userScale_）；实际生效值再压自适应上限，
            // 否则慢机上用户随手选个 100% 就把降档结果冲掉。
            userScale_ = newScale;
            configScaleWish_ = newScale;
            const int appliedScale = cappedScale(newScale);
            // 仅当生效缩放（configScale_）真实变化才重建：high/verylow 档位可能已把
            // configScale_ 设成相同值，重复重建会丢 IDR 造成画面卡顿。
            const bool scaleChanged = (configScale_ != appliedScale);
            configScale_ = appliedScale;
            if (jpegCompressor_)
                jpegCompressor_->setScalePercent(appliedScale);
            scheduleSaveConfig(); // [P2] 去抖落盘
#ifdef USE_FFMPEG
            // 视频模式下缩放档位改变需按新尺寸重建编码器，
            // 否则始终全分辨率 H.264 软编（CPU 高且 scale 无效）
            if (scaleChanged && currentMode_ == ServerMode::Video && videoEncoder_)
                needEncoderRebuild = true;
#endif
        }
        // 编码协议（h264/hevc/vp8/vp9/av1）与硬件编码开关（auto/on/off）：
        // 视频/FFmpeg 模式下重建编码器即时生效；WebRTC 会话激活期间忽略（会话按
        // 已协商的编码类型推流，中途换码流对端解不出来）
#ifdef USE_WEBRTC
        bool webrtcBusy = !webrtcSessions_.isEmpty();
#else
        bool webrtcBusy = false;
#endif
        QString codecStr = input["codec"].toString();
        if (!codecStr.isEmpty()) {
            CodecType c = codecFromString(codecStr);
            if (c != configCodec_) {
                if (webrtcBusy) {
                    // 会话已按协商好的编码类型推流，编码器不能中途换；但用户这次选择
                    // 仍然有效：记到"用户原值"里供落盘和离开视频模式后恢复，不能把
                    // 当前会话临时钉住的编码当成用户配置持久化（否则用一次 WebRTC
                    // 就永久丢选择）。
                    qWarning() << "Ignoring codec switch while WebRTC active (session keeps its"
                               << "negotiated codec), remembered for later:" << codecToString(c);
                    codecBeforeWebRtc_ = c;
                    codecOverriddenForWebRtc_ = true;
                    scheduleSaveConfig(); // [P2] 去抖落盘
                } else {
                    configCodec_ = c;
                    codecOverriddenForWebRtc_ = false;
                    scheduleSaveConfig(); // [P2] 去抖落盘
#ifdef USE_FFMPEG
                    if (currentMode_ == ServerMode::Video && videoEncoder_)
                        needEncoderRebuild = true;
#endif
                }
            }
        }
        QString hwModeStr = input["hw_encode"].toString();
        if (!hwModeStr.isEmpty()) {
            HwEncodeMode m = HwEncodeMode::Auto;
            if (hwModeStr == "on") m = HwEncodeMode::On;
            else if (hwModeStr == "off") m = HwEncodeMode::Off;
            if (m != configHwEncodeMode_) {
                configHwEncodeMode_ = m;
                scheduleSaveConfig(); // [P2] 去抖落盘
#ifdef USE_FFMPEG
                if (currentMode_ == ServerMode::Video && videoEncoder_ && !webrtcBusy) {
                    needEncoderRebuild = true;
                } else if (webrtcBusy) {
                    qWarning() << "Ignoring hw_encode switch while WebRTC active";
                }
#endif
            }
        }
#ifdef USE_FFMPEG
        // 统一收尾：先重建（重建内部会按新尺寸/帧率重算基准码率），
        // 否则仅刷新码率，避免一次配置触发多次重建。
        if (needEncoderRebuild) {
            reinitVideoEncoderForScale();
        } else if (needBitrateRefresh && videoEncoder_ && screenCapturer_) {
            int sw = screenCapturer_->width();
            int sh = screenCapturer_->height();
            int encW = sw, encH = sh;
            if (configScale_ < 100) {
                const int tw = encW * configScale_ / 100;
                const int th = encH * configScale_ / 100;
                if (tw > 0 && th > 0) { encW = tw; encH = th; }
            }
            videoBaseBitrate_ = videoBitrateFor(encW, encH, configFps_, configCodec_);
            if (videoBaseBitrate_ > 0)
                videoEncoder_->setBitrate(videoBaseBitrate_);
        }
#endif
    } else if (type == "file_list") {
        emit requestFileList(clientId, input["path"].toString());
    } else if (type == "file_download") {
        emit requestDownload(clientId, input["path"].toString());
    } else if (type == "file_upload_start") {
        emit requestUploadStart(clientId, input["path"].toString(),
            input["size"].toVariant().toLongLong());
    } else if (type == "file_upload_done") {
        emit requestUploadDone(clientId, input["path"].toString());
    } else if (type == "set_resolution" && !serviceMode_) {
        // 与 HTTP /api/config 共用同一实现：失败时回 error，成功时广播实际尺寸。
        applyResolutionChange(input["width"].toInt(), input["height"].toInt(), clientId);
    } else if (type == "set_output") {
        // 多屏切换（Linux X11）：切换捕获目标输出
        int index = input["index"].toInt();
        if (screenCapturer_ && screenCapturer_->refreshOutputs()
                && screenCapturer_->switchOutput(index)) {
            // 切换输出后捕获区尺寸与根窗口原点都变了：输入归一化基准和偏移
            // 必须同步，否则点击会按旧输出/旧偏移落点（多屏错位的主因之一）。
            if (inputManager_) {
                inputManager_->setScreenSize(screenCapturer_->width(), screenCapturer_->height());
                inputManager_->setScreenOffset(screenCapturer_->originX(), screenCapturer_->originY());
            }
            QJsonObject info;
            info["type"] = "screen_info";
            info["width"] = screenCapturer_->width();
            info["height"] = screenCapturer_->height();
            info["platform"] = platformTag();
            wsServer_->broadcastJson(info);

            QJsonObject cfg;
            cfg["type"] = "server_config";
            cfg["outputs"] = screenCapturer_->enumerateOutputs();
            cfg["currentOutput"] = screenCapturer_->currentOutputIndex();
            wsServer_->broadcastJson(cfg);

#ifdef USE_FFMPEG
            // 输出切换可能改变捕获尺寸，同样需要重建编码器并作废关键帧缓存
            lastKeyframeData_.clear();
            lastKeyframeTs_ = 0;
            reinitVideoEncoderForScale();
#endif
            qInfo() << "Switched capture output to" << info["width"].toInt()
                    << "x" << info["height"].toInt();
        }
    }
}

void RDPServer::injectPasteShortcut()
{
    // 远端剪贴板已写入浏览器发来的文本后，注入一次 Ctrl+V 让聚焦的远端应用完成粘贴
    QJsonObject down {
        { "type", "keydown" },
        { "keycode", 86 },      // V
        { "code", "KeyV" },
        { "ctrl", true },
        { "alt", false },
        { "shift", false },
        { "meta", false },
        { "isChar", false },
    };
    QJsonObject up {
        { "type", "keyup" },
        { "keycode", 86 },
        { "code", "KeyV" },
        // 显式带 false：helper 依据这些标志维护修饰键状态，
        // 缺省不传会让 Ctrl 在远端保持按下（解锁后键盘卡住/被污染）。
        { "ctrl", false },
        { "alt", false },
        { "shift", false },
        { "meta", false },
    };

    if (serviceMode_ && wsServer_->isCaptureSourceConnected()) {
        // 服务模式（有 helper，如 Windows）：由 helper 进程注入
        wsServer_->sendToCaptureSource(down);
        wsServer_->sendToCaptureSource(up);
        // 锁屏态必须成对发送 down/up：旧实现只发 down，V 与 Ctrl 在安全输入
        // helper 里永远保持按下 → 解锁后键盘卡住（后续输入全被修饰键污染）。
        if (screenLocked_ && secureInputRunning_) {
            wsServer_->sendToSecureInput(down);
            wsServer_->sendToSecureInput(up);
        }
    } else if (inputManager_) {
#if defined(Q_OS_LINUX) && !defined(Q_OS_ANDROID)
        // Linux 服务模式无 helper：本进程注入（inputManager_ 在 startCapture 时已按
        // detectUserX11Env 设置的 DISPLAY 重建，XTest 可用）
#else
        // 其他平台服务模式由 helper 在用户会话注入；服务进程（Session 0）SendInput
        // 无法作用于交互桌面，直接注入无效
        if (serviceMode_)
            return;
#endif
        inputManager_->injectKeyboard(86, "KeyV", true, true, false, false, false, false);
        inputManager_->injectKeyboard(86, "KeyV", false, true, false, false, false, false);
        inputManager_->updateModifiers(false, false, false); // 释放 Ctrl
    }
}

void RDPServer::onFrameCaptured(const QImage& frame)
{
    // [缓存顺序] 先投递、后缓存。缓存（lastCapturedFrame_）是给"下一个接入的客户端"
    // 用的，但它会让本帧 QImage 的引用计数变 2，于是下游 enqueue()/encode() 里
    // `frame.isDetached() ? frame : frame.copy()` 的快路径失效 → 每帧多拷一整帧
    //（2117x1398 ≈ 11.8MB）。改为投递完再持有，同一帧在入队时仍是独占的。
    //
    // 仅图片模式需要缓存首帧（onClientConnected 用它给新客户端补图）。视频模式下
    // 无条件持有会让 QImage 引用计数始终 >1 → VideoEncoder::encode 的 isDetached()
    // 快路径永远失败 → 每帧多一次整帧深拷贝（1080p@30 ≈ 240MB/s）。
    const bool needCache = (currentMode_ == ServerMode::Image);

    if (wsServer_->clientCount() > 0) {
        bool dispatched = false;
#ifdef USE_FFMPEG
        if (currentMode_ == ServerMode::Video && videoEncoder_) {
            // 编码器/sws 上下文的源尺寸与当前捕获尺寸不一致（分辨率切换、输出切换、
            // 显示器热插拔后未重建）→ 节流重建，避免 sws_scale 越界读。
            if (videoEncoder_->sizeMismatch()) {
                const qint64 nowMs = monoNowMs();
                if (nowMs - lastSizeReinitMs_ > 3000) {
                    lastSizeReinitMs_ = nowMs;
                    qInfo() << "Capture size changed, reinitializing video encoder";
                    reinitVideoEncoderForScale();
                }
            } else {
                // 缩放已在编码线程 sws_scale 完成，主线程不再做全帧缩放（避免阻塞输入处理）
                // X11 双缓冲采集：帧在编码读完前不会被改写，可零拷贝入队。
                videoEncoder_->setAssumeExclusiveInput(
                    screenCapturer_ && screenCapturer_->framesAreExclusive());
                videoEncoder_->encode(frame);
            }
            dispatched = true;
        }
#endif
        if (!dispatched) {
            // 图片模式：缩放移到 JPEG 压缩线程，避免全帧缩放占用主线程阻塞输入处理。
            // （视频模式但编码器缺失时也落到这里，与改造前一致；onJpegCompressed
            //  另有模式门，不会把 JPEG 混进视频流。）
            if (jpegCompressor_)
                jpegCompressor_->enqueue(frame);
        }
    }

    if (needCache)
        lastCapturedFrame_ = frame;
    else if (!lastCapturedFrame_.isNull())
        lastCapturedFrame_ = QImage();
}

void RDPServer::broadcastCursorPos()
{
    // [流畅度] 光标位置广播与视频帧解耦：原逻辑挂在 onFrameCaptured 里，
    // 静止桌面（Damage 为空）时完全不上报 → 前端光标 overlay 卡死在旧位置；
    // 运动时上报率 = 帧率且被整条抓屏路径延迟拖累。独立 33Hz 定时器后
    // 光标 overlay 只滞后注入 ~30ms（+前端开环预测见 index.html）。
    if (!wsServer_ || wsServer_->clientCount() == 0)
        return;

    // 服务模式 + helper 已连：光标位置由 helper（winlogon 会话内真实位置）经
    // /capture 通道上报，本进程 QCursor/XQueryPointer 再广播会形成双源互相拉扯。
    if (serviceMode_ && wsServer_->isCaptureSourceConnected())
        return;

    // Linux 服务模式为 QCoreApplication，QCursor::pos() 恒为 (0,0)；
    // 由 InputManager 用 XQueryPointer 提供真实光标位置。
    // cursorPosition() 各平台统一返回"捕获区相对坐标"（InputManager 内已减去
    // 捕获区原点）。这里不能再减一次 origin：副屏捕获时会双重偏移；且负值在
    // 多屏布局（副屏位于主屏左/上方）下是合法值，不能据此丢弃，否则 overlay
    // 在副屏上永久冻结。
    QPoint cursorPos;
    if (inputManager_) {
        cursorPos = inputManager_->cursorPosition();
    } else {
        const bool useCaptureOrigin = screenCapturer_ && screenCapturer_->width() > 0;
        const int originX = useCaptureOrigin ? screenCapturer_->originX() : screenGeometry_.x();
        const int originY = useCaptureOrigin ? screenCapturer_->originY() : screenGeometry_.y();
        cursorPos = QCursor::pos() - QPoint(originX, originY);
    }
    const int relativeX = cursorPos.x();
    const int relativeY = cursorPos.y();

    if (lastCursorPos_.x() != relativeX || lastCursorPos_.y() != relativeY) {
        lastCursorPos_ = QPoint(relativeX, relativeY);

        QJsonObject cursorInfo;
        cursorInfo["type"] = "cursor_pos";
        cursorInfo["x"] = relativeX;
        cursorInfo["y"] = relativeY;

        // 多客户端时每个查看端都需看到远端光标位置
        wsServer_->broadcastJson(cursorInfo);
    }
}

void RDPServer::onJpegCompressed(const QByteArray& jpegData)
{
    // 模式门：压缩线程与主线程异步，切到视频模式前已入队的 JPEG 结果仍会到达。
    // 若照常广播，MSE/WebCodecs 客户端会在视频流中收到 0x03 图片帧并覆盖画面。
    if (currentMode_ != ServerMode::Image)
        return;
    // 构造二进制包： [1字节类型标识(0x03)] [4字节大端长度] [JPEG数据]
    QByteArray packet;
    QDataStream stream(&packet, QIODevice::WriteOnly);
    stream.setByteOrder(QDataStream::BigEndian);
    stream << quint8(0x03);
    stream << quint32(jpegData.size());
    packet.append(jpegData);

    wsServer_->broadcastBinary(packet);
}

void RDPServer::onEncodedFrame(const QByteArray& data, bool isKeyframe, qint64 timestamp)
{
#ifdef USE_FFMPEG
    // 模式门：编码线程与主线程异步，切回图片模式后仍在途的编码帧不能广播，
    // 否则前端会收到无解码器对应的 0x01/0x02 视频帧（花屏/误触发降级）。
    if (currentMode_ != ServerMode::Video)
        return;
    // [A2] 出站时间戳单调化。两个来源会让客户端看到的帧时间线倒退：
    //   1) 缓存 IDR 回放（replayCachedKeyframe 用旧 ts）；
    //   2) helper/编码器重启后 `now - startTime_` 从 0 重新计时，而客户端缓冲里
    //      还是上一轮的大 ts → MSE 判为乱序分片，整段丢弃（表现为"重启后黑屏一会儿"）。
    // 统一在这里抬到"上一帧 +1"：正常帧原样通过，不引入累积漂移。
    timestamp = qMax(timestamp, lastSentVideoTs_ + 1);
    lastSentVideoTs_ = timestamp;
    // 缓存最近关键帧，供新客户端接入时立即回放以引导出图
    if (isKeyframe) {
        lastKeyframeData_ = data;
        lastKeyframeTs_ = timestamp;
        lastKeyframeWallMs_ = monoNowMs();
        // IDR 已产出：其对应的"待命请求"被满足，撤销兜底重建。
        idrRequestMs_ = 0;
        if (idrFallbackTimer_)
            idrFallbackTimer_->stop();
    }
    // [PLI-REUSE] 记录缓存 IDR 对应的"桌面内容版本"。
    // 采集器的 contentChangeCount() 只在画面**真实变化**时递增（forceNextFrame 的
    // 强制抓帧画面未变、校验和相同，故不计数）。于是
    //     contentChangeCount() == staticIdrEpoch_
    // ⇔ 自该 IDR 起桌面逐像素未变 ⇔ 重发缓存 IDR 与"重新抓屏编码出的 IDR"等价。
    // 判据比"自缓存以来编码帧数 == 0"稳健：openh264 有 1 帧流水线延迟，重建编码器
    // 逼 IDR 时必然多吐 1 个 P 帧（同画面），旧判据因此永不成立（实测 framesSinceKf=1）。
    if (isKeyframe) {
        staticIdrEpoch_ = screenCapturer_ ? screenCapturer_->contentChangeCount() : 0;
        staticIdrEpochValid_ = true;
    }
#endif
#ifdef USE_WEBRTC
    // 通过 RTP 发给所有已连通的 WebRTC 客户端。
    // 注意：sendFrame() 内部可能同步 emit failed()/closed() → stopWebRtcSession()
    // 把当前会话从 webrtcSessions_ 中 erase。QMap 节点式容器：erase 只失效指向
    // 被删元素的迭代器，因此"先推进迭代器再 sendFrame"即可安全遍历，
    // 且免去旧快照 keys() 方案每帧一次的 QList 堆分配（30fps 下纯浪费）。
    auto it = webrtcSessions_.begin();
    while (it != webrtcSessions_.end()) {
        WebRtcSession* s = it.value();
        const QString sessionId = it.key();
        ++it; // 先推进：sendFrame 可能同步移除当前会话
        if (!s)
            continue;
#ifdef USE_FFMPEG
        // 会话 offer 的编码类型必须与编码器现在产出的码流一致。共享一个编码器，
        // 而编码类型可以中途被改（典型：A 端接受 H.265 正在推流，B 端拒收 H265 后
        // 服务端把 WebRTC 钉回 H.264 并重建编码器）。继续给 HEVC 会话投 H.264 帧，
        // 浏览器按 2 字节 NAL 头解 1 字节头 → 整条会话解不出任何帧，比断开更糟
        // （黑屏还没有降级契机）。拆掉这个会话，前端按既定路径降级/重连重新协商。
        if (videoEncoder_ && s->codec() != videoEncoder_->currentCodec()) {
            qWarning() << "WebRTC session codec stale:" << codecToString(s->codec())
                       << "-> " << codecToString(videoEncoder_->currentCodec())
                       << "; restarting negotiation for" << sessionId;
            stopWebRtcSession(sessionId);
            wsServer_->sendJson(sessionId, QJsonObject {
                { "type", "signal_state" },
                { "state", "failed" },
                { "reason", QStringLiteral("codec_changed") },
            });
            continue;
        }
#endif
        s->sendFrame(data, isKeyframe);
    }
#endif
    // WS 客户端（排除列表中的走 RTP，不再收 WS 帧）
    wsServer_->broadcastFrame(data, isKeyframe, timestamp);
}

bool RDPServer::applyConfigFromHttp(const QJsonObject& cfg, QString* error)
{
    auto reject = [&](const QString& msg) {
        if (error)
            *error = msg;
        qWarning() << "HTTP config rejected:" << msg;
        return false;
    };

    // 消息类型由服务端决定，前端不传（否则这个入口就成了"任意消息注入"）。
    // 只允许改显示/编码参数，不允许 keydown/input 等输入类消息。
    QJsonObject input;
    for (auto it = cfg.begin(); it != cfg.end(); ++it) {
        const QString k = it.key();
        if (k == "quality" || k == "fps" || k == "scale" || k == "codec"
                || k == "hw_encode" || k == "systemName" || k == "width" || k == "height")
            input.insert(k, it.value());
    }
    if (input.isEmpty())
        return reject(QStringLiteral("没有可应用的配置项"));

    // 参数校验：旧实现把非法值交给 onInputReceived，内部多数字段是"不合法就忽略"，
    // 但 HTTP 一律回 200 success，前端以为设置已生效。这里先挡掉非法值。
    if (input.contains("quality")) {
        const QString q = input.value("quality").toString();
        if (q != QLatin1String("high") && q != QLatin1String("medium")
            && q != QLatin1String("low") && q != QLatin1String("verylow")) {
            return reject(QStringLiteral("quality 必须是 high/medium/low/verylow"));
        }
    }
    if (input.contains("fps")) {
        const int f = input.value("fps").toInt(0);
        if (f < 1 || f > 60)
            return reject(QStringLiteral("fps 必须在 1..60"));
    }
    if (input.contains("scale")) {
        const int s = input.value("scale").toInt(0);
        if (s < 10 || s > 100)
            return reject(QStringLiteral("scale 必须在 10..100"));
    }
    if (input.contains("codec")) {
        const QString c = input.value("codec").toString().toLower();
        // 只接受 codecFromString 真正认识的协议。旧白名单里的 h265/mjpeg 会被
        // codecFromString 静默回退成 H.264，接口却回 success —— 删掉这两项，
        // 让拒绝发生在入口（hevc 是 H.265 的正式名，前端统一传 hevc）。
        if (c != QLatin1String("h264") && c != QLatin1String("hevc")
            && c != QLatin1String("vp8") && c != QLatin1String("vp9")
            && c != QLatin1String("av1")) {
            return reject(QStringLiteral("不支持的 codec: %1").arg(c));
        }
    }
    if (input.contains("hw_encode")) {
        const QString h = input.value("hw_encode").toString().toLower();
        if (h != QLatin1String("auto") && h != QLatin1String("on")
            && h != QLatin1String("off")) {
            return reject(QStringLiteral("hw_encode 必须是 auto/on/off"));
        }
    }

    // 分辨率请求：旧实现把 type 改成 set_resolution 后整条消息只走分辨率分支，
    // 其余字段被静默忽略且仍回 success。现在混用直接拒绝，单一分辨率请求则
    // 把真实结果带回去（applyResolutionChange 失败会返回 false → HTTP 4xx）。
    if (input.contains("width") || input.contains("height")) {
        if (!input.contains("width") || !input.contains("height"))
            return reject(QStringLiteral("width 与 height 必须同时提供"));
        // 分辨率切换会停/起采集器并重建编码器；同一条请求里再带 quality/fps/scale/
        // codec 等字段会被 onInputReceived 的分辨率分支静默丢弃（旧实现仍回 success）。
        // 显式拒绝混用，避免"只改了一半却报成功"；这些参数请分开请求。
        QStringList mixed;
        const QStringList resolutionExclusive = {
            QStringLiteral("quality"), QStringLiteral("fps"), QStringLiteral("scale"),
            QStringLiteral("codec"), QStringLiteral("hw_encode"), QStringLiteral("systemName")
        };
        for (const QString& key : resolutionExclusive) {
            if (input.contains(key))
                mixed << key;
        }
        if (!mixed.isEmpty()) {
            return reject(QStringLiteral("width/height 不能与 %1 同时提交；请分开请求")
                              .arg(mixed.join(QStringLiteral("/"))));
        }
        const int w = input.value("width").toInt(0);
        const int h = input.value("height").toInt(0);
        if (w <= 0 || h <= 0)
            return reject(QStringLiteral("width/height 必须为正整数"));
        input["type"] = QStringLiteral("set_resolution");
        qInfo() << "HTTP config applied:" << QString::fromUtf8(
            QJsonDocument(input).toJson(QJsonDocument::Compact));
        if (serviceMode_) {
            // 服务模式下分辨率由 helper 应用，本进程只能确认"已下发"。
            onInputReceived(QString(), input);
            return true;
        }
        return applyResolutionChange(w, h, QString());
    }

    input["type"] = QStringLiteral("config");
    qInfo() << "HTTP config applied:" << QString::fromUtf8(
        QJsonDocument(input).toJson(QJsonDocument::Compact));
    onInputReceived(QString(), input);
    return true;
}

bool RDPServer::applyResolutionChange(int w, int h, const QString& clientId)
{
    // 采集启动期间不允许停/起采集器：此时可能正处在 Wayland 初始化的
    // 嵌套事件循环里，stop()+start() 会销毁正在使用的平台捕获器。
    if (captureStarting_) {
        wsServer_->sendJson(clientId, QJsonObject {
            { "type", "error" },
            { "message", "屏幕采集正在启动，请稍后重试" } });
        return false;
    }
    if (!ScreenCapturer::changeDisplayResolution(w, h)) {
        wsServer_->sendJson(clientId, QJsonObject {
            { "type", "error" },
            { "message", QString("分辨率 %1x%2 切换失败").arg(w).arg(h) } });
        return false;
    }
    // 记录切换前的实际尺寸，供采集器重启失败时回滚。
    const int oldW = screenCapturer_ ? screenCapturer_->width() : 0;
    const int oldH = screenCapturer_ ? screenCapturer_->height() : 0;
    if (screenCapturer_)
        screenCapturer_->stop();
    if (screenCapturer_ && !screenCapturer_->start(configFps_)) {
        // 旧实现只 qWarning 然后照常广播 screen_info + return true，调用方以为成功。
        // 现在必须回滚：先把显示模式切回旧尺寸，再尝试恢复采集；都失败则明确报错。
        qCritical() << "Failed to restart capturer after resolution change; rolling back";
        bool rolledBack = false;
        if (oldW > 0 && oldH > 0 && ScreenCapturer::changeDisplayResolution(oldW, oldH))
            rolledBack = screenCapturer_->start(configFps_);
        // 采集没恢复时 width() 可能是 0，不能把 0 写进输入归一化基准。
        if (inputManager_ && screenCapturer_ && screenCapturer_->width() > 0
                && screenCapturer_->height() > 0) {
            inputManager_->setScreenSize(screenCapturer_->width(), screenCapturer_->height());
            inputManager_->setScreenOffset(screenCapturer_->originX(), screenCapturer_->originY());
        }
        wsServer_->sendJson(clientId, QJsonObject {
            { "type", "error" },
            { "message", rolledBack
                ? QString("分辨率 %1x%2 切换失败，已恢复原分辨率 %3x%4")
                      .arg(w).arg(h).arg(oldW).arg(oldH)
                : QString("分辨率 %1x%2 切换失败，且恢复原分辨率失败；屏幕采集已停止")
                      .arg(w).arg(h) } });
        return false;
    }
    // [B-4] 同步输入归一化基准：uinput 绝对定位按屏幕尺寸缩放 0..65535，
    // 不同步会导致切分辨率后鼠标坐标系统性偏移（helper 路径已修、此处为直连路径）。
    // 优先用抓屏器回报的实际尺寸（显示器可能未精确应用请求的模式）。
    if (inputManager_)
        inputManager_->setScreenSize(
            screenCapturer_ && screenCapturer_->width() > 0 ? screenCapturer_->width() : w,
            screenCapturer_ && screenCapturer_->height() > 0 ? screenCapturer_->height() : h);
    if (inputManager_ && screenCapturer_)
        inputManager_->setScreenOffset(screenCapturer_->originX(), screenCapturer_->originY());
#ifdef USE_FFMPEG
    // 分辨率变化后必须重建编码器：sws/编码器上下文仍按旧尺寸建立，
    // 继续投帧会让 sws_scale 按旧宽度读新帧 → 堆越界读。
    // 缓存的关键帧也一并作废（尺寸与参数集都已变化）。
    lastKeyframeData_.clear();
    lastKeyframeTs_ = 0;
    reinitVideoEncoderForScale();
#endif
    QJsonObject info;
    info["type"] = "screen_info";
    // 必须广播**实际**尺寸：显示器可能未精确应用请求的模式（如 Android
    // 上 1800x1200 被 XFCE 改成 1600x1200）。广播请求值会让前端按错误
    // 宽高做坐标映射，所有点击系统性偏移。
    const int actualW = (screenCapturer_ && screenCapturer_->width() > 0)
            ? screenCapturer_->width() : w;
    const int actualH = (screenCapturer_ && screenCapturer_->height() > 0)
            ? screenCapturer_->height() : h;
    info["width"] = actualW;
    info["height"] = actualH;
    info["platform"] = platformTag();
    wsServer_->broadcastJson(info);
    return true;
}

void RDPServer::updateClipboardSuspend()
{
    if (!clipboardService_)
        return;
    const bool noViewer = (wsServer_ && wsServer_->clientCount() == 0);
    clipboardService_->setSuspended(clipSuspendedByHelper_ || noViewer);
}

QString RDPServer::qualityName() const
{
    // 15 是 verylow 的内部码率档（videoBitrateFor 的 0.035bpp 档），不是"低"。
    if (configQuality_ >= 80)
        return QStringLiteral("high");
    if (configQuality_ <= 15)
        return QStringLiteral("verylow");
    if (configQuality_ <= 35)
        return QStringLiteral("low");
    return QStringLiteral("medium");
}

void RDPServer::invalidateCachedIdr()
{
#ifdef USE_FFMPEG
    lastKeyframeData_.clear();
    lastKeyframeTs_ = 0;
    // 复用基准一并作废：staticIdrEpoch_ 在服务模式下恒为 0（本进程没有采集器），
    // 保留 valid 会让"桌面未变"这个判据永远成立。
    staticIdrEpochValid_ = false;
#endif
    // 分辨率/画质变化后 SPS/PPS 也会变；helper 重建编码器后会重发 codec_config。
    lastCodecExtra_.clear();
}

bool RDPServer::replayCachedKeyframe(const QString& clientId, qint64 maxAgeMs)
{
#ifdef USE_FFMPEG
    if (lastKeyframeData_.isEmpty())
        return false;
    if (maxAgeMs > 0 && lastKeyframeWallMs_ > 0
            && monoNowMs() - lastKeyframeWallMs_ > maxAgeMs)
        return false;
    // 时间戳必须严格大于"已经下发过的最大值"。缓存帧带的是它产出时的旧 ts，
    // 原样下发会让客户端帧时间线倒退（实测 XP 回退 422~3563ms、Win7 最大 2684ms），
    // 而前端 LL-MSE 的播放头早已越过该位置：这一帧既救不了画面，又白花 55~60KB，
    // 还多一次解码报错/花屏的机会。
    const qint64 ts = qMax(lastKeyframeTs_, lastSentVideoTs_ + 1);
    lastSentVideoTs_ = ts;
    wsServer_->sendFrameToClient(clientId, lastKeyframeData_, true, ts);
    return true;
#else
    Q_UNUSED(clientId);
    Q_UNUSED(maxAgeMs);
    return false;
#endif
}

void RDPServer::pumpKeyframe(bool requestNewIdr, bool allowStaticReuse)
{
#ifdef USE_FFMPEG
    if (currentMode_ != ServerMode::Video)
        return;

    // 服务模式：编码在 helper 侧，**桌面是否变化只有 helper 知道**——服务进程的
    // screenCapturer_ 为空，下面那套基于 contentChangeCount 的本地复用判据在这里
    // 永远不成立（实测 [PLI-REUSE] replayed 恒为 0，每次请求都退化成一次重建）。
    // 因此服务模式直接转发请求，让 helper 自己按"桌面是否逐像素未变"决定重发缓存
    // IDR 还是重建编码器；服务端不再做（也不可能做）这个判断。
    if (remoteVideoSourceAvailable()) {
        QJsonObject rq;
        rq["type"] = "request_keyframe";
        // 语义标记：true = 浏览器 PLI（可能可以复用），false = 显式请求新 IDR。
        // helper 现在的判据是"桌面变没变"，此字段只用于日志区分请求来源。
        rq["reuse_ok"] = allowStaticReuse && !requestNewIdr;
        wsServer_->sendToCaptureSource(rq);
        Q_UNUSED(requestNewIdr);
        return;
    }

    // [PLI-REUSE] 静止桌面下的 PLI 优化（判据说明见 rdpserver.h）。
    // 复用前提：有缓存 IDR、缓存时的桌面内容版本 == 当前内容版本（桌面未变）、
    // 距上次复用超过冷却。任一不满足都退到下方"产一帧真 IDR"。
    if (allowStaticReuse && !requestNewIdr && !lastKeyframeData_.isEmpty()
            && staticIdrEpochValid_ && screenCapturer_
            && screenCapturer_->contentChangeCount() == staticIdrEpoch_) {
        constexpr qint64 kPliReuseCooldownMs = 400; // PLI 风暴下的重发冷却
        const qint64 now = monoNowMs();
        if (now - lastPliReuseMs_ < kPliReuseCooldownMs) {
            // 冷却期内：刚刚才重发过同一份 IDR（浏览器尚未收到/尚未反应），
            // 直接忽略这次重复请求。**绝不能退到下方的"抓屏 + 编码"**——这正是
            // 要避免的开销（旧实现每 ~3s 抓一帧 + 编码 IDR 即由此而来）。
            // 实测浏览器 PLI 会成对到达（同一毫秒两次），不拦就会漏抓一次。
            return;
        }
        bool sent = false;
#ifdef USE_WEBRTC
        // 广播给所有 WebRTC 会话（与 onEncodedFrame 语义一致：一帧 IDR 对所有
        // RTP 流都有效）。sendFrame 内部按 rtpClock_.elapsed() 取当前时间戳，
        // 因此重发不会造成 RTP 时间戳回退（Chrome 不按重复帧丢弃）。
        // QMap 节点式容器：erase 只失效被删元素的迭代器，故"先推进再发送"安全。
        auto it = webrtcSessions_.begin();
        while (it != webrtcSessions_.end()) {
            WebRtcSession* s = it.value();
            ++it;
            if (s) {
                s->sendFrame(lastKeyframeData_, true);
                sent = true;
            }
        }
#endif
        if (sent) {
            lastPliReuseMs_ = now;
            if (pliReuseCount_++ % 10 == 0)
                qInfo() << "[PLI-REUSE] replayed cached IDR (static desktop, no capture/encode)"
                        << "reuse=" << pliReuseCount_;
        }
        // sent == false（当前无 WebRTC 会话要帧）时同样无需抓屏编码：没有人接。
        return;
    }
    if (allowStaticReuse && pliEncodeCount_++ % 10 == 0)
        qInfo() << "[PLI-REUSE] need fresh IDR (enc=" << pliEncodeCount_
                << "epoch=" << staticIdrEpoch_ << "cur="
                << (screenCapturer_ ? screenCapturer_->contentChangeCount() : 0) << ")";

    if (!videoEncoder_)
        return;

    // allowStaticReuse（浏览器 PLI）在复用不成立时同样必须产**新 IDR**：PLI 的语义
    // 就是"给我一帧能解码的 IDR"，继续回 P 帧会让浏览器每 ~3s 反复索要（旧行为的
    // 根因，见 rdpserver.h）。requestNewIdr 是前端的显式 request_keyframe。
    if (requestNewIdr || allowStaticReuse) {
        // 显式请求新 IDR：必须产生**新的**关键帧，因为请求方大概率已经丢弃了此前
        // 所有帧（WS 的 videoStarted_ 门在客户端重连/切模式后会被清空，只有新 IDR
        // 才能放行后续 P 帧）。
        const qint64 now = monoNowMs();
        if (now - lastReinitMs_ > 1000) {
            lastReinitMs_ = now;
            // 先试编码器自身的强制关键帧（flush + pict_type=I）；libx264 等软编可能
            // 不认（openh264 实测仍输出 P 帧），故启动单次兜底定时器，超时未见 IDR
            // 就重建编码器（重建后首帧必为 IDR）。
            videoEncoder_->requestKeyframe();
            if (screenCapturer_)
                screenCapturer_->forceNextFrame();
            idrRequestMs_ = now;
            if (idrFallbackTimer_)
                idrFallbackTimer_->start();
            qInfo() << "pumpKeyframe: new IDR requested";
        } else if (idrFallbackTimer_) {
            // 落在 1s 冷却内不等于请求可以作废：这条 request_keyframe 来自"我已经没有
            // 可用 IDR 了"的客户端（接入、重连、PLI），直接 return 会让它永远等不到
            // IDR —— hasIdr_ 已为真时周期兜底也不会介入（实测前端只重试 4 次就放弃，
            // 表现是永久黑屏）。这里改为把请求挂起来，冷却一到由兜底定时器补做
            // （它的 Retry 分支就是为"冷却未到/线程在飞"设计的）。
            idrRequestMs_ = now;
            idrFallbackTimer_->start(qint64(1000) - (now - lastReinitMs_));
            qInfo() << "pumpKeyframe: in cooldown, IDR request deferred to fallback timer";
        }
        return;
    }
    // 请求编码器下一帧出关键帧，并强制抓取当前屏幕（绕过去重），
    // 这样即便桌面长时间静止，新接入的客户端也能拿到一帧关键帧引导出图。
    // 部分软编（ARM 上 h264_mmal/v4l2m2m、旧版 ffmpeg 的默认 h264）不吃
    // avcodec_flush_buffers + frame->pict_type：flush 后仍输出 P 帧，
    // WebRTC/MSE 永远收不到 IDR 导致黑屏。此时重建编码器（重新 open 后
    // 首帧自然为 IDR），且仅当编码器从未产出过 IDR 时执行；带冷却避免
    // 2s 定时器与编码线程竞争（曾因连续重建触发 MPP 库崩溃）。
    if (!videoEncoder_->hasIdr()) {
        const qint64 now = monoNowMs();
        // 等待窗口按实测单帧耗时放大：固定 4s 在单帧 2s 以上的机器上会在首帧 IDR
        // 产出前就触发重建（hasIdr_ 只在编码完成时才置位），同样导致活锁。
        // 另外必须扣除"编码器刚重建、还没机会处理帧"的时间，否则每次接入新客户端
        //（其间会重建编码器）都会立刻再重建一次——白停一次线程 + 丢一队帧。
        // age 用编码器自己的单调时钟（不要拿本地墙钟与内部时刻相减）。
        const qint64 window = qMax<qint64>(4000, 4 * videoEncoder_->lastFrameCostMs());
        const qint64 age = videoEncoder_->initAgeMs();
        if (now - lastReinitMs_ > window && age > window) {
            // busy 长时间不归零说明线程卡死在驱动调用里（或单帧耗时已经大到永远
            // 追不上），无限推迟等于永久黑屏——超过硬上限强制重建；否则走原子退役
            // （retireIfIdle），恰好有帧在飞就留到下一轮 2s 定时器再判。
            // 绝不能先 busyForMs() 判空闲再 shutdownRetire()：两次调用之间编码线程
            // 可以开始下一帧，主线程就要白等一整帧（弱机 1~2s）。
            const qint64 lastCost = videoEncoder_->lastFrameCostMs();
            const bool hung = age >= qMax<qint64>(30000, 10 * lastCost);
            if (hung)
                qWarning() << "[PLI-REUSE] encoder produced no IDR for" << age
                           << "ms, forcing rebuild (possible hang)";
            const RebuildResult r = hung ? rebuildVideoEncoder(/*onlyIfIdle=*/false)
                                         : rebuildVideoEncoder(/*onlyIfIdle=*/true);
            // Deferred（有帧在飞）/ NotApplicable：本次没有实际重建，冷却时间戳
            // 不能推进，否则每次接入新客户端都会白等一整个窗口。
            if (r != RebuildResult::Done)
                return;
            lastReinitMs_ = now;
            qInfo() << "Video encoder produced no IDR yet, rebuilt to force keyframe"
                    << "(last frame" << lastCost << "ms)";
        }
    }
    // 不再调用 requestKeyframe()：90/ARM 软编 flush 不产 IDR 且会打断刚重建的编码器，
    // hasIdr_ 为 true 后保持稳定推流（GOP 由编码器自身控制）。
    if (screenCapturer_)
        screenCapturer_->forceNextFrame();
#endif
}

void RDPServer::switchToImageMode()
{
    if (currentMode_ == ServerMode::Image)
        return;

    // [ASYNC-RETIRE] 先把数据源切走：此后采集帧不再投给旧编码器，编码线程的在途帧
    // 也会被 onEncodedFrame 的模式门丢弃；下面的退役因此无需主线程阻塞等待。
    currentMode_ = ServerMode::Image;

#ifdef USE_WEBRTC
    // 切图片模式时结束所有 WebRTC 会话：RTP 推流依赖 H.264 编码器，
    // 编码器即将销毁，保留会话只会让客户端对着断流等待。
    // （旧逻辑在会话已连通时直接拒绝切图片，会把用户锁死在视频模式）
    {
        QList<QString> ids = webrtcSessions_.keys();
        for (const QString& id : ids)
            stopWebRtcSession(id);
    }
#endif

    // 服务模式：通知 helper 退出视频编码、恢复 JPEG 上报（否则 helper 继续发 H.264
    // 而服务端已切图片模式，浏览器侧无对应解码通道 → 画面停滞）。
    if (serviceMode_ && !screenCapturer_ && wsServer_ && wsServer_->isCaptureSourceConnected()) {
        QJsonObject ctrl;
        ctrl["type"] = "capture_control";
        ctrl["action"] = "video_off";
        wsServer_->sendToCaptureSource(ctrl);
    }

    // 释放视频编码器（会调用析构，析构中调用 shutdown）
#ifdef USE_FFMPEG
    // [ASYNC-RETIRE] 切图片模式是用户高频操作：只置 abort_ 唤醒编码线程；线程若已
    // 退出则立即析构释放，否则移入墓地由回收定时器处理，主线程不再同步等最长 8s。
    // 旧编码器已断开全部信号，在途帧不会回调到本对象；线程退出后由回收定时器析构。
    retireVideoEncoderAsync(/*onlyIfIdle=*/false);
    // 编码器销毁后，缓存的关键帧与 codec extradata 全部作废：
    // 若保留，下次切回视频模式时新客户端会收到上一次编码器的 IDR/SPS/PPS，
    // 而前端已按新的 codec_config 配置解码器 → 花屏或黑屏。
    lastKeyframeData_.clear();
    lastKeyframeTs_ = 0;
    lastCodecExtra_.clear();
#endif

#ifdef USE_WEBRTC
    // 图片模式不涉及编码：恢复 WebRTC 强制 H.264 之前的用户选择，
    // 否则用一次 WebRTC 后选择就被永久钉在 H.264。
    if (codecOverriddenForWebRtc_) {
        configCodec_ = codecBeforeWebRtc_;
        codecOverriddenForWebRtc_ = false;
        qInfo() << "Restored user codec after WebRTC:" << codecToString(configCodec_);
    }
#endif

    // 图片模式恒全量抓取：规避增量 XDamage 漏报子窗口（任务栏/顶部面板）
    // 重绘导致的局部黑块，且图片模式多为低动态，全量抓取的 CPU 成本可接受。
    if (screenCapturer_) {
        screenCapturer_->setFullCaptureOnly(true);
        // 立即强制抓一帧：视频模式期间桌面可能静止，checksum 去重会让客户端
        // 切回图片后迟迟等不到第一张 JPEG（画面停留在旧帧/黑屏）。
        screenCapturer_->forceNextFrame();
    }

    // 通知所有客户端模式已变更
    QJsonObject notification;
    notification["type"] = "mode_changed";
    notification["mode"] = "image";
    notification["hwEncode"] = hwEncodeAvailable();
    notification["videoSupported"] = videoModeSupported();
    notification["codec"] = codecToString(configCodec_);
    wsServer_->broadcastJson(notification);

    qInfo() << "Switched to image mode";
}

bool RDPServer::switchToVideoMode()
{
    if (currentMode_ == ServerMode::Video)
        return true;

    const bool localSource = screenCapturer_ && screenCapturer_->width() > 0
        && screenCapturer_->height() > 0;

    if (!localSource) {
        // 服务模式：画面在 helper 进程。若 helper 已连且具备视频能力，则进入"远端编码"
        // 视频模式——由 helper 完成 H.264 编码，编码帧经 /capture 上报（服务端不编码）。
        if (!remoteVideoSourceAvailable()) {
            qWarning() << "Cannot switch to video mode: no local raw frame source "
                          "and no video-capable helper";
            return false;
        }
#ifdef USE_FFMPEG
        sendVideoParamsToCaptureSource(QStringLiteral("video_on"));
        currentMode_ = ServerMode::Video;
        // 上一轮会话的缓存全部作废（新编码器的 IDR/SPS/PPS 稍后由 helper 上报）
        lastKeyframeData_.clear();
        lastKeyframeTs_ = 0;
        lastCodecExtra_.clear();

        QJsonObject notification;
        notification["type"] = "mode_changed";
        notification["mode"] = "video";
        notification["hwEncode"] = captureHwEncode_;
        notification["videoSupported"] = true;
        notification["codec"] = captureCodecName_;
        wsServer_->broadcastJson(notification);
        qInfo() << "Switched to video mode (helper-side H.264 encoding)";
        return true;
#else
        return false;
#endif
    }

#ifdef USE_FFMPEG
    // 如果编码器已被释放，重新创建
    createVideoEncoder();
    if (!videoEncoder_)
        return false;
    {
        // 上述三个信号的接线已统一到 createVideoEncoder()
    }

    // 尝试重新初始化视频编码器（按当前缩放档位编码，避免始终全分辨率软编导致 CPU 高）
    int sw = screenCapturer_->width();
    int sh = screenCapturer_->height();
    int encW = sw, encH = sh;
    if (configScale_ < 100) {
        int tw = encW * configScale_ / 100;
        int th = encH * configScale_ / 100;
        if (tw > 0 && th > 0) {
            encW = tw;
            encH = th;
        }
    }
    if (!videoEncoder_->initialize(configCodec_, sw, sh, encW, encH,
            configFps_, videoBitrateFor(encW, encH, configFps_, configCodec_), configHwEncodeMode_)) {
        // 编码器初始化失败（如软编不支持 hevc/vp9/av1）：回退 H.264 重试，
        // 保证视频/WebRTC 模式在任意平台上始终可用（WebRTC 固定 H.264）。
        qWarning() << "Failed to initialize video encoder (" << codecToString(configCodec_)
                   << "), falling back to H.264";
        configCodec_ = CodecType::H264;
        if (!videoEncoder_->initialize(configCodec_, sw, sh, encW, encH,
                configFps_, videoBitrateFor(encW, encH, configFps_, configCodec_), configHwEncodeMode_)) {
            qWarning() << "Failed to initialize video encoder (H.264 fallback), staying in image mode";
            // 两次 initialize 都失败：残留的编码器上下文不可用，尽早释放。
            // 否则对象留在"半初始化"状态（编码线程已启动、上下文为空），
            // 下次切视频模式时复用会出现状态错乱/编码线程空转。
            // [ASYNC-RETIRE] 非阻塞退役，失败路径也不再让主线程同步等最长 8s。
            retireVideoEncoderAsync(/*onlyIfIdle=*/false);
            return false;
        }
    }
    videoBaseBitrate_ = videoBitrateFor(encW, encH, configFps_, configCodec_);
    // 新编码器就绪：上一会话的缓存 IDR / 静态复用标记作废（与 reinit 路径一致）
    lastKeyframeData_.clear();
    lastKeyframeTs_ = 0;
    staticIdrEpochValid_ = false;

    currentMode_ = ServerMode::Video;
    // 视频模式恢复增量抓取（低延迟、省 CPU）；图片模式关闭此开关时已恒全量。
    if (screenCapturer_) {
        screenCapturer_->setFullCaptureOnly(false);
    }

    // 通知所有客户端
    QJsonObject notification;
    notification["type"] = "mode_changed";
    notification["mode"] = "video";
    notification["hwEncode"] = hwEncodeAvailable();
    notification["videoSupported"] = true; // 已成功进入视频模式
    notification["codec"] = codecToString(configCodec_);
    wsServer_->broadcastJson(notification);

    qInfo() << "Switched to video mode";
    return true;
#else
    return false;
#endif
}

#ifdef USE_FFMPEG
void RDPServer::createVideoEncoder()
{
    if (videoEncoder_)
        return;

    videoEncoder_ = std::unique_ptr<VideoEncoder>(new VideoEncoder(this));
    // 代际：创建/退役各递增一次。disconnect 不会撤销已经排进主线程队列的元调用，
    // 所以每个 lambda 捕获创建时的代际号，投递时若已换代就丢弃——否则旧分辨率
    // 帧会进新 sws、旧 avcC 会覆盖新的 SPS/PPS，表现为花屏。
    const quint64 gen = ++videoEncoderGeneration_;
    connect(videoEncoder_.get(), &VideoEncoder::encodedFrame,
        this, [this, gen](const QByteArray& data, bool isKeyframe, qint64 timestamp) {
            if (gen != videoEncoderGeneration_)
                return;
            onEncodedFrame(data, isKeyframe, timestamp);
        });
    connect(videoEncoder_.get(), &VideoEncoder::codecConfigChanged,
        this, [this, gen](const QByteArray& extradata) {
            if (gen != videoEncoderGeneration_)
                return;
            onCodecConfigChanged(extradata);
        });
    // 编码器（重）初始化完成：请求捕获端强制推帧（静止桌面首帧兜底）
    connect(videoEncoder_.get(), &VideoEncoder::encoderReady,
        this, [this, gen]() {
            if (gen != videoEncoderGeneration_)
                return;
            if (screenCapturer_)
                screenCapturer_->forceNextFrame();
        });
    // 过载时自动降码率，恢复后回到基准码率
    connect(videoEncoder_.get(), &VideoEncoder::encoderOverload,
        this, [this, gen](bool overloaded) {
            if (gen != videoEncoderGeneration_ || !videoEncoder_)
                return;
            if (overloaded) {
                int cur = videoEncoder_->currentBitrate();
                int reduced = qMax(150000, static_cast<int>(cur * 0.6));
                videoEncoder_->setBitrate(reduced);
                qWarning() << "Video encoder overload: reducing bitrate to" << reduced;
            } else if (videoBaseBitrate_ > 0) {
                videoEncoder_->setBitrate(videoBaseBitrate_);
                qInfo() << "Video encoder recovered: restoring bitrate to" << videoBaseBitrate_;
            }
        });
    // MPP 硬编被证明不可用：由本线程（主线程）重建为软编。
    // 必须在主线程做：VideoEncoder 的 encodingLoop 不允许重建自己所属的线程。
    connect(videoEncoder_.get(), &VideoEncoder::reinitRequired,
        this, [this, gen]() {
            if (gen != videoEncoderGeneration_ || !videoEncoder_)
                return;
            qInfo() << "Video encoder reinit requested (MPP unavailable), rebuilding with software encoder";
            reinitVideoEncoderForScale();
        });
}

void RDPServer::reinitVideoEncoderForScale()
{
    rebuildVideoEncoder(/*onlyIfIdle=*/false);
#ifdef USE_FFMPEG
    // 配置变更触发的重建：把编码器实际跑起来的协议广播出去，客户端据此对齐解码器。
    // 请求 hevc 而本机没有 hevc 编码器时会静默回退 h264，前端若仍按 hevc 配置解码器，
    // 收到的每一帧都会报错 → 关键帧请求风暴 → 黑屏。
    if (currentMode_ == ServerMode::Video && videoEncoder_) {
        QJsonObject notification;
        notification["type"] = "mode_changed";
        notification["mode"] = "video";
        notification["hwEncode"] = hwEncodeAvailable();
        notification["videoSupported"] = true;
        notification["codec"] = codecToString(videoEncoder_->currentCodec());
        wsServer_->broadcastJson(notification);
    }
#endif
}

// [ASYNC-RETIRE] 退役：主线程切数据源 + 置 abort_，不无限等线程退出。
// 硬编只做 500ms 短超时等待（设备独占）；线程已退出则立即析构释放，否则移入墓地。
// 返回 false 表示 onlyIfIdle 且有在飞帧，未做任何改动（调用方稍后重试）。
bool RDPServer::retireVideoEncoderAsync(bool onlyIfIdle, bool* threadStopped)
{
    if (threadStopped)
        *threadStopped = true;
    if (!videoEncoder_)
        return true;
    if (!videoEncoder_->beginRetire(onlyIfIdle))
        return false;
    // 退役即换代：队列里尚未投递的旧 encodedFrame/codecConfigChanged 全部作废。
    ++videoEncoderGeneration_;
    // 先断信号：即使编码线程还在处理在途帧，也不会再回调本对象（数据源已切走）。
    QObject::disconnect(videoEncoder_.get(), nullptr, this, nullptr);

    // 硬编重建前等旧线程退出。线程还在驱动调用里就让新实例 open，会撞上设备独占
    // （RK MPP 表现为 mpp_venc_kcfg_init 失败），这是"永久退软编"的根因之一。
    // 只等一个短超时，绝不在主线程无限等；等不到就进墓地，由回收定时器延后重建。
    bool stopped = videoEncoder_->threadStopped();
    const bool hardware = videoEncoder_->isHardwareEncoder();
    if (hardware && !stopped) {
        static constexpr int kHwRetireWaitMs = 500;
        stopped = videoEncoder_->waitStopped(kHwRetireWaitMs);
        if (!stopped)
            qWarning() << "Retired HW encoder thread still running after"
                       << kHwRetireWaitMs << "ms; deferring rebuild";
    }
    if (threadStopped)
        *threadStopped = stopped;

    reapRetiredEncoders();
    if (stopped) {
        // 线程已退出：直接析构会走 shutdown() 的 !isRunning 分支，同步释放
        // codecCtx_/mpp_ 等设备资源，让新实例立刻可用；不必留在墓地占位。
        videoEncoder_.reset();
        return true;
    }
    retiredEncoders_.push_back(std::move(videoEncoder_));
    capRetiredEncoders();
    // 线程自行退出后由定时器把墓地对象析构（threadStopped() 后才安全）。
    if (retiredEncoderReapTimer_ && !retiredEncoderReapTimer_->isActive())
        retiredEncoderReapTimer_->start();
    return true;
}

// 墓地硬上限：先回收线程已退出的对象；仍卡死的对象线程可能还在触碰
// codecCtx_/mpp_，析构是 UB，只能放弃所有权（release）阻止 vector 无限增长。
// 这是有意的最后一次泄漏，且每个溢出周期只发生一次并打 qCritical。
void RDPServer::capRetiredEncoders()
{
    static constexpr std::size_t kMaxRetiredEncoders = 4;
    if (retiredEncoders_.size() <= kMaxRetiredEncoders)
        return;
    reapRetiredEncoders();
    while (retiredEncoders_.size() > kMaxRetiredEncoders) {
        std::unique_ptr<VideoEncoder> stuck = std::move(retiredEncoders_.front());
        retiredEncoders_.erase(retiredEncoders_.begin());
        stuck.release();
        qCritical() << "Retired encoder graveyard exceeded" << kMaxRetiredEncoders
                    << "entries while the oldest thread is still wedged; abandoning it (deliberate leak)";
    }
}

// 退役对象只能在线程真正退出后释放：卡死时线程仍在触碰 codecCtx_/mpp_，删除
// 对象是 UB。每次新退役前清扫一次，卡死恢复后墓地不会无限累积历史编码器；
// 仍卡死的对象继续留在墓地里（无安全手段强杀，见 stopEncoderThread 注释）。
void RDPServer::reapRetiredEncoders()
{
    const auto it = std::remove_if(retiredEncoders_.begin(), retiredEncoders_.end(),
        [](const std::unique_ptr<VideoEncoder>& enc) {
            return !enc || enc->threadStopped();
        });
    retiredEncoders_.erase(it, retiredEncoders_.end());
}

// 返回 Deferred 表示"因有在飞帧而推迟"，未对编码器做任何改动；NotApplicable 表示
// 当前没有可重建的编码器/捕获源（重试也不会成功）；其余情况（含初始化失败回退
// 图片模式）都算已处理，返回 Done。
RDPServer::RebuildResult RDPServer::rebuildVideoEncoder(bool onlyIfIdle)
{
    if (!screenCapturer_)
        return RebuildResult::NotApplicable;
    // 上一次因旧硬编线程未退出而推迟创建时，videoEncoder_ 为空但重建请求仍有效。
    // 旧对象还在墓地（线程/设备未释放）时，任何提前的重建请求继续排队，避免
    // PLI/IDR 的 200ms 重试绕过等待直接 open 新硬编实例。
    if (!videoEncoder_ && videoRebuildPending_) {
        if (!retiredEncoders_.empty())
            return RebuildResult::Deferred;
        // 墓地已空：旧线程已退出且资源已释放，可以立即创建。
    } else if (!videoEncoder_) {
        return RebuildResult::NotApplicable;
    }

    const bool wasHardware = videoEncoder_ && videoEncoder_->isHardwareEncoder();
    int sw = screenCapturer_->width(), sh = screenCapturer_->height();
    int ew = sw, eh = sh;
    if (configScale_ < 100) {
        int tw = ew * configScale_ / 100, th = eh * configScale_ / 100;
        if (tw > 0 && th > 0) { ew = tw; eh = th; }
    }

    // [ASYNC-RETIRE] 重建走非阻塞退役：旧编码器只置 abort_ 并移入墓地，主线程不再
    // 同步等最长 8s（旧实现线程卡在驱动调用时每次重建都冻住 UI/输入）。信号已断开，
    // 在途帧不会回调；随后立刻建新编码器继续推流，旧线程醒来自行退出并被定时器回收。
    // onlyIfIdle：IDR 兜底路径专用，空闲判定与置 abort_ 在编码器内部同一把锁
    // 完成（beginRetire），避免先 busyForMs()==0 再停线程之间的竞态。
    bool stopped = true;
    if (!retireVideoEncoderAsync(onlyIfIdle, &stopped))
        return RebuildResult::Deferred;
    if (wasHardware && !stopped) {
        // 旧线程仍可能占着硬编设备：推迟创建，等回收定时器确认线程退出后再建，
        // 避免新实例 open 失败（RK MPP 会返回 init 失败并被误判成硬编不可用）。
        videoRebuildPending_ = true;
        videoRebuildPendingSinceMs_ = monoNowMs();
        if (retiredEncoderReapTimer_ && !retiredEncoderReapTimer_->isActive())
            retiredEncoderReapTimer_->start();
        return RebuildResult::Done;
    }
    createVideoEncoder();
    videoRebuildPending_ = false;
    // 旧编码器已经退役（不可回滚），这里不能报 Deferred 让调用方重试；
    // 返回 Done：新实例在本函数内同步创建，或已排队给墓地的回收定时器在旧线程
    // 退出后创建（videoRebuildPending_）。
    if (!videoEncoder_)
        return RebuildResult::Done;
    // 重建后旧编码器的比特流上下文作废：缓存 IDR/extradata/静态复用标记必须清空，
    // 否则新客户端补发或 PLI 复用会把旧编码器的关键帧喂给新会话（花屏/黑屏）。
    lastKeyframeData_.clear();
    lastKeyframeTs_ = 0;
    staticIdrEpochValid_ = false;
    if (videoEncoder_->initialize(configCodec_, sw, sh, ew, eh,
            configFps_, videoBitrateFor(ew, eh, configFps_, configCodec_), configHwEncodeMode_)) {
        videoBaseBitrate_ = videoBitrateFor(ew, eh, configFps_, configCodec_);
        // 新编码器 open 后首帧自然为 IDR；这里不再 requestKeyframe()，
        // 避免其 avcodec_flush_buffers 干扰刚 open 的编码器（部分软编
        // flush 后仍出 P 帧，导致 IDR 永远不来）。
        qInfo() << "Video encoder re-initialized for scale" << configScale_ << "at" << ew << "x" << eh;
    } else {
        // 当前 codec 初始化失败（软编无此编码器）时回退 H.264 重试，保证推流不中断
        qWarning() << "Failed to re-initialize video encoder for scale" << configScale_
                   << "codec" << codecToString(configCodec_) << ", falling back to H.264";
        configCodec_ = CodecType::H264;
        if (videoEncoder_->initialize(configCodec_, sw, sh, ew, eh,
                configFps_, videoBitrateFor(ew, eh, configFps_, configCodec_), configHwEncodeMode_)) {
            videoBaseBitrate_ = videoBitrateFor(ew, eh, configFps_, configCodec_);
            qInfo() << "Video encoder re-initialized (H.264 fallback) at" << ew << "x" << eh;
        } else {
            qWarning() << "Failed to re-initialize video encoder (H.264 fallback) for scale" << configScale_;
            // 两种编码器都建不起来（无可用编码器/设备被占用）：继续留在视频模式
            // 只会黑屏——videoEncoder_ 已 shutdown，没有任何帧能出。退回图片模式
            // 保证画面可见，用户下次改配置时会重新尝试视频。
            switchToImageMode();
        }
    }
    // 用户请求的 codec 与编码器实际跑起来的 codec 不同时（典型：请求 hevc 但本机
    // 没有 hevc 编码器，静默回退 h264），必须广播真实协议。否则前端解码器仍按
    // hevc 配置去收 H.264 码流，每一帧 decode 报错 → 关键帧请求风暴 → 黑屏。
    return RebuildResult::Done;
}
#endif

bool RDPServer::hwEncodeAvailable() const
{
#ifdef USE_FFMPEG
    // 服务模式：画面与编码都在 helper 侧，以 helper 上报的能力为准
    if (serviceMode_ && !screenCapturer_)
        return captureHwEncode_;
    return VideoEncoder::isHwAcceleratedAvailable(configCodec_);
#else
    return false;
#endif
}

bool RDPServer::remoteVideoSourceAvailable() const
{
    // 服务模式 + 无本地捕获源 + helper 已连且声明支持视频
    return serviceMode_ && !screenCapturer_
        && captureVideoCapable_
        && wsServer_ && wsServer_->isCaptureSourceConnected();
}

// [ADAPT-GEO] 采集几何（用户切的分辨率）变化时松开自适应上限。
// 自适应是按"当时那套几何的单帧读屏耗时"定的，而这个耗时基本与像素数成正比
// （XP 实测 2310x1398→344ms、1024x768→78ms）。上限本身只降不升（设计上是为了不被
// 一次模式切换冲回用户配置值），代价是切回小几何后仍按旧几何的 fps/缩放/节流跑：
// 帧率被一套早已不存在的负担钉住（实测 XP 停在 614x460@10fps、pace 3）。
// 只认几何变化——模式切换、改画质仍然沿用上限，原设计的防抖效果不变。松完后
// helper 重新武装的 [ADAPT] 会按新几何再评估一次，真慢会重新降下来，不会来回震荡。
void RDPServer::releaseAdaptCapsForGeometry(int width, int height)
{
    if (width < 1 || height < 1)
        return;
    if (width == adaptGeomW_ && height == adaptGeomH_)
        return;
    const bool hadCaps = adaptFpsCap_ > 0 || adaptScaleCap_ > 0 || adaptPaceFactor_ > 0.0;
    adaptGeomW_ = width;
    adaptGeomH_ = height;
    // 从未降级过：只更新几何记录，不做任何重推（避免每次上报都白重建一次编码器）
    if (!hadCaps)
        return;

    qInfo() << "Capture geometry changed to" << width << "x" << height
            << "- releasing adaptive caps (was fps" << adaptFpsCap_
            << "scale" << adaptScaleCap_ << "pace" << adaptPaceFactor_ << ")";
    adaptFpsCap_ = 0;
    adaptScaleCap_ = 0;
    adaptPaceFactor_ = 0.0;

    const int prevFps = configFps_, prevScale = configScale_;
    configFps_ = cappedFps(userFps_);
    configScale_ = cappedScale(configScaleWish_);
    const bool paramsChanged = configFps_ != prevFps || configScale_ != prevScale;
    if (screenCapturer_ && configFps_ != prevFps)
        screenCapturer_->setFps(configFps_);
    if (jpegCompressor_ && configScale_ != prevScale)
        jpegCompressor_->setScalePercent(configScale_);
#ifdef USE_FFMPEG
    // 本地编码路径（非服务模式）：生效尺寸/帧率变了要重建，否则编码器仍停在旧档
    if (paramsChanged && currentMode_ == ServerMode::Video && videoEncoder_)
        reinitVideoEncoderForScale();
#endif
    // 服务模式：helper 里的 captureFps/videoScale/paceFactor 是它自己的副本，
    // 必须重推一次 config 才回得到用户参数（helper 收到 config 会按新值重建编码器）。
    if (remoteVideoSourceAvailable())
        sendVideoParamsToCaptureSource(QStringLiteral("video_on"));
    // 通知前端把"实际生效"的 fps/缩放标签抬回真实值
    if (paramsChanged && wsServer_ && wsServer_->clientCount() != 0) {
        QJsonObject out;
        out["type"] = "config_adapted";
        out["avgCapMs"] = 0;
        out["fps"] = configFps_;
        out["scale"] = configScale_;
        wsServer_->broadcastJson(out);
    }
}

void RDPServer::sendVideoParamsToCaptureSource(const QString& action)
{
    if (!wsServer_ || !wsServer_->isCaptureSourceConnected())
        return;
    // 先下发 config（helper 据此确定 codec/hw_encode/fps/scale/画质），再发 capture_control。
    // quality 用字符串档位，与前端 config 消息协议保持一致（helper 已支持前 3 项）。
    QJsonObject cfg;
    cfg["type"] = "config";
    cfg["codec"] = codecToString(configCodec_);
    cfg["hw_encode"] = (configHwEncodeMode_ == HwEncodeMode::On) ? QStringLiteral("on")
                     : (configHwEncodeMode_ == HwEncodeMode::Off) ? QStringLiteral("off")
                                                                  : QStringLiteral("auto");
    cfg["fps"] = configFps_;
    cfg["scale"] = configScale_;
    // 慢读回节流倍数：helper 的采集器据此算采集间隔（占空比 ≈ 1/(1+factor)）。
    // 取用户配置与自适应结果中的较大者：慢机自适应要求更省 CPU 时不能被用户默认值盖回去。
    cfg["paceFactor"] = cappedPaceFactor();
    cfg["quality"] = qualityName();
    wsServer_->sendToCaptureSource(cfg);

    QJsonObject ctrl;
    ctrl["type"] = "capture_control";
    ctrl["action"] = action;
    wsServer_->sendToCaptureSource(ctrl);
}

bool RDPServer::videoModeSupported() const
{
#ifdef USE_FFMPEG
    // ① 本地原始帧源可用（直连模式 / Linux 服务模式进程内捕获）→ 直接可编码
    if (screenCapturer_ && screenCapturer_->width() > 0 && screenCapturer_->height() > 0)
        return true;
#else
    // 无 FFmpeg 构建里没有本地编码器：switchToVideoMode() 本地分支恒返回 false，
    // 本机采集只能走 JPEG。只有 helper 自带编码器（服务模式上报 video=true）时
    // 才可能进入视频模式；否则必须如实告诉前端 videoSupported=false，避免前端
    // 反复尝试视频模式（每次都在 MSE/WebCodecs 上黑屏等待）。
#endif
    // ② 服务模式：画面在 helper 进程，helper 具备视频能力即可（编码在 helper 侧完成）
    return remoteVideoSourceAvailable();
}

// 依据编码分辨率、帧率与画质档位估算 H.264 码率，
// 低分辨率/低画质下自动降低码率以节省单板 CPU，高分辨率下保证可用画质。
// 公式已抽到 VideoEncoder::estimateBitrate()，与服务模式 helper 侧共用，避免漂移。
int RDPServer::videoBitrateFor(int encW, int encH, int fps, CodecType codec) const
{
#ifdef USE_FFMPEG
    return VideoEncoder::estimateBitrate(codec, encW, encH, fps, configQuality_);
#else
    Q_UNUSED(encW); Q_UNUSED(encH); Q_UNUSED(fps); Q_UNUSED(codec);
    return 0;
#endif
}

#ifdef USE_WEBRTC

void RDPServer::startWebRtcSession(const QString& clientId)
{
    if (webrtcSessions_.contains(clientId))
        return;

    // 立即把该客户端从 WS 媒体广播中排除：
    // 若等到 answer/connected 才排除，WebRTC 握手窗口（1~3s）内 WS H.264 帧仍会
    // 推给前端，无 WebCodecs 的浏览器会在 H264Decoder.decode 里触发
    // fallbackToImage → webrtcClose，把正在建立的 WebRTC 会话误杀。
    // stopWebRtcSession 中有对应的 remove，会话结束后恢复广播。
    webrtcExcluded_.insert(clientId);
    wsServer_->setMediaExcludedClients(webrtcExcluded_);

    // 失败路径必须回滚上面的"排除"：否则该客户端既拿不到 RTP（会话没建起来），
    // 又被 WS 广播永久排除（broadcastFrame/sendFrameToClient 都会跳过它），
    // 表现为永久黑屏，只能断线重连才恢复。
    auto failNoEncoder = [this, clientId](const char* reason) {
        webrtcExcluded_.remove(clientId);
        wsServer_->setMediaExcludedClients(webrtcExcluded_);
        wsServer_->sendJson(clientId, QJsonObject {
            { "type", "signal_state" },
            { "state", "failed" },
            { "reason", QString::fromUtf8(reason) },
        });
    };

#ifndef USE_FFMPEG
    // WebRTC 依赖 FFmpeg H.264 编码器
    failNoEncoder("no_encoder");
    return;
#endif

    // WebRTC 依赖 H.264 编码器，先确保处于视频模式。
    // 顺序很重要：必须在强制 H.264 **之前**切换 —— switchToVideoMode() 广播的
    // mode_changed 要带上用户原本选择的 codec，否则前端会把 H.264 当成用户配置
    // 记进 localStorage，稍后回传 config 时把用户选择永久覆盖。
    if (!switchToVideoMode()) {
        failNoEncoder("no_encoder");
        return;
    }

    // WebRTC 的 RTP 封装与 NAL 解析按会话的编码类型走（见 WebRtcSession::create）：
    // H.264 走 1 字节 NAL 头 + FU-A，HEVC 走 2 字节头 + RFC7798 分片，两者不能混。
    // 判定必须用“编码器当前真正在产的编码”，而不是配置值：视频模式刚初始化完编码器，
    // 若它因硬编不可用回退，configCodec_ 与线上码流就不一致了。
    // HEVC 目前只在**本地编码**路径开放——服务模式编码在 helper 侧，服务端只有 helper
    // 上报的布尔 hwEncode 能力、拿不到它实际使用的编码类型，猜错就是整链路黑屏，
    // 所以服务模式维持既有的 H.264 行为。
    // webrtcHevcBlocked_：本进程已有浏览器明确拒收 H265（典型 Firefox 无 HEVC over
    // WebRTC），此后不再用 HEVC 建 WebRTC 会话，避免每台浏览器都先失败一次。
    CodecType webrtcCodec = CodecType::H264;
    if (!webrtcHevcBlocked_ && videoEncoder_
            && videoEncoder_->currentCodec() == CodecType::HEVC)
        webrtcCodec = CodecType::HEVC;
    // 其余情况（软编无 x265、用户选 VP9/AV1、浏览器拒收过 H265）临时改写为
    // webrtcCodec，否则编码器产出 WebRTC 侧没有对应封装的码流，一帧都发不出去（黑屏）。
    // 用户原值保留在 codecBeforeWebRtc_，WebRTC 结束后恢复。
    const bool codecForcedToH264 = (configCodec_ != webrtcCodec);
    if (codecForcedToH264) {
        if (!codecOverriddenForWebRtc_) {
            codecBeforeWebRtc_ = configCodec_;
            codecOverriddenForWebRtc_ = true;
        }
        qWarning() << "WebRTC cannot carry" << codecToString(configCodec_)
                   << "; overriding to" << codecToString(webrtcCodec);
        configCodec_ = webrtcCodec;
    }

    // 视频模式刚按用户 codec 初始化过编码器，这里补一次重建：本地编码器直接
    // 重建；服务模式（编码在 helper）重新下发 config+video_on，让 helper 按
    // webrtcCodec 重建编码器。
    if (codecForcedToH264) {
        if (videoEncoder_ && screenCapturer_)
            reinitVideoEncoderForScale();
        else if (serviceMode_)
            sendVideoParamsToCaptureSource(QStringLiteral("video_on"));
    }

    WebRtcSession* session = new WebRtcSession(this);
    webrtcSessions_.insert(clientId, session);

    connect(session, &WebRtcSession::localOffer, this, [this, clientId](const QString& sdp) {
        sendWebRtcToClient(clientId, QJsonObject {
            { "type", "signal_offer" },
            { "sdp", sdp },
        });
    });

    connect(session, &WebRtcSession::localIce, this, [this, clientId](const QString& candidate, const QString& mid) {
        sendWebRtcToClient(clientId, QJsonObject {
            { "type", "signal_ice" },
            { "candidate", candidate },
            { "mid", mid },
        });
    });

    connect(session, &WebRtcSession::connected, this, [this, clientId]() {
        // 该客户端走 RTP 收流，从 WS 帧广播中排除
        webrtcExcluded_.insert(clientId);
        wsServer_->setMediaExcludedClients(webrtcExcluded_);
        sendWebRtcToClient(clientId, QJsonObject {
            { "type", "signal_state" },
            { "state", "connected" },
        });
        // 连接建立后给客户端一帧可解码的 IDR 引导出图（静态桌面去重不会主动出帧）。
        // allowStaticReuse：静止且有缓存 IDR 时复用，否则产真 IDR；不用旧的
        // forceNextFrame 路径（它产 P 帧，不满足解码起点，还白烧一次抓屏+编码）。
        pumpKeyframe(false, /*allowStaticReuse=*/true);
    });

    connect(session, &WebRtcSession::keyframeRequested, this, [this]() {
        // 浏览器 PLI（Yang_Req_Sendkeyframe）。静态桌面下浏览器会周期性地要关键帧，
        // 若每次都强制抓屏 + 全屏 IDR 编码，就是静止时视频模式 CPU 高企的来源。
        // allowStaticReuse=true 时优先重发缓存 IDR（桌面无变化则逐字节等价），
        // 条件不满足会自动回退到原有的强制抓帧路径，行为不变。
        pumpKeyframe(false, /*allowStaticReuse=*/true);
    });

    connect(session, &WebRtcSession::answerReceived, this, [this]() {
        // [IDR-ON-CONNECT] answer 生效后强制编码器重建产 IDR（带冷却防竞争）：
        // metaRTC 纯媒体推流不回调 connected，软编 hasIdr 后也不再主动产 IDR，
        // 新接入客户端必须立刻拿到关键帧，否则黑屏且无 PLI 可触发。
        const qint64 now = monoNowMs();
        if (now - lastReinitMs_ > 2000) {
            lastReinitMs_ = now;
            qInfo() << "WebRTC answer: forcing encoder reinit for IDR";
            reinitVideoEncoderForScale();
        }
        if (screenCapturer_)
            screenCapturer_->forceNextFrame();
    });

    connect(session, &WebRtcSession::failed, this, [this, clientId]() {
        stopWebRtcSession(clientId);
    });
    connect(session, &WebRtcSession::closed, this, [this, clientId]() {
        stopWebRtcSession(clientId);
    });
    connect(session, &WebRtcSession::hevcRejectedByPeer, this, [this, clientId]() {
        // 浏览器在 answer 里拒收了 H265（HEVC over WebRTC 不是所有浏览器都支持，
        // 典型是 Firefox）。编码器是所有客户端共享的，不能在这里单独给它换编码，
        // 否则会打断其它正在收 HEVC 的会话；所以只关掉本会话、告知前端降级，并记住
        // “本进程的 WebRTC 别再用 HEVC”——下一次 set_mode webrtc 会走正常的
        // H.264 覆盖路径（含编码器重建），前端那条降级链路本来就支持 H.264。
        webrtcHevcBlocked_ = true;
        qWarning() << "WebRTC: peer rejected H265 -> WebRTC pinned to H.264 for this process";
        stopWebRtcSession(clientId);
        wsServer_->sendJson(clientId, QJsonObject {
            { "type", "signal_state" },
            { "state", "failed" },
            { "reason", QStringLiteral("h265_unsupported") },
        });
    });

    // 信令与媒体均走内网（192.168.x），仅用 host 候选即可，无需外网 STUN。
    // 外网 STUN 在内网不可达时会拖长 ICE 收集，导致 12s 回退定时器先触发而误关会话。
    webrtcIceServers_ = QVector<QString>();

    if (!session->create(webrtcIceServers_, configFps_, webrtcCodec)) {
        // 必须显式告知失败：否则客户端停在"等待 offer"，只能等 12s 超时才降级。
        stopWebRtcSession(clientId);
        // WebRTC 未能建立时编码器无需再钉在 H.264：恢复用户 codec，否则
        // WS/MSE 回退流会一直用 H.264，用户配置被静默改写（重启进程才恢复）。
        if (codecOverriddenForWebRtc_) {
            configCodec_ = codecBeforeWebRtc_;
            codecOverriddenForWebRtc_ = false;
            qInfo() << "WebRTC create failed, restored user codec:" << codecToString(configCodec_);
            if (videoEncoder_ && screenCapturer_)
                reinitVideoEncoderForScale();
            else if (serviceMode_)
                sendVideoParamsToCaptureSource(QStringLiteral("video_on"));
        }
        wsServer_->sendJson(clientId, QJsonObject {
            { "type", "signal_state" },
            { "state", "failed" },
            { "reason", QStringLiteral("create_failed") },
        });
    }
}

void RDPServer::stopWebRtcSession(const QString& clientId)
{
    WebRtcSession* session = webrtcSessions_.take(clientId);
    webrtcExcluded_.remove(clientId);
    wsServer_->setMediaExcludedClients(webrtcExcluded_);
    if (session) {
        session->close();
        session->deleteLater();
    }
}

void RDPServer::sendWebRtcToClient(const QString& clientId, const QJsonObject& data)
{
    wsServer_->sendJson(clientId, data);
}

void RDPServer::onWebRtcMessage(const QString& clientId, const QJsonObject& msg)
{
    const QString type = msg["type"].toString();

    if (type == "signal_start") {
        startWebRtcSession(clientId);
    } else if (type == "signal_answer") {
        if (WebRtcSession* s = webrtcSessions_.value(clientId)) {
            s->handleAnswer(msg["sdp"].toString());
            // handleAnswer 失败会同步 emit failed → stopWebRtcSession 已删除会话
            // 并把该客户端移出 webrtcExcluded_。若不检查就继续执行下面的插入，
            // 客户端既没有 RTP 会话、又被 WS 广播永久排除 → 永久黑屏（只能重连）。
            if (!webrtcSessions_.contains(clientId)) {
                wsServer_->sendJson(clientId, QJsonObject {
                    { "type", "signal_state" },
                    { "state", "failed" },
                    { "reason", QStringLiteral("answer_failed") },
                });
                return;
            }
            // answer 生效即开始 RTP 推流（sendFrame 以 remoteSet_ 为门禁，
            // metaRTC 不回调 Connected），该客户端从 WS 媒体广播中排除，
            // 避免 RTP+WS 双流同时推送浪费带宽。
            webrtcExcluded_.insert(clientId);
            wsServer_->setMediaExcludedClients(webrtcExcluded_);
            // answer 生效后立即强制一帧关键帧：此时 sendFrame 门禁(remoteSet_)已满足，
            // 关键帧可被服务端推给 metaRTC 加密成 RTP，浏览器才能从 IDR 起解。
            // 这比依赖 connected 信号的时机更稳（避免关键帧在门禁就绪前被丢弃）。
            pumpKeyframe(false, /*allowStaticReuse=*/true);
        }
    } else if (type == "signal_ice") {
        if (WebRtcSession* s = webrtcSessions_.value(clientId))
            s->handleIce(msg["candidate"].toString(), msg["mid"].toString());
    } else if (type == "signal_close") {
        // 前端主动关闭 WebRTC（如切到 FFmpeg/图片模式）：销毁会话，
        // 避免残留会话使 webrtcBusy 恒真而阻塞后续 codec/hw_encode 切换
        stopWebRtcSession(clientId);
    }
}

#endif // USE_WEBRTC
