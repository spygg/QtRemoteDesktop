#include "helper_process.h"
#include "crashhandler.h"
#include "inputmanager.h"
#include "rdpserver.h"
#include "screencapturer.h"
#include "systeminfo.h"
// CodecType / HwEncodeMode 定义在独立小头里（不依赖 FFmpeg）；无 FFmpeg 的
// JPEG 回退构建也必须能编译 helper（否则 BUILD 在缺少 libavcodec 时直接失败，
// 与 CMake 宣称的 "FFmpeg not found (optional) — JPEG fallback mode" 矛盾）。
#include "codecdefs.h"
#ifdef USE_FFMPEG
#include "videoencoder.h"
#endif

#include <QApplication>
#include <QBuffer>
#include <QClipboard>
#include <QCryptographicHash>
#include <QDataStream>
#include <QDir>
#include <QFile>
#include <QGuiApplication>
#include <QImage>
#include <QIODevice>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMimeData>
#include <QProcess>
#include <QSaveFile>
#include <QStandardPaths>
#include <QThread>
#include <QTimer>
#include <QWebSocket>
#include <memory>

#ifdef _WIN32
#include <windows.h>
#include <tlhelp32.h>
#endif

#ifdef Q_OS_LINUX
// X11 EWMH 直发（显示桌面等）：helper 通过 Rdpserver/XdndMonitor PUBLIC 传递链接 X11
#include <X11/Xlib.h>
#include <X11/Xatom.h>
// X11 头会把 None 定义成 0L，与工程枚举（InhibitBackend::None 等）冲突
#ifdef None
#undef None
#endif
#endif

// X11 EWMH：向 root 窗口发送 _NET_SHOWING_DESKTOP client message（wmctrl -k on
// 同款协议），不依赖 wmctrl/xdotool 是否安装；GNOME/XFCE/KDE/openbox/LXDE 等支持
// EWMH 的 WM 都会响应。注意：GNOME(mutter) 不响应 XTEST 合成的 Super+D 快捷键
// （实测 xdotool key super+d 无效），因此该协议是 Linux 显示桌面最可靠的方式。
#ifdef Q_OS_LINUX
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

void logToFile(QtMsgType type, const QMessageLogContext& lg, const QString& msg);
void applyLogLevelFromArgs(int argc, char* argv[]);
// 日志角色后缀：helper 与 service 是两个进程，共用同一日志文件会互相交错
void setLogRoleSuffix(const QString& suffix);

#ifdef USE_FFMPEG
// 编码协议字符串 ↔ CodecType（helper 侧本地映射，与服务端 codecFromString/ToString 语义一致）
static CodecType helperCodecFromString(const QString& val)
{
    const QString s = val.toLower();
    if (s == QLatin1String("hevc") || s == QLatin1String("h265"))
        return CodecType::HEVC;
    if (s == QLatin1String("vp8"))  return CodecType::VP8;
    if (s == QLatin1String("vp9"))  return CodecType::VP9;
    if (s == QLatin1String("av1"))  return CodecType::AV1;
    if (s == QLatin1String("mpeg4")) return CodecType::MPEG4;
    return CodecType::H264;
}

static QString helperCodecToString(CodecType c)
{
    switch (c) {
    case CodecType::HEVC:  return QStringLiteral("hevc");
    case CodecType::VP8:   return QStringLiteral("vp8");
    case CodecType::VP9:   return QStringLiteral("vp9");
    case CodecType::AV1:   return QStringLiteral("av1");
    case CodecType::MPEG4: return QStringLiteral("mpeg4");
    default:               return QStringLiteral("h264");
    }
}
#endif

// [B7] helper 侧配置更新统一走 QSaveFile 原子写：server_config.json 由主服务进程
// 与 helper 两个进程共同读写，ReadWrite+resize(0)+write 的非原子序列在并发窗口
// 内会把对方刚写好的配置截断成半截 JSON（服务端下次启动读配置失败 → 全部回落默认）。
static void helperSaveConfigField(const QString& key, const QJsonValue& value)
{
    const QString cfgPath = QGuiApplication::applicationDirPath() + "/server_config.json";
    // QSaveFile 只接受 WriteOnly：open(ReadWrite) 带 ReadOnly 位会被 Qt 直接拒绝
    // （返回 false），旧实现因此从未真正写入过任何字段。旧内容必须先用 QFile 读。
    QJsonObject root;
    {
        QFile in(cfgPath);
        if (in.open(QIODevice::ReadOnly)) {
            const QByteArray raw = in.readAll();
            in.close();
            if (!raw.isEmpty()) {
                QJsonParseError err {};
                const QJsonDocument doc = QJsonDocument::fromJson(raw, &err);
                // 损坏的 JSON 不能当空对象写回，否则 users 等字段会被整体抹掉
                if (err.error != QJsonParseError::NoError) {
                    qWarning() << "Helper: config is not valid JSON, refusing to rewrite:"
                               << cfgPath << err.errorString();
                    return;
                }
                root = doc.object();
            }
        }
    }

    QSaveFile save(cfgPath);
    if (!save.open(QIODevice::WriteOnly)) {
        qWarning() << "Helper: cannot open config for update:" << cfgPath << save.errorString();
        return;
    }
    root[key] = value;
    save.write(QJsonDocument(root).toJson(QJsonDocument::Indented));
    if (!save.commit())
        qWarning() << "Helper: failed to commit config:" << cfgPath << save.errorString();
}

int HelperProcess::run(int argc, char* argv[])
{
#ifdef _WIN32
#if QT_VERSION < QT_VERSION_CHECK(6, 0, 0)
    QApplication::setAttribute(Qt::AA_EnableHighDpiScaling);
#endif
    QGuiApplication app(argc, argv);

    // helper 进程启用崩溃转储：main.cpp 的 platformMain 在 --helper 分支提前
    // return，breakpad 不会初始化，崩溃时将无 .dmp 可分析。
    Breakpad::CrashHandler::instance()->Init(QGuiApplication::applicationDirPath());

    QDir::setCurrent(QGuiApplication::applicationDirPath());

    QString logDir = QString("%1/logs").arg(QGuiApplication::applicationDirPath());
    QDir().mkpath(logDir);
    applyLogLevelFromArgs(argc, argv);
    setLogRoleSuffix(QStringLiteral("_helper"));
    qInstallMessageHandler(logToFile);

    int wsPort = 8081;
    bool useSsl = false;
    QFile cfg(QGuiApplication::applicationDirPath() + "/server_config.json");
    if (cfg.open(QIODevice::ReadOnly)) {
        QJsonDocument doc = QJsonDocument::fromJson(cfg.readAll());
        cfg.close();
        if (doc.isObject()) {
            QJsonObject root = doc.object();
            int httpPort = root.value("httpPort").toInt(8080);
            wsPort = httpPort + 1;
            useSsl = root.value("ssl").toBool(false);
        }
    }

    QString wsScheme = useSsl ? "wss" : "ws";

    // 声明顺序 = 析构的逆序（后构造者先析构），必须与数据流方向相反：
    //   capturer(采帧) -> compressor(编码线程) -> ws(发送)
    // ws 必须最先构造、最后析构：编码线程通过 Qt::QueuedConnection 把 jpegCompressed
    // 投递给 ws，若 ws 先析构而线程仍在 emit，QMetaObject::activate 会访问已析构的
    // QObjectData（d_ptr=0）-> 0xC0000005。让 compressor 最后构造，其析构
    // （shutdown + 等待线程退出）必定先于 ws 析构，从而消除该竞态。
    QWebSocket ws;
    ScreenCapturer capturer(nullptr);
    // compressor 改为堆分配：它在 worker 线程里通过 Qt::QueuedConnection 向 ws 投递
    // jpegCompressed，且曾作为主线程栈对象与 capturer 栈相邻。为使其脱离（ASLR 关闭时）
    // 固定的主线程栈地址、避免被捕获路径的越界写命中，改为堆分配。
    std::unique_ptr<JpegCompressor> compressor(new JpegCompressor(nullptr));

    // [B-6] 客户端配置的帧率缓存（config 消息更新），供切分辨率/视频编码重启使用
    int captureFps = 30;
    // 视频缩放档位与码率档位（非 FFmpeg 构建下仅作转发/记录用，无副作用）
    int videoScale = 100;
    int videoQualityLevel = 60;  // 视频码率档位（80/60/35/15，与服务端 configQuality_ 一致）

    // WS 写缓冲积压估算：Qt 5.7/5.9 的 QWebSocket 未公开 bytesToWrite()（5.12 才有），
    // 用「sendBinaryMessage 返回值累加 − bytesWritten 扣减」估算，与服务端
    // pendingBytes_ 同一套路。断线后缓冲被丢弃，重连时清零。
    // 必须声明在 USE_FFMPEG 之外：JPEG 上报路径（非 FFmpeg 构建）同样要用它做背压。
    qint64 wsBacklogBytes = 0;
    QObject::connect(&ws, &QWebSocket::bytesWritten, &app,
        [&](qint64 n) { wsBacklogBytes = qMax<qint64>(0, wsBacklogBytes - n); });

#ifdef USE_FFMPEG
    // ---- 服务模式 helper 侧视频编码 ----
    // 画面在用户会话捕获、编码也在 helper 完成，编码后的 H.264 以
    // 0x01(IDR)/0x02(P) 上报服务进程（服务进程只做分发/转发，不再二次编码，
    // 跨进程只传已压缩流，回环带宽仅几 Mbps，与 Linux 单进程方案同构）。
    bool videoActive = false;    // 视频模式：仅发 H.264，停 JPEG（省一半 CPU）
    bool videoCapable = VideoEncoder::isCodecEncodable(CodecType::H264);
    CodecType videoCodec = CodecType::H264;
    HwEncodeMode videoHwMode = HwEncodeMode::Auto;
    int videoBaseBitrate = 0;    // 基准码率（过载恢复用）
    std::unique_ptr<VideoEncoder> encoder(new VideoEncoder(nullptr));
#endif

    bool quitting = false;   // 退出标志：退出流程启动后禁止再访问/重连栈对象
    // [B-5] 前置声明：WS connected 处理器在重连后触发一次剪贴板重读
    QTimer* clipTimer = nullptr;

#ifdef USE_FFMPEG
    // ---- 采集喂帧泵 ----
    // 背景（2026-10-08 本地实测定位）：本工程裁剪版 libopenh264（FFmpeg 3.4.8）存在
    // **1 帧编码延迟**——encode(输入N) 的码流要等 encode(输入N+1) 才吐出来，
    // 即 N 个输入只产出 N-1 个输出（日志实测：第 5 帧输入无输出，第 6 帧输入才输出）。
    // 而 screencapturer 有变化检测，桌面静止（常见：锁屏、无人值守）时只有
    // forceNextFrame() 能逼出 1 帧，于是「request_keyframe → 单次 forceNextFrame」
    // 永远配不出输出，服务端等不到新 IDR → 新接入的浏览器一直黑屏。
    // 解决：任何"需要立刻出帧"的场景都连喂若干帧，把延迟配对补上。
    QTimer* capturePumpTimer = new QTimer(&app);
    int capturePumpLeft = 0;      // 还要"真实产出"多少帧（由 frameCaptured 递减）
    int capturePumpTicksLeft = 0; // 保险丝：定时器最多打这么多拍，防泵永远泵不完
    int capturePumpStall = 0;     // 连续多少拍没有新帧产出（采集停摆的判据）
    // 一次关键帧请求需要连喂几帧：libopenh264 的输出比输入滞后 1 帧，所以
    // 2 帧输入就能顶出第 1 帧（IDR）。原来喂 4 帧，多出的 2 帧纯粹是白抓
    // 全屏（XP 单帧 320ms ⇒ 每次关键帧请求白烧 0.65 秒单核）。
    static const int kKeyframePumpFrames = 2;
    // 采集停摆判定：连续这么多拍（80ms/拍 ≈ 1.9s）没有任何新帧就放弃等待。
    // 原来的保险丝是 n×25 拍（4 帧 = 8s），采集真停摆（锁屏/闸门关/驱动挂）时
    // 前端要黑 8 秒才等到重建后的 IDR。取值要 ≥ 补帧本身的耗时：补帧按 50%
    // 占空比走，单帧 320ms ⇒ 2 帧 ≈ 1.3s（16 拍），这里留到 24 拍。
    static const int kPumpStallTicks = 24;
    auto pumpCaptureFrames = [&](int n) {
        capturePumpLeft = qMax(capturePumpLeft, n);
        capturePumpStall = 0;
        capturePumpTicksLeft = qMax(capturePumpTicksLeft, n * 25);
        if (!capturePumpTimer->isActive())
            capturePumpTimer->start(80);
    };
    QObject::connect(capturePumpTimer, &QTimer::timeout, &app, [&]() {
        if (quitting || !videoActive) {
            capturePumpTimer->stop();
            capturePumpLeft = 0;
            capturePumpTicksLeft = 0;
            capturePumpStall = 0;
            return;
        }
        // 配额已满（真的出了足够多的帧）或保险丝烧断：立刻收工。
        // 必须在这里判，否则定时器会一直空转到保险丝用完（慢机上等于持续满速抓屏）。
        if (capturePumpLeft <= 0 || --capturePumpTicksLeft <= 0
                || ++capturePumpStall > kPumpStallTicks) {
            const bool stalled = capturePumpLeft > 0;
            capturePumpTimer->stop();
            capturePumpLeft = 0;
            capturePumpTicksLeft = 0;
            capturePumpStall = 0;
            if (stalled) {
                // 采集停摆：把配额清零，兜底定时器下一拍（≤400ms）就会看到
                // capturePumpLeft==0 并立即走重建，不必再等满 8 秒保险丝。
                qWarning() << "Helper: capture pump stalled, giving up after"
                           << kPumpStallTicks << "ticks";
            }
            return;
        }
        // 必须用 nudgeCapture()：forceNextFrame() 只在"从 idle 唤醒"那一次插即时
        // 抓帧，慢读回机器上 pace 已到 2×capAvg（XP 约 640ms），80ms 的泵拍子全部
        // 落空 → 泵窗口内一帧都不出 → 兜底定时器必然判"未产出 IDR"并重建编码器
        //（XP 实测一天 471 次重建、491 次 encoder init）。
        // paced=true：补帧也受 pace 约束（最小间隔 pace/2），否则泵会把抓屏拖成
        // 背靠背连跑，占空比从 33% 飙到 ~90%（每次关键帧请求 1 秒满核尖峰）。
        capturer.nudgeCapture(true);
    });

    // 上报视频能力（capture_caps）：服务端据此决定前端能否切视频模式。
    // video 表示"helper 当前是否具备 H.264 编码能力"（非"是否已激活"）。
    auto sendCaptureCaps = [&](const QString& reason) {
        if (ws.state() != QAbstractSocket::ConnectedState)
            return;
        QJsonObject caps;
        caps["type"] = "capture_caps";
        caps["video"] = videoCapable;
        caps["codec"] = helperCodecToString(videoCodec);
        caps["hwEncode"] = VideoEncoder::isHwAcceleratedAvailable(videoCodec);
        caps["width"] = capturer.width();
        caps["height"] = capturer.height();
        if (!reason.isEmpty())
            caps["reason"] = reason;
        ws.sendTextMessage(QString::fromUtf8(QJsonDocument(caps).toJson(QJsonDocument::Compact)));
    };

    // [KF-REPLAY] 静止桌面下的关键帧请求：缓存最近 IDR 及其"桌面内容版本"，
    // 再次被索要时若画面逐像素未变则直接重发缓存码流（不抓屏、不编码）。
    // 背景（2026-10-28 XP 实测）：服务模式下服务端把浏览器每个 PLI（静止桌面
    // ~6.3s 一次）转发成 request_keyframe，而本机 openh264 不认 flush+pict_type=I，
    // 800ms 兜底必然重建编码器并强抓 4 帧（XP 全屏 ~350ms/帧）→ 静止桌面 CPU 35%。
    // 复用判据与服务端 rdpserver.cpp 的 PLI-REUSE 一致：contentChangeCount() 不变
    // ⇔ 自该 IDR 起桌面未变 ⇔ 重发等价于重新编码。
    // 版本号只能记在"我们自己发起"的 IDR 上（pendingKfEpoch）：编码有 1 帧延迟，
    // 自然 GOP 出的 IDR 对应更早的输入帧，用出帧时刻的计数会把旧画面标成新版本。
    // 编码器每次（重）建都必须清缓存：旧实例的 IDR 带着旧 SPS/PPS 与旧分辨率。
    QByteArray lastIdrData;
    qint64 lastVideoTs = 0;
    quint64 lastIdrEpoch = 0;
    bool lastIdrEpochValid = false;
    quint64 pendingKfEpoch = 0;
    bool pendingKfEpochValid = false;
    qint64 lastIdrReplayMs = 0;
    quint64 idrReplayCount = 0;
    auto dropCachedIdr = [&]() {
        lastIdrData.clear();
        lastIdrEpochValid = false;
        pendingKfEpochValid = false;
    };

    // 按当前 codec/hw/fps/scale/画质重建编码器（视频模式中参数变化时调用）
    auto reinitVideoEncoder = [&]() {
        if (!videoActive || !encoder)
            return;
        int sw = capturer.width(), sh = capturer.height();
        if (sw <= 0 || sh <= 0)
            return;
        int ew = sw, eh = sh;
        if (videoScale >= 10 && videoScale < 100) {
            ew = sw * videoScale / 100;
            eh = sh * videoScale / 100;
        }
        videoBaseBitrate = VideoEncoder::estimateBitrate(videoCodec, ew, eh, captureFps, videoQualityLevel);
        encoder->shutdown();
        dropCachedIdr();
        if (!encoder->initialize(videoCodec, sw, sh, ew, eh, captureFps, videoBaseBitrate, videoHwMode))
            qWarning() << "Helper: video encoder re-init failed (config change)";
    };

    // 关键帧请求兜底：先走 requestKeyframe（软编快路径），超时未见关键帧就重建编码器。
    // 依据（2026-10-08 实测）：videoencoder.cpp:632 的注释已说明"旧版 ffmpeg 不认
    // frame->pict_type"，本机 libopenh264 实测吐出的仍是 P 帧（562B），只有重建
    // 编码器才能可靠拿到新 IDR。服务端 pumpKeyframe 的"单进程路径"也是同样兜底
    // （hasIdr 仍未见新 IDR 时重建），helper 侧此前缺这一步 → 新接入浏览器黑屏。
    bool kfRequestPending = false;
    // [KF-SOFT-PATH] 该编码器实例是否认 requestKeyframe()。libopenh264（FFmpeg 3.4
    // 内置软编）实测忽略 pict_type=I：requestKeyframe() 之后喂进去的帧吐出来仍是
    // P 帧，只有重建编码器才能可靠拿到新 IDR。首次兜底失败即置 false，之后的关键帧
    // 请求直接走"重建 + 喂帧"快路径，省掉每轮"2 帧白抓 + 800ms 空等 + 末尾再重建"
    //（XP 实测一次显式关键帧请求从 ~2.7s 降到 ~1.1s，且不再有第二次满速抓屏）。
    // 换 codec / 重进视频模式时复位（新实例可能认）。
    bool encoderHonorsRequestKf = true;
    QTimer* kfFallbackTimer = new QTimer(&app);
    kfFallbackTimer->setSingleShot(true);
    QObject::connect(kfFallbackTimer, &QTimer::timeout, &app, [&]() {
        if (!kfRequestPending)
            return;
        // 喂帧是 80ms 一拍的定时泵，慢机（XP 全屏实测 ~320ms/帧）跑完要 1s+，
        // 此时兜底先到点会把"还在喂帧"误判成"没出 IDR"，白重建一次编码器（新 IDR
        // 与旧码流交错，纯粹烧 CPU）。泵没跑完就再等一轮，等泵结束仍无 IDR 才重建。
        if (capturePumpLeft > 0) {
            kfFallbackTimer->start(400);
            return;
        }
        kfRequestPending = false;
        if (quitting || !videoActive || !encoder)
            return;
        qWarning() << "Helper: request_keyframe 未产出 IDR，重建编码器强制关键帧"
                   << "(软路无效，后续直接重建)";
        encoderHonorsRequestKf = false;
        reinitVideoEncoder();
        // 重建后的首帧 IDR 也要记录内容版本，否则 KF-REPLAY 缓存永远不生效
        // （reinitVideoEncoder 里已清空缓存）。
        pendingKfEpoch = capturer.contentChangeCount();
        pendingKfEpochValid = true;
        pumpCaptureFrames(kKeyframePumpFrames);
    });

    // [ADAPT] 慢机自适应配置：按实测单帧采集耗时（capturer.avgCaptureMs）分级下调
    // 帧率/缩放比、提高节流倍数。目标是让 VMware 上的 XP 这类"读屏 320ms/帧、单核"
    // 的机器自动落到能交互的参数，而不是拿默认 15fps/75% 硬撑（实测只有 1.2fps 却
    // 占单核 43%）。只在视频模式评估、最多 3 轮，用户之后在页面改的参数随时覆盖。
    QTimer* adaptTimer = new QTimer(&app);
    adaptTimer->setInterval(4000);
    int adaptRounds = 0;
    bool adaptDecided = false;
    QObject::connect(adaptTimer, &QTimer::timeout, &app, [&]() {
        if (quitting || !videoActive)
            return;
        const int avg = capturer.avgCaptureMs();
        if (avg <= 0)
            return;                       // 还没有有效样本
        if (++adaptRounds < 2)
            return;                       // 至少 2 个窗口（≈8s），避开启动抖动
        adaptDecided = true;
        adaptTimer->stop();

        int wantFps = captureFps;
        int wantScale = videoScale;
        double wantPace = 2.0;
        if (avg < 60) {
            // 读回正常（真机/有加速的 VM）：保持用户配置
        } else if (avg < 200) {
            wantFps = qMin(captureFps, 12);
        } else if (avg < 400) {
            wantFps = qMin(captureFps, 10);
            wantScale = qMin(videoScale, 60);
            wantPace = 3.0;
        } else {
            wantFps = qMin(captureFps, 8);
            wantScale = qMin(videoScale, 50);
            wantPace = 4.0;
        }
        if (wantFps == captureFps && wantScale == videoScale && wantPace <= 2.0)
            return;

        qWarning() << "Helper: slow capture detected, avgCapMs=" << avg
                   << "adapting fps" << captureFps << "->" << wantFps
                   << "scale" << videoScale << "->" << wantScale
                   << "paceFactor ->" << wantPace;
        captureFps = wantFps;
        videoScale = wantScale;
        capturer.setFps(captureFps);
        capturer.setPaceFactor(wantPace);
        reinitVideoEncoder();
        // 把实际生效的参数告诉服务端（页面上的下拉框仍显示用户选的值，但日志与
        // capture_caps 能看出实际配置，避免"设了 15fps 却只有 8fps"的困惑）。
        QJsonObject adapt;
        adapt["type"] = "capture_adapt";
        adapt["avgCapMs"] = avg;
        adapt["fps"] = captureFps;
        adapt["scale"] = videoScale;
        adapt["paceFactor"] = wantPace;
        ws.sendTextMessage(QString::fromUtf8(QJsonDocument(adapt).toJson(QJsonDocument::Compact)));
    });

    // 进入视频模式：按服务端下发的 config 建立编码器，成功后开始上报 H.264
    auto startVideoMode = [&]() {
        int sw = capturer.width(), sh = capturer.height();
        if (sw <= 0 || sh <= 0) {
            qWarning() << "Helper: cannot start video mode, screen size unavailable";
            videoCapable = false;
            sendCaptureCaps(QStringLiteral("no_screen"));
            return;
        }
        int ew = sw, eh = sh;
        if (videoScale >= 10 && videoScale < 100) {
            ew = sw * videoScale / 100;
            eh = sh * videoScale / 100;
        }
        videoBaseBitrate = VideoEncoder::estimateBitrate(videoCodec, ew, eh, captureFps, videoQualityLevel);
        encoder->shutdown();
        dropCachedIdr();
        if (!encoder->initialize(videoCodec, sw, sh, ew, eh, captureFps, videoBaseBitrate, videoHwMode)) {
            // 当前 codec 不支持（如软编无 HEVC）→ 回退 H.264 重试，保证视频模式可用
            qWarning() << "Helper: codec init failed, falling back to H.264";
            videoCodec = CodecType::H264;
            videoBaseBitrate = VideoEncoder::estimateBitrate(videoCodec, ew, eh, captureFps, videoQualityLevel);
            if (!encoder->initialize(videoCodec, sw, sh, ew, eh, captureFps, videoBaseBitrate, videoHwMode)) {
                qWarning() << "Helper: video encoder init failed, staying in image mode";
                videoCapable = false;
                videoActive = false;
                sendCaptureCaps(QStringLiteral("encoder_init_failed"));
                return;
            }
        }
        videoCapable = true;
        videoActive = true;
        qInfo() << "Helper: video mode ON" << ew << "x" << eh << "@" << captureFps
                << "codec" << (videoCodec == CodecType::H264 ? "h264" : "hevc")
                << "bitrate" << videoBaseBitrate;
        sendCaptureCaps(QString());
        // 新实例可能认 requestKeyframe（换 codec / 换硬件后再试一次软路）
        encoderHonorsRequestKf = true;
        // 立即连喂几帧：新编码器首帧即 IDR，且要喂够帧数才能突破 1 帧输出延迟，
        // 否则接入后要等桌面自然变化才有画面（静止桌面会长时间黑屏）。
        pumpCaptureFrames(kKeyframePumpFrames);
        // 开始慢机自适应评估（每 4s 一轮，最多 3 轮后停）
        if (!adaptDecided) {
            adaptRounds = 0;
            adaptTimer->start();
        }
    };

    // 退出视频模式：停编码器、恢复 JPEG 上报
    auto stopVideoMode = [&]() {
        capturePumpTimer->stop();
        capturePumpLeft = 0;
        capturePumpTicksLeft = 0;
        capturePumpStall = 0;
        kfRequestPending = false;
        kfFallbackTimer->stop();
        adaptTimer->stop();
        if (encoder)
            encoder->shutdown();
        dropCachedIdr();
        videoActive = false;
        qInfo() << "Helper: video mode OFF, resumed JPEG";
    };

    // [KF-REPLAY] 缓存见上（lastIdrData/lastIdrEpoch*）。
    // 编码帧 → 服务端：[1字节类型(0x01=IDR/0x02=P)][u32 长度][i64 时间戳][数据]
    // （与服务端→浏览器 13 字节头一致，服务端可直接复用 onEncodedFrame 分发）
    qint64 droppedVideoFrames = 0;
    QObject::connect(encoder.get(), &VideoEncoder::encodedFrame, &app,
        [&](const QByteArray& data, bool keyframe, qint64 ts) {
            if (keyframe) {
                // 关键帧已产出 → 撤销 request_keyframe 的重建兜底
                kfRequestPending = false;
                if (kfFallbackTimer)
                    kfFallbackTimer->stop();
            }
            if (quitting || !videoActive)
                return;
            if (ws.state() != QAbstractSocket::ConnectedState)
                return;
            if (keyframe) {
                lastIdrData = data;
                if (pendingKfEpochValid) {
                    lastIdrEpoch = pendingKfEpoch;
                    lastIdrEpochValid = true;
                    pendingKfEpochValid = false;
                } else {
                    lastIdrEpochValid = false; // 自然 GOP 关键帧：来源帧版本不可知
                }
            }
            lastVideoTs = ts;
            // 背压：服务端消费不过来时丢弃**非关键帧**。QWebSocket 写缓冲无上限，
            // 一直塞会让内存暴涨、延迟线性增长（画面越看越滞后）。丢帧是可接受的：
            // 关键帧仍然送，浏览器随时能重新同步。
            // 阈值=延迟上限：积压阈值 ÷ 链路吞吐 = 常驻延迟。4MB 在 2~4MB/s
            // 链路上是 1~2 秒；512KB 亚秒级（与服务端 kMaxSocketBacklogBytes 一致）。
            static const qint64 kMediaBacklogLimit = 512 * 1024;
            if (!keyframe && wsBacklogBytes > kMediaBacklogLimit) {
                if ((++droppedVideoFrames % 60) == 1)
                    qWarning() << "Helper: WS backlog" << wsBacklogBytes
                               << "bytes - dropping video frames (total" << droppedVideoFrames << ")";
                return;
            }
            QByteArray packet;
            QDataStream stream(&packet, QIODevice::WriteOnly);
            stream.setByteOrder(QDataStream::BigEndian);
            stream << quint8(keyframe ? 0x01 : 0x02);
            stream << quint32(data.size());
            stream << qint64(ts);
            packet.append(data);
            wsBacklogBytes += qMax<qint64>(0, ws.sendBinaryMessage(packet));
        });
    // SPS/PPS：编码器 init 产出 extradata 时上报，服务端缓存并转发给浏览器
    QObject::connect(encoder.get(), &VideoEncoder::codecConfigChanged, &app,
        [&](const QByteArray& extra) {
            if (quitting)
                return;
            if (ws.state() != QAbstractSocket::ConnectedState)
                return;
            QJsonObject m;
            m["type"] = "codec_config";
            m["extradata"] = QString::fromLatin1(extra.toBase64());
            ws.sendTextMessage(QString::fromUtf8(QJsonDocument(m).toJson(QJsonDocument::Compact)));
        });
    // 过载时降码率，恢复后回到基准码率
    QObject::connect(encoder.get(), &VideoEncoder::encoderOverload, &app,
        [&](bool overloaded) {
            if (!encoder)
                return;
            if (overloaded) {
                int reduced = qMax(150000, static_cast<int>(encoder->currentBitrate() * 0.6));
                encoder->setBitrate(reduced);
                qWarning() << "Helper: video encoder overload, reducing bitrate to" << reduced;
            } else if (videoBaseBitrate > 0) {
                encoder->setBitrate(videoBaseBitrate);
                qInfo() << "Helper: video encoder recovered, restoring bitrate to" << videoBaseBitrate;
            }
        });
    // [N7] MPP 硬编运行中被证明不可用（编码线程经队列信号通知）：主线程重建为软编。
    // 缺这条接线时 helper 会一直停在"编码器损坏但无人重建"→ 视频模式永久无帧，
    // 只能等 request_keyframe 的兜底重建（静止桌面上可能永远等不到）。
    QObject::connect(encoder.get(), &VideoEncoder::reinitRequired, &app, [&]() {
        if (quitting || !videoActive || !encoder)
            return;
        qWarning() << "Helper: encoder reported reinitRequired (MPP unavailable), rebuilding as software encoder";
        reinitVideoEncoder();
        pumpCaptureFrames(kKeyframePumpFrames);
    });
#endif // USE_FFMPEG

    // 在 connected 处理器之前声明：重连时需要补发当前锁屏状态
    bool locked = false;
    // 输入桌面轮询（OpenInputDesktop + 桌面名）的结论。Windows 上它才是"锁没锁"的权威
    // 依据，抓屏侧的"全黑帧"只是启发式 —— 见下面 screenLocked 处理器的注释。
    bool desktopPollLocked = false;
    // 最近一次轮询看到的输入桌面名（原样大小写，锁屏时如 "Winlogon"/"Welcome"）。
    // 服务进程在 Session 0/私有窗口站内 OpenInputDesktop 会失败（实测 XP 返回错误 1），
    // 它自己无从得知登录 UI 在哪个桌面；锁屏事件里把这个名字带给服务端，
    // secure-input 进程才能投到正确的桌面上（投错 = 注入的按键石沉大海）。
    QString inputDesktopName;

    // 采集闸门：只有"服务端说有客户端"且"输入桌面未锁"才真的抓屏。
    // 锁屏期间继续抓屏在 XP 上是纯烧 CPU（实测 172.24.52.172 切到 Winlogon 后）：
    // helper 每个 tick 仍做一次 2310x1398 的 BitBlt，日志里 capAvg=207.88 /
    // capMax=310.47 而 emits=0 —— 一个帧都发不出去（前端此时只显示锁屏遮罩），
    // 却把这台弱机的 CPU 占满。解锁时闸门自动打开，不需要额外信令。
    bool serverWantsCapture = false;
    // 初值 = true：启动时 capturer.start(30) 已经在跑，闸门必须从"开着"的状态开始记账，
    // 否则服务端第一条 pause（无客户端）会与状态相同而被跳过，空闲采集永远停不下来。
    bool captureGateOpen = true;
    auto applyCaptureGate = [&]() {
        const bool want = serverWantsCapture && !desktopPollLocked;
        if (want == captureGateOpen)
            return;
        captureGateOpen = want;
        qInfo() << "Helper: capture gate" << (want ? "open" : "closed")
                << "serverWants =" << serverWantsCapture << "locked =" << desktopPollLocked;
        if (want)
            capturer.resume();
        else
            capturer.suspend();
#ifdef USE_FFMPEG
        // 闸门从关到开 = 锁屏期间码流断了一阵：必须让编码器下一个 IDR 出来，
        // 否则前端 MSE 拿锁屏前那个旧 IDR 去解新 P 帧，表现是解锁后画面冻住/花屏。
        if (want && videoActive && encoder)
            encoder->requestKeyframe();
#endif
    };

    QObject::connect(&ws, &QWebSocket::connected, &app, [&]() {
        qInfo() << "Helper: connected to service WS successfully";
        wsBacklogBytes = 0; // 断线期间写缓冲被丢弃，重连后从零开始记账
        SetThreadExecutionState(ES_CONTINUOUS | ES_SYSTEM_REQUIRED | ES_DISPLAY_REQUIRED);
        // [P2] 每次连接都上报：服务端内存态在 helper 重连期间可能已变（或服务重启），
        // 只发一次会让重连后的会话拿不到尺寸 → 鼠标坐标映射错位。
        QJsonObject info;
        info["type"] = "screen_info";
        info["width"] = capturer.width();
        info["height"] = capturer.height();
        ws.sendTextMessage(QString::fromUtf8(QJsonDocument(info).toJson(QJsonDocument::Compact)));
#ifdef USE_FFMPEG
        // 上报视频能力：服务端据此决定前端能否切视频模式
        sendCaptureCaps(QString());
#endif
        // 服务端重启或 helper 重连后，服务端内存中的 screenLocked_ 已被重置，
        // 而 desktopCheckTimer 只在状态"变化"时上报。若重连时本就处于锁屏状态，
        // locked 仍是 true → 永远不会再发 → 服务端不拉起 secure-input，
        // Windows 远程永远输不进登录密码。每次连接补发一次当前状态。
        {
            QJsonObject lockMsg;
            lockMsg["type"] = "screen_locked";
            lockMsg["locked"] = locked;
            lockMsg["hint"] = locked ? QString::fromUtf8("Windows 锁屏在安全桌面上，点「解锁」由服务端注入密码") : QString();
            if (locked && !inputDesktopName.isEmpty())
                lockMsg["desktop"] = inputDesktopName;
            ws.sendTextMessage(QString::fromUtf8(QJsonDocument(lockMsg).toJson(QJsonDocument::Compact)));
        }
        // [B-5] 重连后重读当前剪贴板：断线窗口内复制的内容此前因未发送成功
        // 不记账（回声抑制只在发送成功后记录），此时补传。
        if (clipTimer)
            clipTimer->start();
    });
    QObject::connect(&ws, QOverload<QAbstractSocket::SocketError>::of(&QWebSocket::error),
        &app, [&](QAbstractSocket::SocketError err) {
            qWarning() << "Helper: WS error" << err << ws.errorString();
            if (quitting)
                return;
            // 连接失败（服务尚未开始监听/端口切换）时某些平台只发 error、不发
            // disconnected → 旧实现没有重连，helper 永久停在"采集了但没人收"的状态。
            if (err == QAbstractSocket::ConnectionRefusedError
                || err == QAbstractSocket::HostNotFoundError
                || err == QAbstractSocket::SocketTimeoutError
                || err == QAbstractSocket::RemoteHostClosedError) {
                QTimer::singleShot(3000, &app, [&]() {
                    if (quitting || ws.state() != QAbstractSocket::UnconnectedState)
                        return;
                    ws.open(QUrl(QString("%1://127.0.0.1:%2/capture").arg(wsScheme).arg(wsPort)));
                });
            }
        });
    QObject::connect(&ws, &QWebSocket::disconnected, &app, [&]() {
        if (quitting) return;
        qWarning() << "Helper WS disconnected (helper may have crashed), retrying in 3s...";
        SetThreadExecutionState(ES_CONTINUOUS);
        // 必须绑定 context（&app）：无 context 的 singleShot 定时器不随对象析构取消，
        // helper 在 3s 内退出时，lambda 会解引用已销毁的 ws/quitting（use-after-scope），
        // 而 `if (quitting) return` 里的 quitting 本身也是悬垂引用，保护形同虚设。
        QTimer::singleShot(3000, &app, [&]() {
            // 与 error 路径同构的守卫：若 3s 内已有其它路径发起重连（或 ws 尚未
            // 回到 Unconnected），对非空闲状态的 socket 再 open() 会被 Qt 断言/忽略，
            // 且两条重连线会互相竞争。
            if (quitting || ws.state() != QAbstractSocket::UnconnectedState)
                return;
            ws.open(QUrl(QString("%1://127.0.0.1:%2/capture").arg(wsScheme).arg(wsPort)));
        });
    });
    QObject::connect(&ws, &QWebSocket::sslErrors, &app, [&](const QList<QSslError>& errors) {
        for (const auto& err : errors)
            qWarning() << "Helper: SSL error" << err.errorString();
        ws.ignoreSslErrors();
    });
    // [N2] 30s 保活：服务端 180s 静默清扫无法区分"合法静默"（pause/静止桌面无帧）
    // 与真正半开的挂死连接，会把健康的采集源误杀 → 每次误杀都是断连重连周期，
    // 观看端表现为无画面数秒。事件循环存活即心跳照常；进程挂死则心跳停止，
    // 清扫的半开检测语义仍然成立。
    QTimer* captureHbTimer = new QTimer(&app);
    captureHbTimer->setInterval(30000);
    QObject::connect(captureHbTimer, &QTimer::timeout, &app, [&]() {
        if (quitting || ws.state() != QAbstractSocket::ConnectedState)
            return;
        QJsonObject hb;
        hb["type"] = "capture_hb";
        ws.sendTextMessage(QString::fromUtf8(QJsonDocument(hb).toJson(QJsonDocument::Compact)));
    });
    captureHbTimer->start();
    ws.open(QUrl(QString("%1://127.0.0.1:%2/capture").arg(wsScheme).arg(wsPort)));

    QObject::connect(&capturer, &ScreenCapturer::frameCaptured,
        &app, [&](const QImage& frame) {
            // 退出流程启动后（栈对象已开始析构）不再访问 compressor，
            // 避免队列中延迟投递的事件访问已析构对象
            if (quitting) return;
            // 喂帧泵按"真实出帧"计数：只有真的产出一帧才算喂到，80ms 拍子本身
            // 不代表任何一帧（慢读回机器上一个拍子连 BitBlt 都跑不完）。
            if (capturePumpLeft > 0)
                --capturePumpLeft;
            capturePumpStall = 0;   // 有新帧 ⇒ 采集没停摆
#ifdef USE_FFMPEG
            if (videoActive) {
                // 视频模式：走编码器，不再喂 JPEG 压缩线程（省一半 CPU）
                if (encoder)
                    encoder->encode(frame);
                return;
            }
#endif
            compressor->enqueue(frame);
        });
    qint64 droppedJpegFrames = 0;
    QObject::connect(compressor.get(), &JpegCompressor::jpegCompressed,
        &ws, [&](const QByteArray& jpegData) {
            if (quitting) return;
            if (ws.state() != QAbstractSocket::ConnectedState)
                return;
            // 背压：图片模式下同样丢弃积压帧（下一帧很快会到，丢帧只损失一帧画面，
            // 不丢帧则内存与延迟双爆）。512KB=亚秒级延迟上限，与视频路径一致。
            static const qint64 kMediaBacklogLimit = 512 * 1024;
            if (wsBacklogBytes > kMediaBacklogLimit) {
                if ((++droppedJpegFrames % 60) == 1)
                    qWarning() << "Helper: WS backlog" << wsBacklogBytes
                               << "bytes - dropping JPEG frames (total" << droppedJpegFrames << ")";
                return;
            }
            QByteArray packet;
            QDataStream stream(&packet, QIODevice::WriteOnly);
            stream.setByteOrder(QDataStream::BigEndian);
            stream << quint8(0x03);
            stream << quint32(jpegData.size());
            packet.append(jpegData);
            wsBacklogBytes += qMax<qint64>(0, ws.sendBinaryMessage(packet));
        }, Qt::QueuedConnection);

    // OS 判定统一走 SystemInfo：Vista+ 用 kernel32 导出探测，不受"以兼容模式
    // 运行 Windows XP"标记伪造（那台 Win7 上 RtlGetVersion 谎报 5.1.2600）。
    const WinOsInfo& osInfo = winOsInfo();
    const bool isWinXP = !osInfo.vistaPlus;                                  // 5.x：XP / 2003
    const bool isWin7 = osInfo.vistaPlus && osInfo.major == 6 && osInfo.minor == 1;
    qInfo() << "Helper: starting desktop polling, isWin7 =" << isWin7 << "isWinXP =" << isWinXP
            << "os" << QStringLiteral("%1.%2.%3").arg(osInfo.major).arg(osInfo.minor).arg(osInfo.build)
            << QStringLiteral("src=%1").arg(QLatin1String(osInfo.source));

    InputManager inputMgr;
    // 输入坐标归一化基准与上报前端的 screen_info 一致（高 DPI 缩放）
    inputMgr.setScreenSize(capturer.width(), capturer.height());

    // 共享剪贴板：监听用户会话剪贴板变化，去抖后上报给服务端（服务端广播给所有客户端）
    QString lastClipText;
    // [N2] 图片剪贴板：与 service xclip 通道能力对齐。回声抑制用图像内容签名
    // （写入时记录，dataChanged 后重算比对—— setImage 回读再编码 PNG 字节可能不同，
    //  但像素内容一致）。
    QByteArray lastClipImageSig;
    auto clipImageSig = [](const QImage& img) -> QByteArray {
        if (img.isNull())
            return QByteArray();
        QCryptographicHash h(QCryptographicHash::Md5);
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
        h.addData(reinterpret_cast<const char*>(img.constBits()),
                  qsizetype(img.sizeInBytes()));
#else
        // Qt5：sizeInBytes() 为 qint64（5.10+），byteCount() 为 int（<5.10，Qt6 已删）
#if QT_VERSION >= QT_VERSION_CHECK(5, 10, 0)
        h.addData(reinterpret_cast<const char*>(img.constBits()),
                  int(img.sizeInBytes()));
#else
        h.addData(reinterpret_cast<const char*>(img.constBits()),
                  int(img.byteCount()));
#endif
#endif
        return h.result();
    };
    clipTimer = new QTimer(&app); // [B-5] 赋值给前置声明的指针
    clipTimer->setSingleShot(true);
    clipTimer->setInterval(150);
    QObject::connect(QGuiApplication::clipboard(), &QClipboard::dataChanged, &app, [&]() {
        clipTimer->start();
    });
    QObject::connect(clipTimer, &QTimer::timeout, &app, [&]() {
        // [N2] 图片优先：hasImage 时上报 image/png（base64），与前端/service 协议一致
        const QMimeData* md = QGuiApplication::clipboard()->mimeData();
        if (md && md->hasImage()) {
            QImage img = qvariant_cast<QImage>(md->imageData());
            if (!img.isNull()) {
                const QByteArray sig = clipImageSig(img);
                if (sig == lastClipImageSig)
                    return;  // 自己写入的回声
                // [B-5] 断线时不记账：签名只在发送成功后记录，否则重连后该内容
                // 会被误判为回声而永久丢失。
                if (ws.state() != QAbstractSocket::ConnectedState)
                    return;
                QBuffer buf;
                buf.open(QIODevice::WriteOnly);
                if (img.save(&buf, "PNG") && !buf.data().isEmpty()) {
                    QJsonObject msg;
                    msg["type"] = "clipboard";
                    msg["mime"] = "image/png";
                    msg["data"] = QString::fromLatin1(buf.data().toBase64());
                    ws.sendTextMessage(QString::fromUtf8(QJsonDocument(msg).toJson(QJsonDocument::Compact)));
                    lastClipImageSig = sig;
                    lastClipText.clear();
                    return;
                }
            }
        }
        QString t = QGuiApplication::clipboard()->text();
        if (t.isEmpty() || t == lastClipText)
            return;
        // [B-5] 断线时不记账，重连后由 connected 处理器补传
        if (ws.state() != QAbstractSocket::ConnectedState)
            return;
        lastClipText = t;
        lastClipImageSig.clear();
        QJsonObject msg;
        msg["type"] = "clipboard";
        msg["text"] = t;
        ws.sendTextMessage(QString::fromUtf8(QJsonDocument(msg).toJson(QJsonDocument::Compact)));
    });

    QObject::connect(&ws, &QWebSocket::textMessageReceived, &app,
        [&](const QString& msg) {
            QJsonParseError err;
            QJsonDocument doc = QJsonDocument::fromJson(msg.toUtf8(), &err);
            if (err.error != QJsonParseError::NoError || !doc.isObject())
                return;
            QJsonObject obj = doc.object();
            QString type = obj["type"].toString();

            if (type == "capture_control") {
                QString action = obj["action"].toString();
                if (action == "pause") {
                    qInfo() << "Helper: capture pause requested";
                    serverWantsCapture = false;
                    applyCaptureGate();
                } else if (action == "resume") {
                    qInfo() << "Helper: capture resume requested";
                    serverWantsCapture = true;
                    // 锁屏中这条 resume 不会真的打开采集，解锁时闸门自动打开。
                    // 不打出来的话，日志里只有一条没人应答的 resume，排查时像是丢了。
                    if (desktopPollLocked)
                        qInfo() << "Helper: still locked, capture stays paused until unlock";
                    const bool gateWasOpen = captureGateOpen;
                    applyCaptureGate();
                    // [IDLE-WAKE] 已有客户端在观看时，第二个客户端接入不会改变闸门
                    // 状态（applyCaptureGate 直接返回），但 idle 降频下它要等下一帧
                    // （最长 ~5s），静止桌面甚至要等到画面变化才出帧 → 新客户端黑屏。
                    // 这里补一次强制帧：resume 只在客户端接入/唤醒时下发，频率很低。
                    if (gateWasOpen && captureGateOpen)
                        capturer.forceNextFrame();
                    // 客户端接入：上报当前剪贴板文本，让新客户端拿到初始状态
                    QString t = QGuiApplication::clipboard()->text();
                    if (!t.isEmpty()) {
                        QJsonObject c;
                        c["type"] = "clipboard";
                        c["text"] = t;
                        ws.sendTextMessage(QString::fromUtf8(QJsonDocument(c).toJson(QJsonDocument::Compact)));
                    }
                }
#ifdef USE_FFMPEG
                else if (action == "video_on") {
                    qInfo() << "Helper: video mode ON requested";
                    startVideoMode();
                } else if (action == "video_off") {
                    qInfo() << "Helper: video mode OFF requested";
                    stopVideoMode();
                }
#endif
                return;
            }

#ifdef USE_FFMPEG
            if (type == "request_keyframe") {
                // 锁屏中不配合抓帧：这几帧全屏抓取在 XP 上要 200~300ms/tick，而前端
                // 此刻只显示锁屏遮罩，一个像素都不会被看到（见 applyCaptureGate 注释）。
                if (desktopPollLocked)
                    return;
                if (videoActive && encoder) {
                    // [KF-REPLAY] 浏览器 PLI（服务端标记 reuse_ok）且自缓存 IDR 起桌面
                    // 逐像素未变：直接重发缓存码流，不抓屏、不碰编码器。
                    // 这是静止桌面下 35% CPU 的来源——本机 openh264 不认
                    // requestKeyframe()，每次都退到 800ms 兜底重建 + 强抓 4 帧。
                    // 显式请求（新客户端接入/前端自愈，reuse_ok=false）不走这里：
                    // 它要的是"从此刻起可解码"，缓存 IDR 的时间戳可能早于前端
                    // 已缓冲位置，重发会被 MSE 当过期帧丢掉。
                    const bool reuseOk = obj["reuse_ok"].toBool();                    if (reuseOk && lastIdrEpochValid && !lastIdrData.isEmpty()
                            && capturer.contentChangeCount() == lastIdrEpoch) {
                        const qint64 now = QDateTime::currentMSecsSinceEpoch();
                        // PLI 实测成对到达（同一毫秒两次），冷却内忽略重复请求。
                        if (now - lastIdrReplayMs < 250)
                            return;
                        if (ws.state() != QAbstractSocket::ConnectedState)
                            return;
                        QByteArray packet;
                        QDataStream stream(&packet, QIODevice::WriteOnly);
                        stream.setByteOrder(QDataStream::BigEndian);
                        // 时间戳取"下一帧"位置：MSE 要求单调递增，直接复用旧 ts 会倒退。
                        stream << quint8(0x01);
                        stream << quint32(lastIdrData.size());
                        stream << qint64(lastVideoTs + qMax(1, 1000 / qMax(1, captureFps)));
                        packet.append(lastIdrData);
                        wsBacklogBytes += qMax<qint64>(0, ws.sendBinaryMessage(packet));
                        lastIdrReplayMs = now;
                        if ((++idrReplayCount % 10) == 1)
                            qInfo() << "Helper: replayed cached IDR (static desktop, no capture/encode)"
                                    << "reuse=" << idrReplayCount;
                        return;
                    }
                    // 服务端请求新 IDR（新客户端接入 / 解码器自愈）：强制下一帧为关键帧，
                    // 并连喂几帧。注意不能只喂 1 帧：libopenh264 有 1 帧输出延迟，
                    // 单帧输入配不出码流（详见 pumpCaptureFrames 处说明）。
                    pendingKfEpoch = capturer.contentChangeCount();
                    pendingKfEpochValid = true;
                    if (encoderHonorsRequestKf) {
                        // 软路：先试 requestKeyframe（硬件编码器认这一套，能省一次
                        // 编码器重建）。失败由 800ms 兜底接管并永久关闭软路。
                        encoder->requestKeyframe();
                        pumpCaptureFrames(kKeyframePumpFrames);
                        kfRequestPending = true;
                        kfFallbackTimer->start(800);
                    } else {
                        // 已知软路无效（libopenh264）：直接重建。新实例首帧必为 IDR，
                        // 喂 2 帧就能把它顶出来，不用先白抓一轮再等 800ms。
                        reinitVideoEncoder();
                        pendingKfEpoch = capturer.contentChangeCount();
                        pendingKfEpochValid = true;
                        pumpCaptureFrames(kKeyframePumpFrames);
                        kfRequestPending = true;
                        kfFallbackTimer->start(1200);
                    }
                }
                return;
            }
#endif

            if (type == "set_resolution") {
                int w = obj["width"].toInt();
                int h = obj["height"].toInt();
                qInfo() << "Helper: changing resolution to" << w << "x" << h;
                if (ScreenCapturer::changeDisplayResolution(w, h)) {
                    capturer.stop();
                    // [B-6] 用客户端配置的帧率重启（原硬编码 30 会让 fps 配置静默
                    // 回退）。重启失败必须显式回错：分辨率已在系统层生效，但采集
                    // 起不来（驱动/权限/设备占用），前端只会看到画面永久冻结。
                    const bool captureRestarted = capturer.start(captureFps);
                    if (captureRestarted) {
                        // capturer.start() 把采集重新拉起，闸门记账必须跟上，
                        // 否则锁屏中切分辨率会留下一条没人关的采集定时器。
                        captureGateOpen = true;
                        applyCaptureGate();
                    } else {
                        qWarning() << "Helper: capturer restart after resolution change FAILED";
                        QJsonObject err;
                        err["type"] = "error";
                        err["message"] = QString("分辨率已切换为 %1x%2，但屏幕采集重启失败，"
                                                 "画面将停止刷新").arg(w).arg(h);
                        ws.sendTextMessage(QString::fromUtf8(QJsonDocument(err).toJson(QJsonDocument::Compact)));
                    }
                    // [H9] 输入归一化基准必须同步更新：仅在启动时 setScreenSize 一次，
                    // 切分辨率后仍是旧尺寸 → 服务模式鼠标系统性偏移
                    inputMgr.setScreenSize(w, h);
                    // 改分辨率可能顺带改变了该显示器在虚拟桌面里的原点（副屏被
                    // 主屏"推开/拉近"），原点必须跟着采集快照一起刷新。
                    inputMgr.setScreenOffset(capturer.originX(), capturer.originY());
                    // 更新配置文件中的当前分辨率（QSaveFile 原子写，见 helperSaveConfigField）
                    helperSaveConfigField("currentResolution", QString("%1x%2").arg(w).arg(h));
                    // 系统分辨率确实已变，仍需下发真实尺寸供前端调整 canvas/坐标映射
                    QJsonObject info;
                    info["type"] = "screen_info";
                    info["width"] = w;
                    info["height"] = h;
                    ws.sendTextMessage(QString::fromUtf8(QJsonDocument(info).toJson(QJsonDocument::Compact)));
#ifdef USE_FFMPEG
                    // 分辨率变化 → 编码器/sws 上下文尺寸失效，必须重建（否则 sws 越界读）。
                    // 同时重新上报能力（宽高变了），服务端据此更新坐标映射基准。
                    if (videoActive && captureRestarted)
                        reinitVideoEncoder();
                    // 采集未重启成功时不能上报"视频可用"：服务端会据此继续等 H.264，
                    // 而 helper 已无帧可产 → 永久黑屏。
                    if (captureRestarted)
                        sendCaptureCaps(QString());
#endif
                } else {
                    QJsonObject err;
                    err["type"] = "error";
                    err["message"] = QString("分辨率 %1x%2 切换失败").arg(w).arg(h);
                    ws.sendTextMessage(QString::fromUtf8(QJsonDocument(err).toJson(QJsonDocument::Compact)));
                }
                return;
            }

            if (type == "clipboard") {
                // 客户端粘贴 → 写入用户会话剪贴板，并在远端注入一次 Ctrl+V
                // [B20] 支持图片（mime:"image/png" + data:base64）：服务模式此前只
                // 处理 text 字段，图片被静默丢弃。
                QClipboard* cb = QGuiApplication::clipboard();
                if (obj["mime"].toString() == QLatin1String("image/png")) {
                    const QByteArray png = QByteArray::fromBase64(
                        obj["data"].toString().toLatin1());
                    QImage img;
                    if (png.isEmpty() || !img.loadFromData(png, "PNG") || !cb)
                        return;
                    cb->setImage(img);
                    lastClipText.clear();
                    lastClipImageSig = clipImageSig(img);  // 避免监听到自己的写入后重复上报
                    if (!locked) {
                        QTimer::singleShot(250, &app, [&inputMgr]() {
                            inputMgr.injectKeyboard(86, "KeyV", true, true, false, false, false, false);
                            inputMgr.injectKeyboard(86, "KeyV", false, true, false, false, false, false);
                            inputMgr.updateModifiers(false, false, false);
                        });
                    }
                    return;
                }
                QString text = obj["text"].toString();
                if (text.isEmpty())
                    return;
                if (cb)
                    cb->setText(text);
                lastClipText = text;  // 避免监听到自己的写入后重复上报
                lastClipImageSig.clear();
                if (!locked) {
                    // setText 后延时注入 Ctrl+V：X11 下剪贴板 owner 就绪有短暂异步窗口，
                    // 立即注入可能让远端应用粘贴到旧内容
                    QTimer::singleShot(250, &app, [&inputMgr]() {
                        inputMgr.injectKeyboard(86, "KeyV", true, true, false, false, false, false);
                        inputMgr.injectKeyboard(86, "KeyV", false, true, false, false, false, false);
                        inputMgr.updateModifiers(false, false, false); // 释放 Ctrl
                    });
                }
                return;
            }

            if (type == "system_action") {
                // 系统操作必须在用户会话执行（服务进程在 Session 0 无法操作交互桌面）。
                // 与服务端 handleSystemAction 保持同一套平台探测逻辑。
                QString action = obj["action"].toString();
                qInfo() << "Helper: system action requested:" << action;
#ifdef _WIN32
                if (action == "lock") {
                    // 直接在 helper 进程里调 LockWorkStation：它本来就跑在
                    // winsta0\Default 交互桌面上，起 `rundll32.exe user32.dll,LockWorkStation`
                    // 子进程只是把失败变成无声（实测 XP 主机：日志显示动作已到达 helper，
                    // 屏幕却从未锁上，且拿不到任何错误码）。
                    if (LockWorkStation())
                        qInfo() << "Helper: LockWorkStation ok";
                    else
                        qWarning() << "Helper: LockWorkStation failed, error =" << GetLastError();
                } else if (action == "show_desktop") {
                    // Win7+ 用 PowerShell；WinXP 无 powershell → cscript 同款 COM 调用
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
                }
#elif defined(Q_OS_LINUX)
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
                    QString lock = firstOf({ "xdg-screensaver", "gnome-screensaver-command", "loginctl" });
                    if (lock.endsWith(QStringLiteral("xdg-screensaver")))
                        launch(lock, { "lock" });
                    else if (lock.endsWith(QStringLiteral("gnome-screensaver-command")))
                        launch(lock, { "-l" });
                    else
                        launch(lock, { "lock-session" });
                } else if (action == "show_desktop") {
                    // 0) X11 EWMH 直发 _NET_SHOWING_DESKTOP —— 最可靠，不依赖外部工具；
                    //    GNOME(mutter) 实测不响应 XTEST 合成的 Super+D，必须走此协议。
                    // 1) wmctrl -k on → 2) xdotool super+d → 3) GNOME Shell Eval
                    x11ShowDesktopOnce(true);
                    QString sh = findBin("wmctrl");
                    if (!sh.isEmpty())
                        launch(sh, { "-k", "on" });
                    else if (!(sh = findBin("xdotool")).isEmpty())
                        launch(sh, { "key", "--clearmodifiers", "super+d" });
                    else if (!(sh = findBin("gdbus")).isEmpty())
                        launch(sh, { "call", "--session", "--dest", "org.gnome.Shell",
                                     "--object-path", "/org/gnome/Shell",
                                     "--method", "org.gnome.Shell.Eval",
                                     "global.activate_action('show-desktop', null)" });
                } else if (action == "task_manager") {
                    QString tm = firstOf({ "lxtask", "xfce4-taskmanager", "mate-system-monitor",
                                           "gnome-system-monitor", "ksysguard", "plasma-systemmonitor" });
                    if (!tm.isEmpty())
                        launch(tm);
                } else if (action == "logout") {
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
                    }
                } else if (action == "reboot") {
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
                }
#elif defined(Q_OS_MACOS)
                if (action == "lock") {
                    QProcess::startDetached(QStringLiteral(
                        "/System/Library/CoreServices/Menu Extras/User.menu/Contents/Resources/CGSession"),
                        { QStringLiteral("-suspend") });
                } else if (action == "show_desktop") {
                    QProcess::startDetached(QStringLiteral("osascript"), { QStringLiteral("-e"),
                        QStringLiteral("tell application \"System Events\" to key code 103") });
                } else if (action == "task_manager") {
                    QProcess::startDetached(QStringLiteral("open"),
                        { QStringLiteral("-a"), QStringLiteral("Activity Monitor") });
                } else if (action == "logout") {
                    QProcess::startDetached(QStringLiteral("osascript"), { QStringLiteral("-e"),
                        QStringLiteral("tell app \"System Events\" to log out") });
                } else if (action == "reboot") {
                    QProcess::startDetached(QStringLiteral("osascript"), { QStringLiteral("-e"),
                        QStringLiteral("tell app \"System Events\" to restart") });
                } else if (action == "poweroff") {
                    QProcess::startDetached(QStringLiteral("osascript"), { QStringLiteral("-e"),
                        QStringLiteral("tell app \"System Events\" to shut down") });
                }
#endif
                return;
            }

            // [B17] 画质/帧率配置：服务模式下压缩/缩放/采集节拍都在 helper 侧，
            // 必须在锁屏门前处理（非输入类，锁屏中也应生效）。档位映射与服务端
            // 本地分支一致（high=80/100、medium=60、low=35、verylow=20）。
            if (type == "config") {
                QString qname = obj["quality"].toString();
                int jpegQ = qname == "high" ? 80
                          : qname == "medium" ? 60
                          : qname == "low" ? 35
                          : qname == "verylow" ? 20 : -1;
                if (jpegQ > 0) {
                    compressor->setQuality(jpegQ);
                    // 视频码率档位：verylow 单独用更低档（与服务端 videoBitrateFor 一致）
                    videoQualityLevel = (qname == "verylow") ? 15 : jpegQ;
                    int scale = obj["scale"].toInt();
                    if (qname == "high")
                        scale = 100;
                    if (scale >= 10 && scale <= 100) {
                        compressor->setScalePercent(scale);
                        videoScale = scale;
                    }
                    qInfo() << "Helper: config applied, quality" << jpegQ << "scale"
                            << (scale >= 10 && scale <= 100 ? scale : 100);
                }
                int fps = obj["fps"].toInt();
                if (fps >= 1) {
                    // 与 ScreenCapturer::setFps 的上界保持一致：captureFps 还要参与
                    // 编码器帧率/码率估算与切分辨率后的 capturer.start()，
                    // 原样保存 5000 会让定时器间隔算成 0ms（忙转）+ 编码器重建风暴。
                    if (fps > 60) fps = 60;
                    capturer.setFps(fps);
                    captureFps = fps; // [B-6] 记住配置帧率，供切分辨率后重启使用
                }
                // [PACE-FACTOR] 慢读回节流倍数（服务端 server_config.json 的
                // capturePaceFactor）。采集间隔 = 单帧实测耗时 × 倍数，占空比
                // ≈1/(1+倍数)：2 ⇒ 33%（默认）、4 ⇒ 20%、9 ⇒ 10%。只对读屏
                // 慢到触发 pace 的机器有效，快机 pace 恒 0、不受影响。
                if (obj.contains("paceFactor")) {
                    const double pf = obj["paceFactor"].toDouble();
                    if (pf >= 1.0 && pf <= 12.0) {
                        capturer.setPaceFactor(pf);
                        qInfo() << "Helper: capture pace factor =" << pf;
                    }
                }
#ifdef USE_FFMPEG
                // 视频参数（服务模式 helper 侧编码）：codec / hw_encode
                bool videoParamChanged = false;
                QString codecStr = obj["codec"].toString();
                if (!codecStr.isEmpty()) {
                    CodecType c = helperCodecFromString(codecStr);
                    if (c != videoCodec) { videoCodec = c; videoParamChanged = true; }
                }
                QString hwStr = obj["hw_encode"].toString();
                if (!hwStr.isEmpty()) {
                    HwEncodeMode m = hwStr == QLatin1String("on") ? HwEncodeMode::On
                                   : hwStr == QLatin1String("off") ? HwEncodeMode::Off
                                                                   : HwEncodeMode::Auto;
                    if (m != videoHwMode) { videoHwMode = m; videoParamChanged = true; }
                }
                // 视频模式中参数（编码协议/硬件开关/帧率/缩放/画质）变化 → 重建编码器即时生效。
                // 注意：切视频前服务端会先下发 config 再发 video_on，此时 videoActive 仍为 false，
                // 参数已记录到上面的变量，startVideoMode() 会按新值建立编码器。
                if (videoActive && (videoParamChanged || fps >= 1 || jpegQ > 0))
                    reinitVideoEncoder();
#endif
                return;
            }

            // [XP] 锁屏状态在 XP 上高频抖动（实锤日志：locked true→false 仅隔 2 秒），
            // 且 XP 的登录/解锁 UI 运行在 winsta0\default 桌面（即 helper 所在桌面，
            // 锁屏期间实测 input desktop 名字就是 "default"）。XP 下不丢输入：
            // helper 直注覆盖 default 桌面激活的场景（欢迎屏点用户名块、输解锁密码），
            // secure-input 进程覆盖 winlogon 桌面激活的场景；非激活桌面上的
            // SendInput 是无害 no-op，两条通道总有一条命中。丢输入会让抖动窗口期
            // 内的密码输入全部蒸发送失（XP 首次登录输密码失败的根因）。
            // Win7+ 登录 UI 在 winlogon 安全桌面，helper 注入无效但同样无害，
            // 仍保留丢弃以省去无效 SendInput。
            if (locked && !isWinXP)
                return;

            bool needFrame = false;
            if (type == "mousemove") {
                inputMgr.injectMouseMove(obj["x"].toInt(), obj["y"].toInt());
            } else if (type == "mousedown" || type == "mouseup") {
                inputMgr.injectMouseButton(obj["x"].toInt(), obj["y"].toInt(),
                    obj["button"].toInt(), type == "mousedown");
                needFrame = true;
            } else if (type == "keydown" || type == "keyup") {
                inputMgr.injectKeyboard(obj["keycode"].toInt(), obj["code"].toString(),
                    type == "keydown", obj["ctrl"].toBool(),
                    obj["alt"].toBool(), obj["shift"].toBool(),
                    isWin7 && locked,
                    obj["isChar"].toBool(),
                    obj["meta"].toBool()); // [B15] Win/Super 必须透传，否则服务模式 Win 键全失效
                needFrame = true;
            } else if (type == "wheel") {
                inputMgr.injectWheel(obj["delta"].toInt());
                needFrame = true;
            }
            // [IDLE-WAKE] 静止降频到 ~3s 一帧后，点击/按键必须立刻逼出一帧，
            // 否则第一次输入的效果要等下一个 idle 周期才看得见。鼠标移动不唤醒：
            // 光标由客户端本地绘制，且移动是高频事件，唤醒等于把抓屏重新拉满。
            if (needFrame)
                capturer.forceNextFrame();
        });

    QObject::connect(&capturer, &ScreenCapturer::screenLocked, &app,
        [&](bool val) {
            // 抓屏侧的"黑帧=锁屏"在 Windows 上只是猜测，且经常猜错：DPMS 关屏、XP 的
            // 空白屏保（blank.scr）、纯黑壁纸、会话切换瞬间都会全黑而桌面并未锁。
            // 实测 XP 主机：输入桌面轮询说 default（未锁），黑帧启发式说锁 → 两个来源
            // 各广播各的，前端 locked 每 2 秒 true↔false 抖动，用户已解锁却一直看到
            // "屏幕已锁定"遮罩。锁屏/登录界面在 Windows 上必然切走输入桌面（XP 的
            // Welcome/Security、Win7+ 的 Winlogon 安全桌面），轮询那条通道足够可靠，
            // 所以这里只记录、不再驱动状态。
            if (val == locked)
                return;
            qInfo() << "Helper: capturer reports screen" << (val ? "black(locked)" : "unlocked")
                    << "but desktop poll says locked =" << desktopPollLocked << "- ignored";
        });

    QTimer* desktopCheckTimer = new QTimer(&app);
    // 「屏保恢复时是否要密码」。实测（XP 主机 + mingw530 winuser.h:4983/4988）：
    //   0x0072 = SPI_GETSCREENSAVERRUNNING —— 只表示"屏保进程在跑"，与要不要密码无关；
    //   0x0076 = SPI_GETSCREENSAVESECURE —— XP 上调用直接失败 err=87（不支持），Vista+ 才有。
    // 旧代码把 0x0072 当成 0x0076 用，于是"只要屏保在跑就算锁屏"：没勾密码的闲置屏保
    // 会被永久报成锁屏，而"非安全屏保就杀掉 .scr 把画面放回来"那条分支成了死代码。
    // 注册表才是各版本都可靠来源；helper 以登录用户身份运行，HKCU 就是那个用户。
    // 值名实测（XP 主机 172.24.52.172 控制台用户 hive + 本机 Win11）：两处都叫
    // ScreenSaverIsSecure（REG_SZ "0"/"1"）；先前按 ScreenSaveSecure 读，在任何版本上
    // 都读不到，于是 Vista+ 靠 SPI 兜底还能对，XP 兜底又失败 → 真锁屏被报成未锁。
    auto screenSaverRequiresPassword = []() -> bool {
        wchar_t buf[8] = { 0 };
        DWORD bytes = sizeof(buf) - sizeof(wchar_t), type = 0;
        HKEY hk = nullptr;
        if (RegOpenKeyExW(HKEY_CURRENT_USER, L"Control Panel\\Desktop", 0,
                          KEY_QUERY_VALUE, &hk) == ERROR_SUCCESS) {
            const LONG rc = RegQueryValueExW(hk, L"ScreenSaverIsSecure", nullptr, &type,
                                             reinterpret_cast<LPBYTE>(buf), &bytes);
            RegCloseKey(hk);
            if (rc == ERROR_SUCCESS && (type == REG_SZ || type == REG_EXPAND_SZ))
                return buf[0] == L'1';
        }
        BOOL secure = FALSE;   // 注册表读不到才问系统（Vista+ 支持 0x0076）
        return SystemParametersInfo(0x0076, 0, &secure, 0) != FALSE && secure != FALSE;
    };
    // 屏保进程当前是否在跑（0x0072 在 XP/Vista+ 都可用；实测 XP 锁屏欢迎界面下为 0）
    auto screenSaverProcessRunning = []() -> bool {
        BOOL running = FALSE;
        SystemParametersInfo(0x0072, 0, &running, 0);
        return running != FALSE;
    };
    // XP 的"锁屏"其实有两种，判据、可行操作和该给用户的话都不一样（全部实测于
    // 172.24.52.172）：
    //   screensaver —— 屏保恢复锁屏：输入桌面是 Screen-saver，logon.scr 还在跑，
    //     ScreenSaverIsSecure=1。用户会话活着，鼠标注入能把屏保点掉。
    //   logon —— 登录界面锁屏：输入桌面已是 Winlogon（欢迎屏幕/切换用户），显示的是
    //     msgina 的 DirectUI 登录面板。
    // 两者的解锁都由 secure-input 在安全桌面上完成：屏保锁屏先结束 .scr，等桌面切回
    // 登录桌面后点用户磁贴展开密码框，再键入密码；所以 kind 只用于诊断，提示语一致。
    auto lockKindOf = [](const QString& desk, bool onScreensaverDesk) -> QString {
        if (onScreensaverDesk)
            return QStringLiteral("screensaver");
        const QString lower = desk.toLower();
        if (lower == QStringLiteral("winlogon") || lower == QStringLiteral("welcome"))
            return QStringLiteral("logon");
        return QStringLiteral("secure");
    };
    auto lockHintFor = [](const QString&) -> QString {
        return QString::fromUtf8("Windows 锁屏在安全桌面上，点「解锁」由服务端注入密码");
    };
    QObject::connect(desktopCheckTimer, &QTimer::timeout, &app, [&]() {
        // 桌面名在锁屏期间也会变（XP：屏保退出→登录 UI 落在另一个桌面），而服务端的
        // secure-input 进程是按桌面名投放的 —— 名字变了必须重报，否则键鼠继续打进
        // 已经没人看的旧桌面。
        static QString lastReportedDesk;
        static QString lastReportedKind;
        static int openInputFailures = 0;
        HDESK hDesk = OpenInputDesktop(0, FALSE, GENERIC_READ);
        if (!hDesk) {
            // OpenInputDesktop 在桌面切换的瞬间（屏保退出→登录桌面、会话切换）会偶发失败。
            // 单次失败就广播 locked=true 会让前端闪出 2 秒"屏幕已锁定"遮罩，而用户那边
            // 桌面其实正常 —— 实测 XP 主机上这正是"没锁却提示已锁"的来源之一。
            // 连续两次（≈4 秒）失败才认定为锁屏。
            if (++openInputFailures < 2) {
                qInfo() << "Helper: OpenInputDesktop failed, waiting for confirmation";
                return;
            }
            desktopPollLocked = true;
            applyCaptureGate();
            // XP 的 helper 是服务用登录用户令牌拉起来的，它打得开 Screen-saver 桌面、
            // 打不开 Winlogon 桌面（实测同一进程：前者报 deskName=Screen-saver，后者
            // OpenInputDesktop 直接失败）。所以这里要按屏保进程在不在跑分辨两种锁屏：
            //   在跑 → 还停在屏保（恢复要密码）桌面；
            //   没跑 → 屏保已退出、登录 UI 接管了输入桌面，就是欢迎屏幕那种锁。
            // 不做这个判断的话，从屏保桌面切到登录界面后 locked 一直是 true、失败分支
            // 又只在 !locked 时广播，前端就永远停在上一条 "screensaver" 的提示上（实测
            // 已到 Winlogon 仍报"屏保恢复要密码"）。
            QString kind;
            QString hint;
            if (isWinXP) {
                kind = screenSaverProcessRunning() ? QStringLiteral("screensaver")
                                                   : QStringLiteral("logon");
                hint = lockHintFor(kind);
            } else {
                kind = QStringLiteral("unknown");
                hint = QString::fromUtf8("Windows 锁屏在安全桌面上，点「解锁」由服务端注入密码");
            }
            if (!locked || kind != lastReportedKind) {
                locked = true;
                lastReportedDesk.clear();   // 这次连桌面名都拿不到，别拿旧值糊弄服务端
                lastReportedKind = kind;
                inputDesktopName.clear();
                qInfo() << "Helper: desktop locked, kind =" << kind;
                QJsonObject msg;
                msg["type"] = "screen_locked";
                msg["locked"] = true;
                msg["kind"] = kind;
                msg["hint"] = hint;
                ws.sendTextMessage(QString::fromUtf8(QJsonDocument(msg).toJson(QJsonDocument::Compact)));
            }
            return;
        }
        openInputFailures = 0;
        wchar_t name[256] = {};
        DWORD len = 0;
        bool isLocked = false;
        bool onScreensaverDesk = false;
        QString deskName;
        if (GetUserObjectInformationW(hDesk, UOI_NAME, name, sizeof(name), &len)) {
            deskName = QString::fromWCharArray(name);
            const QString lower = deskName.toLower();
            // XP 的屏保不在 Default 桌面上跑，系统给它单独建了一个名为
            // "Screen-saver" 的输入桌面（实测该主机：deskprobe 输出
            // `INPUT desktop = Screen-saver`，同时 Welcome/Security 还没创建、
            // Winlogon 桌面存在但没在显示）。
            //
            // 屏保桌面算不算"锁屏"取决于**恢复时是否要密码**（控制面板"屏幕保护
            // 程序设置 → 在恢复时显示登录屏幕"，即 SPI_GETSCREENSAVESEC）：
            // * 勾了：这台机器上"屏保"就是锁 —— 想回到桌面必须输密码。按锁屏处理，
            //   前端给出"屏幕已锁定 + 一键解锁"，用户点一下就能进。
            // * 没勾：动一下鼠标就回桌面，不需要密码。按未锁处理，并由下面的
            //   "非安全屏保才杀 .scr"分支把画面放回来。
            // 一刀切地"屏保 != Default 就是锁屏"会把每次闲置屏保都报成锁屏；
            // 一刀切地"屏保永远不算锁"又会让 XP 的 LockWorkStation（实测它就是把
            // 输入桌面切到 Screen-saver）显示成"没锁的黑屏"，两个方向都不对。
            onScreensaverDesk = (lower == QStringLiteral("screen-saver"));
            if (onScreensaverDesk) {
                // 同一个 Screen-saver 桌面有两种完全不同的情况，必须分开判：
                //   * 屏保进程还在跑 → 只有"恢复时显示登录屏幕"勾了才算锁屏。没勾就是
                //     普通闲置屏保，报锁屏会把远端永久挡在假锁提示后面（动鼠标就能回去）。
                //   * 屏保进程不在跑 → 这个桌面被登录 UI 占着（XP 锁屏/切换用户），算锁屏。
                isLocked = !screenSaverProcessRunning() || screenSaverRequiresPassword();
            } else {
                isLocked = (lower != QStringLiteral("default"));
            }
        }
        CloseDesktop(hDesk);
        inputDesktopName = isLocked ? deskName : QString();
        desktopPollLocked = isLocked;
        applyCaptureGate();
#ifdef _WIN32
        // 不安全的 XP 屏保（恢复时不要密码）：杀一次 .scr 把远端画面放回来。
        //
        // 为什么只在"不要密码"时杀（实测 XP 主机日志得出）：
        // * helper 被服务用 lpDesktop=winsta0\\default 拉起，它的 SendInput 只会落在
        //   Default 桌面；屏保在 "Screen-saver" 桌面上，根本收不到 —— 旧 stage 1 白做。
        // * 旧实现无条件杀 .scr，而这台机器的屏保是**安全屏保**：杀掉它等于"用户动了
        //   鼠标"，XP 于是切到登录桌面。登录桌面 helper 打不开 → OpenInputDesktop
        //   连续失败 → 我们报 locked=true，一直挂到系统 ~60 秒后把屏保重新拉起来才
        //   复位。这就是"已经解锁了还是显示屏幕已锁定"里那 1 分钟的假锁。
        // * 旧 stage 3 的 LockWorkStation 更糟：锁屏后屏保状态位依旧为真，于是每 2 秒
        //   锁一次，用户刚输完密码又被锁回去 —— "反复横跳"的直接制造者，已彻底删除。
        static int xpSsKills = 0;
        if (isWinXP && onScreensaverDesk && !isLocked) {
            if (xpSsKills++ == 0) {
                qInfo() << "XP screen saver (no password on resume): killing .scr once";
                HANDLE hSnap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
                if (hSnap != INVALID_HANDLE_VALUE) {
                    PROCESSENTRY32W pe = { sizeof(pe) };
                    if (Process32FirstW(hSnap, &pe)) {
                        do {
                            QString exe = QString::fromWCharArray(pe.szExeFile);
                            if (exe.endsWith(".scr", Qt::CaseInsensitive)) {
                                HANDLE hProc = OpenProcess(PROCESS_TERMINATE, FALSE, pe.th32ProcessID);
                                if (hProc) {
                                    TerminateProcess(hProc, 0);
                                    CloseHandle(hProc);
                                    qInfo() << "Terminated:" << exe;
                                }
                            }
                        } while (Process32NextW(hSnap, &pe));
                    }
                    CloseHandle(hSnap);
                }
            }
        } else {
            xpSsKills = 0;
        }
#endif
        // 桌面名在锁屏期间也会变（XP：屏保退出→登录 UI 落在另一个桌面），而服务端的
        // secure-input 进程是按桌面名投放的 —— 名字变了必须重报，否则键鼠继续打进
        // 已经没人看的旧桌面。锁的种类变了同样要重报（前端遮罩文案按 kind 给话术）。
        const QString kind = isLocked ? lockKindOf(deskName, onScreensaverDesk) : QString();
        if (isLocked != locked || (isLocked && (deskName != lastReportedDesk || kind != lastReportedKind))) {
            locked = isLocked;
            lastReportedDesk = isLocked ? deskName : QString();
            lastReportedKind = kind;
            qInfo() << "Helper: screen locked =" << locked << "input desktop =" << deskName;
            QJsonObject msg;
            msg["type"] = "screen_locked";
            msg["locked"] = locked;
            msg["hint"] = locked ? lockHintFor(kind) : QString();
            if (locked)
                msg["kind"] = kind;
            if (locked && !deskName.isEmpty())
                msg["desktop"] = deskName;
            ws.sendTextMessage(QString::fromUtf8(QJsonDocument(msg).toJson(QJsonDocument::Compact)));
        }
#ifdef Q_OS_WIN
        // 外部改分辨率 / 热插拔：GDI 捕获区是建实例时定死的几何，BitBlt 不会失败，
        // 只会静默抓到错位/残缺的画面（DXGI 则可能在 ACCESS_LOST 后自己重初始化，
        // 但 winOutputs_ 快照与鼠标基准仍是旧的）。解锁状态下每 2s 比对一次枚举
        // 快照，变了就重建采集并整链路上报。
        if (!desktopPollLocked && capturer.pollOutputGeometry()) {
            qInfo() << "Helper: capture geometry changed ->" << capturer.width() << "x" << capturer.height()
                    << "@" << capturer.originX() << "," << capturer.originY();
            inputMgr.setScreenSize(capturer.width(), capturer.height());
            inputMgr.setScreenOffset(capturer.originX(), capturer.originY());
            // 重建在 winApplyOutput 里完成，闸门原本关着时由它自己保持 suspend。
            QJsonObject info;
            info["type"] = "screen_info";
            info["width"] = capturer.width();
            info["height"] = capturer.height();
            ws.sendTextMessage(QString::fromUtf8(QJsonDocument(info).toJson(QJsonDocument::Compact)));
#ifdef USE_FFMPEG
            // 尺寸变了编码器/sws 上下文失效，重建后前端按新 screen_info 重建 MSE
            if (videoActive && encoder)
                reinitVideoEncoder();
            sendCaptureCaps(QString());
#endif
        }
#endif
    });
    desktopCheckTimer->start(2000);

    compressor->start();
    if (!capturer.start(30)) {
        qCritical("Helper: failed to start screen capturer");
        compressor->shutdown();
        return 1;
    }
    // 采集启动后才有多屏枚举快照。输入归一化基准必须按捕获区真实几何/原点来算：
    // 选了副屏时原点不是 (0,0)，否则注入的鼠标整体落在主屏上（前端画面与系统
    // 光标各走各的）。
    inputMgr.setScreenSize(capturer.width(), capturer.height());
    inputMgr.setScreenOffset(capturer.originX(), capturer.originY());

    // [EVLOOP] 主线程事件循环延迟探针。采集异步化（CAP-ASYNC）之后这里应长期是个
    // 位数 ms；若又出现 100ms+ 的尖峰，说明有重活抢回了 GUI 线程（整帧拷贝/编码/
    // 同步抓屏），是"操作一顿一顿"的量化指标。同步老路径下 XP 会稳定打出 ~300ms。
    {
        QTimer* lagTimer = new QTimer(&app);
        lagTimer->setInterval(100);
        qint64 lagPrev = QDateTime::currentMSecsSinceEpoch();
        qint64 lagMax = 0, lagSum = 0;
        int lagN = 0, lagTicks = 0;
        QObject::connect(lagTimer, &QTimer::timeout, &app, [=]() mutable {
            const qint64 now = QDateTime::currentMSecsSinceEpoch();
            const qint64 d = now - lagPrev - 100;
            lagPrev = now;
            if (d > 0) {
                lagSum += d;
                ++lagN;
                if (d > lagMax)
                    lagMax = d;
            }
            if (++lagTicks >= 50) {   // 每 5 秒一条
                qInfo() << "EVLOOP lagAvg=" << (lagN ? lagSum / lagN : 0) << "lagMax=" << lagMax;
                lagSum = 0;
                lagN = 0;
                lagMax = 0;
                lagTicks = 0;
            }
        });
        lagTimer->start();
    }

    // 枚举支持的分辨率并写入配置文件（QSaveFile 原子写，见 helperSaveConfigField）
    {
        QJsonArray resolutions = ScreenCapturer::enumerateSupportedResolutions();
        helperSaveConfigField("resolutions", resolutions);
        helperSaveConfigField("currentResolution",
            QString("%1x%2").arg(capturer.width()).arg(capturer.height()));
        qInfo() << "Helper: wrote" << resolutions.size() << "supported resolutions to config, current ="
                << capturer.width() << "x" << capturer.height();
    }

    QObject::connect(&app, &QGuiApplication::aboutToQuit, [&]() {
        SetThreadExecutionState(ES_CONTINUOUS);
        quitting = true;
        // 退出前显式停止后台线程，避免 run() 返回时逆序析构栈对象
        // （compressor->capturer->ws）与编码线程/采集线程并发竞态导致 use-after-free：
        // 编码线程在 JpegCompressor 析构后仍 emit jpegCompressed -> activate 访问
        // 已析构的 QObjectData（d_ptr=0）-> EXCEPTION_ACCESS_VIOLATION。
        // 停止顺序须逆数据流：先停采集（不再产生新帧）→ 排空已排队但尚未投递的
        // frameCaptured（这些排队事件会访问 compressor，必须在 compressor 析构前消化）
        // → 再停编码线程（compressor 与视频编码器）→ 最后关闭 ws（此时已无任何 emit）。
        capturer.stop();
        QCoreApplication::processEvents();
        compressor->shutdown();
#ifdef USE_FFMPEG
        // 视频编码器必须排在 ws.close() 之前：编码线程通过 QueuedConnection 向 ws
        // 投递 encodedFrame，若 ws 先析构而线程仍在 emit 会访问已析构对象。
        if (encoder)
            encoder->shutdown();
#endif
        ws.close();
    });
    return app.exec();
#else
    (void)argc; (void)argv;
    qCritical("Helper process is not supported on this platform");
    return 1;
#endif
}
