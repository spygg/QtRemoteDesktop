#include "helper_process.h"
#include "crashhandler.h"
#include "inputmanager.h"
#include "rdpserver.h"
#include "screencapturer.h"
#include "videoencoder.h"

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
#include <QStandardPaths>
#include <QThread>
#include <QTimer>
#include <QWebSocket>
#include <memory>

#ifdef _WIN32
#include <windows.h>
#include <tlhelp32.h>
#endif

void logToFile(QtMsgType type, const QMessageLogContext& lg, const QString& msg);
void applyLogLevelFromArgs(int argc, char* argv[]);

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
    int capturePumpLeft = 0;
    auto pumpCaptureFrames = [&](int n) {
        capturePumpLeft = qMax(capturePumpLeft, n);
        if (!capturePumpTimer->isActive())
            capturePumpTimer->start(80);
    };
    QObject::connect(capturePumpTimer, &QTimer::timeout, &app, [&]() {
        if (quitting || !videoActive) {
            capturePumpTimer->stop();
            capturePumpLeft = 0;
            return;
        }
        capturer.forceNextFrame();
        if (--capturePumpLeft <= 0) {
            capturePumpTimer->stop();
            capturePumpLeft = 0;
        }
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
        if (!encoder->initialize(videoCodec, sw, sh, ew, eh, captureFps, videoBaseBitrate, videoHwMode))
            qWarning() << "Helper: video encoder re-init failed (config change)";
    };

    // 关键帧请求兜底：先走 requestKeyframe（软编快路径），超时未见关键帧就重建编码器。
    // 依据（2026-10-08 实测）：videoencoder.cpp:632 的注释已说明"旧版 ffmpeg 不认
    // frame->pict_type"，本机 libopenh264 实测吐出的仍是 P 帧（562B），只有重建
    // 编码器才能可靠拿到新 IDR。服务端 pumpKeyframe 的"单进程路径"也是同样兜底
    // （hasIdr 仍未见新 IDR 时重建），helper 侧此前缺这一步 → 新接入浏览器黑屏。
    bool kfRequestPending = false;
    QTimer* kfFallbackTimer = new QTimer(&app);
    kfFallbackTimer->setSingleShot(true);
    QObject::connect(kfFallbackTimer, &QTimer::timeout, &app, [&]() {
        if (!kfRequestPending)
            return;
        kfRequestPending = false;
        if (quitting || !videoActive || !encoder)
            return;
        qWarning() << "Helper: request_keyframe 未产出 IDR，重建编码器强制关键帧";
        reinitVideoEncoder();
        pumpCaptureFrames(4);
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
        // 立即连喂几帧：新编码器首帧即 IDR，且要喂够帧数才能突破 1 帧输出延迟，
        // 否则接入后要等桌面自然变化才有画面（静止桌面会长时间黑屏）。
        pumpCaptureFrames(4);
    };

    // 退出视频模式：停编码器、恢复 JPEG 上报
    auto stopVideoMode = [&]() {
        capturePumpTimer->stop();
        capturePumpLeft = 0;
        kfRequestPending = false;
        kfFallbackTimer->stop();
        if (encoder)
            encoder->shutdown();
        videoActive = false;
        qInfo() << "Helper: video mode OFF, resumed JPEG";
    };

    // 编码帧 → 服务端：[1字节类型(0x01=IDR/0x02=P)][u32 长度][i64 时间戳][数据]
    // （与服务端→浏览器 13 字节头一致，服务端可直接复用 onEncodedFrame 分发）
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
            QByteArray packet;
            QDataStream stream(&packet, QIODevice::WriteOnly);
            stream.setByteOrder(QDataStream::BigEndian);
            stream << quint8(keyframe ? 0x01 : 0x02);
            stream << quint32(data.size());
            stream << qint64(ts);
            packet.append(data);
            ws.sendBinaryMessage(packet);
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
#endif // USE_FFMPEG

    QObject::connect(&ws, &QWebSocket::connected, &app, [&]() {
        qInfo() << "Helper: connected to service WS successfully";
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
        // [B-5] 重连后重读当前剪贴板：断线窗口内复制的内容此前因未发送成功
        // 不记账（回声抑制只在发送成功后记录），此时补传。
        if (clipTimer)
            clipTimer->start();
    });
    QObject::connect(&ws, QOverload<QAbstractSocket::SocketError>::of(&QWebSocket::error),
        &app, [&](QAbstractSocket::SocketError err) {
            qWarning() << "Helper: WS error" << err << ws.errorString();
        });
    QObject::connect(&ws, &QWebSocket::disconnected, &app, [&]() {
        if (quitting) return;
        qWarning() << "Helper WS disconnected (helper may have crashed), retrying in 3s...";
        SetThreadExecutionState(ES_CONTINUOUS);
        // 必须绑定 context（&app）：无 context 的 singleShot 定时器不随对象析构取消，
        // helper 在 3s 内退出时，lambda 会解引用已销毁的 ws/quitting（use-after-scope），
        // 而 `if (quitting) return` 里的 quitting 本身也是悬垂引用，保护形同虚设。
        QTimer::singleShot(3000, &app, [&]() {
            if (quitting) return;
            ws.open(QUrl(QString("%1://127.0.0.1:%2/capture").arg(wsScheme).arg(wsPort)));
        });
    });
    QObject::connect(&ws, &QWebSocket::sslErrors, &app, [&](const QList<QSslError>& errors) {
        for (const auto& err : errors)
            qWarning() << "Helper: SSL error" << err.errorString();
        ws.ignoreSslErrors();
    });
    ws.open(QUrl(QString("%1://127.0.0.1:%2/capture").arg(wsScheme).arg(wsPort)));

    QObject::connect(&capturer, &ScreenCapturer::frameCaptured,
        &app, [&](const QImage& frame) {
            // 退出流程启动后（栈对象已开始析构）不再访问 compressor，
            // 避免队列中延迟投递的事件访问已析构对象
            if (quitting) return;
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
    QObject::connect(compressor.get(), &JpegCompressor::jpegCompressed,
        &ws, [&](const QByteArray& jpegData) {
            if (quitting) return;
            if (ws.state() != QAbstractSocket::ConnectedState)
                return;
            QByteArray packet;
            QDataStream stream(&packet, QIODevice::WriteOnly);
            stream.setByteOrder(QDataStream::BigEndian);
            stream << quint8(0x03);
            stream << quint32(jpegData.size());
            packet.append(jpegData);
            ws.sendBinaryMessage(packet);
        }, Qt::QueuedConnection);

    bool locked = false;
    bool isWin7 = false, isWinXP = false;
    { struct { ULONG s; ULONG maj; ULONG min; ULONG bld; ULONG pid; WCHAR csd[128]; } osv = { sizeof(osv) };
      typedef LONG (WINAPI *R)(PVOID);
      HMODULE hNt = GetModuleHandleW(L"ntdll.dll");
      if (hNt) { R r = (R)GetProcAddress(hNt, "RtlGetVersion");
        if (r && r(&osv) == 0) {
          if (osv.maj == 6 && osv.min == 1) isWin7 = true;
          if (osv.maj == 5 && (osv.min == 1 || osv.min == 2)) isWinXP = true;
        } } }
    qInfo() << "Helper: starting desktop polling, isWin7 =" << isWin7 << "isWinXP =" << isWinXP;

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
                    capturer.suspend();
                } else if (action == "resume") {
                    qInfo() << "Helper: capture resume requested";
                    capturer.resume();
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
                // 服务端请求新 IDR（新客户端接入 / 解码器自愈）：强制下一帧为关键帧，
                // 并强制抓取当前屏幕一次（绕过去重，静态桌面也能产出 IDR）。
                // 服务端请求新 IDR（新客户端接入 / 解码器自愈）：标记下一帧为关键帧，
                // 并连喂几帧。注意不能只喂 1 帧：libopenh264 有 1 帧输出延迟，
                // 单帧输入配不出码流（详见 pumpCaptureFrames 处说明）。
                if (videoActive && encoder) {
                    encoder->requestKeyframe();
                    pumpCaptureFrames(4);
                    kfRequestPending = true;
                    kfFallbackTimer->start(800);
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
                    // 回退），start 失败必须告警——否则画面永久冻结且无任何线索。
                    if (!capturer.start(captureFps))
                        qWarning() << "Helper: capturer restart after resolution change FAILED";
                    // [H9] 输入归一化基准必须同步更新：仅在启动时 setScreenSize 一次，
                    // 切分辨率后仍是旧尺寸 → 服务模式鼠标系统性偏移
                    inputMgr.setScreenSize(w, h);
                    // 更新配置文件中的当前分辨率
                    {
                        QFile cfgFile(QGuiApplication::applicationDirPath() + "/server_config.json");
                        if (cfgFile.open(QIODevice::ReadWrite)) {
                            QJsonDocument doc = QJsonDocument::fromJson(cfgFile.readAll());
                            if (doc.isObject()) {
                                QJsonObject root = doc.object();
                                root["currentResolution"] = QString("%1x%2").arg(w).arg(h);
                                cfgFile.resize(0);
                                cfgFile.write(QJsonDocument(root).toJson(QJsonDocument::Indented));
                            }
                            cfgFile.close();
                        }
                    }
                    QJsonObject info;
                    info["type"] = "screen_info";
                    info["width"] = w;
                    info["height"] = h;
                    ws.sendTextMessage(QString::fromUtf8(QJsonDocument(info).toJson(QJsonDocument::Compact)));
#ifdef USE_FFMPEG
                    // 分辨率变化 → 编码器/sws 上下文尺寸失效，必须重建（否则 sws 越界读）。
                    // 同时重新上报能力（宽高变了），服务端据此更新坐标映射基准。
                    if (videoActive)
                        reinitVideoEncoder();
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
                    QProcess::startDetached(QStringLiteral("rundll32.exe"),
                        { QStringLiteral("user32.dll,LockWorkStation") });
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
                    capturer.setFps(fps);
                    captureFps = fps; // [B-6] 记住配置帧率，供切分辨率后重启使用
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

            if (locked) return;

            if (type == "mousemove") {
                inputMgr.injectMouseMove(obj["x"].toInt(), obj["y"].toInt());
            } else if (type == "mousedown" || type == "mouseup") {
                inputMgr.injectMouseButton(obj["x"].toInt(), obj["y"].toInt(),
                    obj["button"].toInt(), type == "mousedown");
            } else if (type == "keydown" || type == "keyup") {
                inputMgr.injectKeyboard(obj["keycode"].toInt(), obj["code"].toString(),
                    type == "keydown", obj["ctrl"].toBool(),
                    obj["alt"].toBool(), obj["shift"].toBool(),
                    isWin7 && locked,
                    obj["isChar"].toBool(),
                    obj["meta"].toBool()); // [B15] Win/Super 必须透传，否则服务模式 Win 键全失效
            } else if (type == "wheel") {
                inputMgr.injectWheel(obj["delta"].toInt());
            }
        });

    QObject::connect(&capturer, &ScreenCapturer::screenLocked, &app,
        [&](bool val) {
            locked = val;
            qInfo() << "Helper: screenLocked signal =" << locked;
            QJsonObject msg;
            msg["type"] = "screen_locked";
            msg["locked"] = locked;
            msg["isXP"] = isWinXP;
            msg["hint"] = locked ? QString::fromUtf8("锁屏界面可直接输入密码") : QString();
            ws.sendTextMessage(QString::fromUtf8(QJsonDocument(msg).toJson(QJsonDocument::Compact)));
        });

    QTimer* desktopCheckTimer = new QTimer(&app);
    QObject::connect(desktopCheckTimer, &QTimer::timeout, &app, [&]() {
        HDESK hDesk = OpenInputDesktop(0, FALSE, GENERIC_READ);
        if (!hDesk) {
            if (!locked) {
                locked = true;
                qInfo() << "Helper: desktop locked (OpenInputDesktop failed)";
                QJsonObject msg;
                msg["type"] = "screen_locked";
                msg["locked"] = true;
                msg["hint"] = QString::fromUtf8("锁屏界面可直接输入密码");
                ws.sendTextMessage(QString::fromUtf8(QJsonDocument(msg).toJson(QJsonDocument::Compact)));
            }
            return;
        }
        wchar_t name[256] = {};
        DWORD len = 0;
        bool isLocked = false;
        if (GetUserObjectInformationW(hDesk, UOI_NAME, name, sizeof(name), &len)) {
            QString deskName = QString::fromWCharArray(name);
            isLocked = (deskName.toLower() != QStringLiteral("default"));
        }
        CloseDesktop(hDesk);
#ifdef _WIN32
        // XP 屏保：分级尝试退出，最后再枚举进程杀
        if (isWinXP) {
            BOOL ssRunning = FALSE;
            SystemParametersInfo(0x0072, 0, &ssRunning, 0);
            static int xpSsStage = 0;
            if (ssRunning) {
                if (xpSsStage == 0) {
                    xpSsStage = 1;
                    qInfo() << "XP screen saver: stage 1 - LockWorkStation";
                    LockWorkStation();
                    return;
                }
                if (xpSsStage == 1) {
                    xpSsStage = 2;
                    qInfo() << "XP screen saver: stage 2 - SendInput mouse + ESC";
                    POINT pt; GetCursorPos(&pt);
                    INPUT mi = {};
                    mi.type = INPUT_MOUSE;
                    mi.mi.dx = (pt.x * 65535) / GetSystemMetrics(SM_CXSCREEN);
                    mi.mi.dy = (pt.y * 65535) / GetSystemMetrics(SM_CYSCREEN);
                    mi.mi.dwFlags = MOUSEEVENTF_MOVE | MOUSEEVENTF_ABSOLUTE;
                    SendInput(1, &mi, sizeof(INPUT));
                    INPUT esc[2] = {};
                    esc[0].type = INPUT_KEYBOARD; esc[0].ki.wVk = VK_ESCAPE;
                    esc[1].type = INPUT_KEYBOARD; esc[1].ki.wVk = VK_ESCAPE; esc[1].ki.dwFlags = KEYEVENTF_KEYUP;
                    SendInput(2, esc, sizeof(INPUT));
                    return;
                }
                if (xpSsStage >= 2) {
                    qInfo() << "XP screen saver: stage 3 - killing .scr processes";
                    HANDLE hSnap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
                    if (hSnap != INVALID_HANDLE_VALUE) {
                        PROCESSENTRY32W pe = { sizeof(pe) };
                        if (Process32FirstW(hSnap, &pe)) {
                            do {
                                QString name = QString::fromWCharArray(pe.szExeFile);
                                if (name.endsWith(".scr", Qt::CaseInsensitive)) {
                                    HANDLE hProc = OpenProcess(PROCESS_TERMINATE, FALSE, pe.th32ProcessID);
                                    if (hProc) {
                                        TerminateProcess(hProc, 0);
                                        CloseHandle(hProc);
                                        qInfo() << "Terminated:" << name;
                                    }
                                }
                            } while (Process32NextW(hSnap, &pe));
                        }
                        CloseHandle(hSnap);
                    }
                    LockWorkStation();
                    xpSsStage = 3;
                    return;
                }
            } else {
                xpSsStage = 0;
            }
        }
#endif
        if (isLocked != locked) {
            locked = isLocked;
            qInfo() << "Helper: screen locked =" << locked;
            QJsonObject msg;
            msg["type"] = "screen_locked";
            msg["locked"] = locked;
            msg["hint"] = locked ? QString::fromUtf8("锁屏界面可直接输入密码") : QString();
            ws.sendTextMessage(QString::fromUtf8(QJsonDocument(msg).toJson(QJsonDocument::Compact)));
        }
    });
    desktopCheckTimer->start(2000);

    compressor->start();
    if (!capturer.start(30)) {
        qCritical("Helper: failed to start screen capturer");
        compressor->shutdown();
        return 1;
    }

    // 枚举支持的分辨率并写入配置文件
    {
        QJsonArray resolutions = ScreenCapturer::enumerateSupportedResolutions();
        QFile cfgFile(QGuiApplication::applicationDirPath() + "/server_config.json");
        if (cfgFile.open(QIODevice::ReadWrite)) {
            QJsonDocument doc = QJsonDocument::fromJson(cfgFile.readAll());
            QJsonObject root = doc.isObject() ? doc.object() : QJsonObject();
            root["resolutions"] = resolutions;
            root["currentResolution"] = QString("%1x%2").arg(capturer.width()).arg(capturer.height());
            cfgFile.resize(0);
            cfgFile.write(QJsonDocument(root).toJson(QJsonDocument::Indented));
            cfgFile.close();
            qInfo() << "Helper: wrote" << resolutions.size() << "supported resolutions to config, current ="
                    << root["currentResolution"].toString();
        }
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
