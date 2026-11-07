// server/screencapturer_wayland.cpp
// Wayland 屏幕捕获实现（GNOME Mutter ScreenCast 优先，xdg-desktop-portal 回退）
#include "screencapturer_wayland.h"

#include <QCoreApplication>
#include <QDateTime>
#include <QDeadlineTimer>
#include <QDebug>
#include <QDBusConnection>
#include <QDBusInterface>
#include <QDBusMessage>
#include <QDBusObjectPath>
#include <QDBusReply>
#include <QDBusUnixFileDescriptor>
#include <QEventLoop>
#include <QFile>
#include <QGuiApplication>
#include <QList>
#include <QPair>
#include <QScreen>
#include <QThread>
#include <QTimer>
#include <QVariantMap>

#include <pipewire/pipewire.h>
#include <pipewire/proxy.h>
#include <pipewire/link.h>
#include <spa/param/video/format-utils.h>
#include <spa/param/video/raw.h>
#include <spa/param/video/raw-utils.h>
#include <spa/param/format-utils.h>
#include <spa/pod/builder.h>
#include <spa/pod/vararg.h>
#include <spa/utils/result.h>

#include <unistd.h>
#include <sys/stat.h>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <string>

// ---------------------------------------------------------------------------
// PipeWire 事件回调
// ---------------------------------------------------------------------------
namespace {

void onLinkDestroyed(void* data) {
    qInfo() << "WaylandCapturer: link destroyed";
}

void onLinkDone(void* data, int result) {
    qInfo() << "WaylandCapturer: link done, result" << result;
}

void onLinkError(void* data, int seq, int res, const char* message) {
    qWarning() << "WaylandCapturer: link error, seq" << seq << "res" << res << "msg:" << (message ? message : "none");
}

void onLinkRemoved(void* data) {
    qInfo() << "WaylandCapturer: link removed";
}

void onLinkBound(void* data, uint32_t global_id) {
    qInfo() << "WaylandCapturer: link bound, global_id" << global_id;
}

const pw_proxy_events g_linkEvents = {
    .version = PW_VERSION_PROXY_EVENTS,
    .destroy = onLinkDestroyed,
    .bound = onLinkBound,
    .removed = onLinkRemoved,
    .done = onLinkDone,
    .error = onLinkError,
    .bound_props = nullptr,
};

void onStreamProcess(void* data)
{
    static_cast<WaylandCapturer*>(data)->streamProcess();
}

void onStreamAddBuffer(void* data, struct pw_buffer* buffer)
{
}

void onStreamRemoveBuffer(void* data, struct pw_buffer* buffer)
{
}

void onStreamParamChanged(void* data, uint32_t id, const struct spa_pod* param)
{
    auto* self = static_cast<WaylandCapturer*>(data);
    if (id == SPA_PARAM_Format) {
        if (param) {
            self->parseFormatParam(param);
            qInfo() << "WaylandCapturer: format negotiated:" << self->width() << "x" << self->height();
        }
    }
}

void onStreamStateChanged(void* data, enum pw_stream_state old, enum pw_stream_state state, const char* error)
{
    static_cast<WaylandCapturer*>(data)->onStreamStateChanged((int)old, (int)state, error);
}

const pw_stream_events g_streamEvents = {
    .version = PW_VERSION_STREAM_EVENTS,
    .destroy = nullptr,
    .state_changed = onStreamStateChanged,
    .control_info = nullptr,
    .io_changed = nullptr,
    .param_changed = onStreamParamChanged,
    .add_buffer = onStreamAddBuffer,
    .remove_buffer = onStreamRemoveBuffer,
    .process = onStreamProcess,
    .drained = nullptr,
};

spa_pod* buildVideoFormat(struct spa_pod_builder* b, uint32_t format, int w, int h)
{
    // 使用官方辅助函数构建 raw video format（避免手写 varargs 的坑）
    struct spa_video_info_raw info = {};
    info.format = static_cast<enum spa_video_format>(format);
    info.size = SPA_RECTANGLE(static_cast<uint32_t>(w), static_cast<uint32_t>(h));
    info.framerate = SPA_FRACTION(0, 1);
    info.max_framerate = SPA_FRACTION(0, 1);
    return static_cast<spa_pod*>(spa_format_video_raw_build(b, format, &info));
}

} // namespace

// ---------------------------------------------------------------------------
// WaylandCapturer
// ---------------------------------------------------------------------------
void WaylandCapturer::onStreamStateChanged(int oldState, int newState, const char* error)
{
    // [P2] pw_stream_state 实际枚举：ERROR=-2, UNCONNECTED=0, CONNECTING=1,
    // PAUSED=2, STREAMING=3。旧表 {"UNCONNECTED","CONNECTING","IDLE","PAUSED",
    // "STREAMING"} 把 2 当 IDLE、3 当 PAUSED：日志全错，且 pw_stream_set_active
    // 挂在错误的 3（实为 STREAMING）上——真正的 PAUSED(2) 永远不触发激活。
    // （此前该 bug 被 stream 节点 NODE_ALWAYS_PROCESS 属性掩盖：不激活也推流。）
    static const char* stateNames[] = {"UNCONNECTED","CONNECTING","PAUSED","STREAMING"};
    const char* oldName = (oldState == -2) ? "ERROR"
        : (oldState >= 0 && oldState <= 3) ? stateNames[oldState] : "UNKNOWN";
    const char* newName = (newState == -2) ? "ERROR"
        : (newState >= 0 && newState <= 3) ? stateNames[newState] : "UNKNOWN";
    qInfo() << "WaylandCapturer:" << oldName << "->" << newName
            << "error:" << (error ? error : "none")
            << "activated:" << activated_.load()
            << "frameCount:" << frameCount_.load();

    if (newState == 2 /*PAUSED*/ && !activated_) {
        activated_ = true;
        qInfo() << "WaylandCapturer: stream PAUSED, node" << pw_stream_get_node_id(stream_)
                << ", calling pw_stream_set_active(true)";
        int res = pw_stream_set_active(stream_, true);
        qInfo() << "WaylandCapturer: pw_stream_set_active returned" << res;
    }

    if (newState == -2 /*ERROR*/) {
        // ERROR 是终态：compositor 撤销共享 / 会话失效 / 设备错误。置标志让
        // captureFrame 如实报失败，上层才会走采集失败处理（计数、重建），
        // 而不是继续返回 true 让画面永久冻结。
        if (!streamError_) {
            streamError_ = true;
            hasFrame_ = false;
            qWarning() << "WaylandCapturer: PipeWire stream ERROR ("
                       << (error ? error : "no detail")
                       << ") - reporting capture failure to upper layer";
        }
    }
}

// ---------------------------------------------------------------------------
// WaylandCapturer
// ---------------------------------------------------------------------------
WaylandCapturer::WaylandCapturer(QObject* parent) : QObject(parent)
{
}

WaylandCapturer::~WaylandCapturer()
{
    teardownPipewire();
    closeSession();
}

bool WaylandCapturer::initialize()
{
    if (initialized_)
        return true;

    if (!QDBusConnection::sessionBus().isConnected()) {
        qWarning() << "WaylandCapturer: session D-Bus not available";
        return false;
    }

    // 重建时不能沿用上一次会话的 node id：PipeWireStreamAdded 超时后旧 node
    // 会让等待循环立刻通过，然后 createStream(旧 node) 失败，重建永远起不来。
    nodeId_ = 0;

    pw_init(nullptr, nullptr);

    // 1) GNOME Mutter 私有 ScreenCast（无需交互授权）
    //    QTRD_FORCE_PORTAL=1 时跳过：用于在 GNOME 机器上验证非 GNOME 合成器才会走的
    //    标准 xdg-desktop-portal ScreenCast 分支（默认为空，行为不变）。
    const QByteArray forcePortalEnv = qgetenv("QTRD_FORCE_PORTAL");
    const bool forcePortal = forcePortalEnv == "1"
        || forcePortalEnv.toLower() == "true";
    if (!forcePortal && setupMutterScreenCast()) {
        initialized_ = true;
        // offscreen 平台下 QGuiApplication 拿不到真实分辨率，共享给 InputManager 做坐标映射
        syncEnvSize();
        return true;
    }

    // 2) 回退：标准 xdg-desktop-portal ScreenCast（可能需要用户确认）
    //    必须先拆掉 mutter 路径可能已建好的 PipeWire loop/线程与会话，否则回退后
    //    会残留一个常驻线程 + 录屏会话（mutter 失败分支已 abort，但 setupMutterScreenCast
    //    内部在 createStream 之前失败也可能留下 session）。
    if (forcePortal)
        qWarning() << "WaylandCapturer: QTRD_FORCE_PORTAL=1, skipping Mutter ScreenCast,"
                      " using xdg-desktop-portal...";
    else
        qWarning() << "WaylandCapturer: Mutter ScreenCast unavailable, falling back to portal...";
    teardownPipewire();
    closeSession();
    if (setupPortalScreenCast()) {
        initialized_ = true;
        syncEnvSize();
        return true;
    }

    qWarning() << "WaylandCapturer: no ScreenCast backend available";
    return false;
}

// ---------------------------------------------------------------------------
// GNOME Mutter ScreenCast（无交互）
// ---------------------------------------------------------------------------
bool WaylandCapturer::setupMutterScreenCast()
{
    QDBusConnection bus = QDBusConnection::sessionBus();

    QDBusInterface sc("org.gnome.Mutter.ScreenCast",
                      "/org/gnome/Mutter/ScreenCast",
                      "org.gnome.Mutter.ScreenCast", bus);
    if (!sc.isValid()) {
        qInfo() << "WaylandCapturer: org.gnome.Mutter.ScreenCast not present";
        return false;
    }

    // CreateSession({})
    QDBusMessage creply = sc.call("CreateSession", QVariantMap());
    if (creply.type() != QDBusMessage::ReplyMessage) {
        qWarning() << "WaylandCapturer: mutter CreateSession failed:" << creply.errorMessage();
        return false;
    }
    const QVariantList cargs = creply.arguments();
    if (cargs.size() < 1) return false;
    sessionPath_ = qdbus_cast<QDBusObjectPath>(cargs.at(0)).path();
    if (sessionPath_.isEmpty())
        sessionPath_ = cargs.at(0).toString();
    if (sessionPath_.isEmpty()) {
        qWarning() << "WaylandCapturer: mutter CreateSession returned empty handle";
        return false;
    }

    // RecordMonitor(connector="", {cursor-mode:1}) → 默认显示器
    QDBusInterface sessionIface("org.gnome.Mutter.ScreenCast",
                                sessionPath_,
                                "org.gnome.Mutter.ScreenCast.Session", bus);
    QVariantMap recOptions;
    recOptions.insert("cursor-mode", 1u); // embedded：光标嵌入帧缓冲
    QDBusMessage rreply = sessionIface.call("RecordMonitor", QString(), recOptions);
    if (rreply.type() != QDBusMessage::ReplyMessage) {
        qWarning() << "WaylandCapturer: mutter RecordMonitor failed:" << rreply.errorMessage();
        return false;
    }
    const QVariantList rargs = rreply.arguments();
    if (rargs.size() < 1) return false;
    streamPath_ = qdbus_cast<QDBusObjectPath>(rargs.at(0)).path();
    if (streamPath_.isEmpty()) {
        qWarning() << "WaylandCapturer: mutter RecordMonitor empty stream path";
        return false;
    }
    // 打印 props 看看有没有尺寸信息
    if (rargs.size() > 1) {
        const QVariantMap props = qdbus_cast<QVariantMap>(rargs.at(1));
        qInfo() << "WaylandCapturer: RecordMonitor props keys:" << props.keys();
        const QVariantList size = props.value("size").toList();
        if (size.size() == 2) {
            width_ = size.at(0).toInt();
            height_ = size.at(1).toInt();
            qInfo() << "WaylandCapturer: monitor size from props" << width_.load() << "x" << height_.load();
        }
    }

    // 监听 PipeWireStreamAdded 信号获取 PipeWire node id
    mutterMode_ = true;
    bool sigOk = bus.connect("org.gnome.Mutter.ScreenCast",
                             streamPath_,
                             "org.gnome.Mutter.ScreenCast.Stream",
                             "PipeWireStreamAdded",
                             QStringList(),
                             QString(),
                             this,
                             SLOT(onPipeWireStreamAdded(uint)));
    if (!sigOk) {
        qWarning() << "WaylandCapturer: cannot connect PipeWireStreamAdded signal";
        return false;
    }

    // 启动会话：Start 时 mutter 同步发出 PipeWireStreamAdded
    QDBusMessage sreply = sessionIface.call("Start");
    if (sreply.type() != QDBusMessage::ReplyMessage) {
        qWarning() << "WaylandCapturer: mutter Start failed:" << sreply.errorMessage();
        return false;
    }

    // 等待 node id 通过信号到达（D-Bus 信号在事件循环中分发）
    QDeadlineTimer deadline(5000);
    while (nodeId_ == 0 && !deadline.hasExpired()) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 100);
        QThread::msleep(10);
    }
    if (nodeId_ == 0) {
        qWarning() << "WaylandCapturer: PipeWireStreamAdded timeout (no node id)";
        return false;
    }

    // 尺寸兜底：优先用 QScreen（xcb 平台可拿到真实分辨率）；
    // 若仍拿不到则用 1080p 默认值。协商完成后 parseFormatParam 会用
    // 实际尺寸覆盖并同步 QTRD_WAYLAND_* 环境变量，因此这里只是初始值。
    if (width_.load() <= 0 || height_.load() <= 0) {
        if (QGuiApplication::primaryScreen()) {
            width_ = QGuiApplication::primaryScreen()->size().width();
            height_ = QGuiApplication::primaryScreen()->size().height();
            qInfo() << "WaylandCapturer: using QScreen size" << width_.load() << "x" << height_.load();
        } else {
            width_ = 1920;
            height_ = 1080;
        }
    }

    // 连接默认 PipeWire remote 建流
    return setupPipewireMutter();
}

// ---------------------------------------------------------------------------
// mutter：连接默认 PipeWire remote，stream 绑定到信号给出的 node id
// ---------------------------------------------------------------------------
bool WaylandCapturer::setupPipewireMutter()
{
    loop_ = pw_thread_loop_new("wayland-capture", nullptr);
    if (!loop_) { qWarning() << "WaylandCapturer: pw_thread_loop_new failed"; return false; }
    context_ = pw_context_new(pw_thread_loop_get_loop(loop_), nullptr, 0);
    if (!context_) {
        qWarning() << "WaylandCapturer: pw_context_new failed";
        abortPipewireSetup();
        return false;
    }

    // 先启动 loop 线程，确保事件能被处理（和 portal 路径一致）
    if (pw_thread_loop_start(loop_) < 0) {
        qWarning() << "WaylandCapturer: pw_thread_loop_start failed";
        abortPipewireSetup();
        return false;
    }
    loopStarted_ = true;

    pw_thread_loop_lock(loop_);
    core_ = pw_context_connect(context_, nullptr, 0);
    if (!core_) {
        pw_thread_loop_unlock(loop_);
        qWarning() << "WaylandCapturer: pw_context_connect failed";
        abortPipewireSetup();
        return false;
    }
    bool ok = createStream(nodeId_);
    pw_thread_loop_unlock(loop_);
    if (!ok) {
        abortPipewireSetup();
        return false;
    }

    qInfo() << "WaylandCapturer: PipeWire capture started (mutter), node" << nodeId_
            << "size" << width_.load() << "x" << height_.load();
    return true;
}

void WaylandCapturer::onPipeWireStreamAdded(uint nodeId)
{
    nodeId_ = nodeId;
    qInfo() << "WaylandCapturer: PipeWireStreamAdded node" << nodeId;
}

// ---------------------------------------------------------------------------
// xdg-desktop-portal ScreenCast（回退，带明确 node id）
// ---------------------------------------------------------------------------
static QString portalTokenFile()
{
    return QString::fromUtf8(qgetenv("QTRD_PORTAL_TOKEN_FILE"));
}

static QString loadPortalToken()
{
    const QString path = portalTokenFile();
    if (path.isEmpty())
        return QString();
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly))
        return QString();
    return QString::fromUtf8(f.readAll()).trimmed();
}

static void savePortalToken(const QString& token)
{
    const QString path = portalTokenFile();
    if (path.isEmpty() || token.isEmpty())
        return;
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate))
        return;
    f.write(token.toUtf8());
    f.close();
    ::chmod(qPrintable(path), 0600);
}

// portal Request 对象路径按 handle_token 预测：
//   /org/freedesktop/portal/desktop/request/<发送者唯一名去冒号、点换下划线>/<token>
// 预连接可以让"方法刚返回、Response 已经发出"的极短请求不丢信号。
static QString portalRequestPath(const QString& token)
{
    QString sender = QDBusConnection::sessionBus().baseService();
    if (sender.isEmpty() || token.isEmpty())
        return QString();
    sender.remove(QLatin1Char(':'));
    sender.replace(QLatin1Char('.'), QLatin1Char('_'));
    return QStringLiteral("/org/freedesktop/portal/desktop/request/%1/%2").arg(sender, token);
}

void WaylandCapturer::onPortalResponse(const QDBusMessage& msg)
{
    // Response(u response_code, a{sv} results)，response_code == 0 表示成功/用户同意
    if (msg.arguments().size() < 2) {
        qWarning() << "WaylandCapturer: portal Response signal malformed";
        return;
    }
    portalResponseCode_ = msg.arguments().at(0).toUInt();
    portalResponseResults_ = qdbus_cast<QVariantMap>(msg.arguments().at(1));
    portalResponseReady_ = true;
    if (portalLoop_)
        portalLoop_->quit();
}

bool WaylandCapturer::waitPortalResponse(const QString& requestPath, int timeoutMs,
                                         uint* code, QVariantMap* results)
{
    if (requestPath.isEmpty())
        return false;
    QDBusConnection::sessionBus().connect(
        QStringLiteral("org.freedesktop.portal.Desktop"), requestPath,
        QStringLiteral("org.freedesktop.portal.Request"), QStringLiteral("Response"),
        this, SLOT(onPortalResponse(QDBusMessage)));

    QEventLoop loop;
    QTimer timer;
    timer.setSingleShot(true);
    QObject::connect(&timer, &QTimer::timeout, &loop, &QEventLoop::quit);
    portalLoop_ = &loop;
    timer.start(timeoutMs);
    // 预连接 + 同步 D-Bus call 内部也会跑事件循环，Response 可能已经先到了。
    if (!portalResponseReady_)
        loop.exec();
    portalLoop_ = nullptr;
    timer.stop();

    if (!portalResponseReady_) {
        qWarning() << "WaylandCapturer: portal response timeout (" << timeoutMs
                   << "ms) for" << requestPath;
        return false;
    }
    if (code)
        *code = portalResponseCode_;
    if (results)
        *results = portalResponseResults_;
    return true;
}

bool WaylandCapturer::setupPortalScreenCast()
{
    QDBusConnection bus = QDBusConnection::sessionBus();

    QDBusInterface portal("org.freedesktop.portal.Desktop",
                          "/org/freedesktop/portal/desktop",
                          "org.freedesktop.portal.ScreenCast", bus);
    if (!portal.isValid()) {
        qWarning() << "WaylandCapturer: portal ScreenCast interface not available";
        return false;
    }

    // CreateSession
    // portal 的 CreateSession/SelectSources/Start 都**只**返回 Request 句柄，真正的
    // 结果（session_handle / streams / 授权码）走该 Request 的 Response 信号。
    // 这里按 handle_token 预测路径并预连接，避免"调用刚返回、响应已经发出"被漏听。
    auto armRequest = [this](const QString& token) -> QString {
        const QString path = portalRequestPath(token);
        portalResponseReady_ = false;
        if (!path.isEmpty()) {
            QDBusConnection::sessionBus().connect(
                QStringLiteral("org.freedesktop.portal.Desktop"), path,
                QStringLiteral("org.freedesktop.portal.Request"),
                QStringLiteral("Response"), this, SLOT(onPortalResponse(QDBusMessage)));
        }
        return path;
    };
    auto attachRequest = [this](const QString& path) {
        if (path.isEmpty())
            return;
        QDBusConnection::sessionBus().connect(
            QStringLiteral("org.freedesktop.portal.Desktop"), path,
            QStringLiteral("org.freedesktop.portal.Request"),
            QStringLiteral("Response"), this, SLOT(onPortalResponse(QDBusMessage)));
    };

    uint code = 2;
    QVariantMap results;
    QString requestPath;

    const QString ctoken = QStringLiteral("qtrdcreate%1").arg(QDateTime::currentMSecsSinceEpoch());
    const QString cpredicted = armRequest(ctoken);
    QVariantMap copts;
    copts.insert("handle_token", ctoken);
    // portal 4 的 CreateSession 必须带 session_handle_token（缺了会报 "Missing token"），
    // 它决定 Response 里 session_handle 的对象路径后缀。
    copts.insert("session_handle_token",
                 QStringLiteral("qtrdsession%1").arg(QDateTime::currentMSecsSinceEpoch()));
    QDBusMessage creply = portal.call("CreateSession", copts);
    if (creply.type() != QDBusMessage::ReplyMessage) {
        qWarning() << "WaylandCapturer: portal CreateSession failed:" << creply.errorMessage();
        return false;
    }
    requestPath = creply.arguments().value(0).value<QDBusObjectPath>().path();
    if (requestPath.isEmpty())
        requestPath = creply.arguments().value(0).toString();
    if (requestPath.isEmpty()) {
        qWarning() << "WaylandCapturer: portal CreateSession empty handle";
        return false;
    }
    if (requestPath != cpredicted)
        attachRequest(requestPath);
    if (!waitPortalResponse(requestPath, 15000, &code, &results) || code != 0) {
        qWarning() << "WaylandCapturer: portal CreateSession rejected, code=" << code;
        return false;
    }
    sessionPath_ = results.value("session_handle").value<QDBusObjectPath>().path();
    if (sessionPath_.isEmpty())
        sessionPath_ = results.value("session_handle").toString();
    if (sessionPath_.isEmpty()) {
        qWarning() << "WaylandCapturer: portal CreateSession returned no session handle";
        return false;
    }
    qInfo() << "WaylandCapturer: portal session created:" << sessionPath_;

    // SelectSources: types=1(Monitor), multiple=false
    QVariantMap selOptions;
    selOptions.insert("types", 1u);
    selOptions.insert("multiple", false);
    // 会话代理场景（QTRD_PORTAL_TOKEN_FILE 由代理设置）才启用持久授权：
    // 用户授权一次后，下次登录/代理重启可复用 restore_token，不再弹窗。
    // root 服务的直采路径不设这个环境变量，行为保持原样。
    const QString tokenFile = portalTokenFile();
    const QString savedToken = tokenFile.isEmpty() ? QString() : loadPortalToken();
    if (!tokenFile.isEmpty()) {
        selOptions.insert("persist_mode", 2u);   // 2 = persist until revoked
        if (!savedToken.isEmpty())
            selOptions.insert("restore_token", savedToken);
    }
    const QString stoken = QStringLiteral("qtrdselect%1").arg(QDateTime::currentMSecsSinceEpoch());
    selOptions.insert("handle_token", stoken);
    const QString spredicted = armRequest(stoken);
    QDBusMessage sreply = portal.call("SelectSources",
                                      QVariant::fromValue(QDBusObjectPath(sessionPath_)),
                                      selOptions);
    if (sreply.type() != QDBusMessage::ReplyMessage) {
        qWarning() << "WaylandCapturer: portal SelectSources failed:" << sreply.errorMessage();
        return false;
    }
    requestPath = sreply.arguments().value(0).value<QDBusObjectPath>().path();
    if (requestPath.isEmpty())
        requestPath = sreply.arguments().value(0).toString();
    if (requestPath != spredicted)
        attachRequest(requestPath);
    if (!waitPortalResponse(requestPath, 15000, &code, &results) || code != 0) {
        qWarning() << "WaylandCapturer: portal SelectSources rejected, code=" << code;
        return false;
    }
    qInfo() << "WaylandCapturer: portal SelectSources ok (monitor, persist_mode="
            << (tokenFile.isEmpty() ? 0 : 2) << "restore_token=" << !savedToken.isEmpty() << ")";

    // Start → 首次会弹共享选择窗口，等用户点“共享”；结果同样走 Response 信号
    const QString ttoken = QStringLiteral("qtrdstart%1").arg(QDateTime::currentMSecsSinceEpoch());
    QVariantMap startOptions;
    startOptions.insert("handle_token", ttoken);
    const QString tpredicted = armRequest(ttoken);
    QDBusMessage startReply = portal.call("Start",
                                          QVariant::fromValue(QDBusObjectPath(sessionPath_)),
                                          QString(), startOptions);
    if (startReply.type() != QDBusMessage::ReplyMessage) {
        qWarning() << "WaylandCapturer: portal Start failed:" << startReply.errorMessage();
        return false;
    }
    requestPath = startReply.arguments().value(0).value<QDBusObjectPath>().path();
    if (requestPath.isEmpty())
        requestPath = startReply.arguments().value(0).toString();
    if (requestPath != tpredicted)
        attachRequest(requestPath);
    qInfo() << "WaylandCapturer: portal Start sent, waiting for user authorization (request="
            << requestPath << ")";
    if (!waitPortalResponse(requestPath, 120000, &code, &results) || code != 0) {
        qWarning() << "WaylandCapturer: portal Start rejected, code=" << code
                   << "(cancelled/denied by user)";
        return false;
    }
    // 持久授权 token：只在启用了 persist_mode 的代理路径写盘（0600）。
    qInfo() << "WaylandCapturer: portal Start authorized, result keys=" << results.keys();
    if (!tokenFile.isEmpty()) {
        const QString newToken = results.value("restore_token").toString();
        if (!newToken.isEmpty()) {
            savePortalToken(newToken);
            qInfo() << "WaylandCapturer: portal restore_token saved for reuse";
        }
    }
    // streams 是 a(ua{sv})，用 QtDBus 原生容器反序列化（QDBusArgument 只读）
    uint nodeId = 0;
    QVariantMap sprops;
    const QVariant streamsVar = results.value("streams");
    if (streamsVar.canConvert<QDBusArgument>()) {
        QDBusArgument arg = streamsVar.value<QDBusArgument>();
        QList<QPair<uint, QVariantMap> > streams;
        arg >> streams;
        if (!streams.isEmpty()) {
            nodeId = streams.first().first;
            sprops = streams.first().second;
        }
    } else {
        const QVariantList streams = streamsVar.toList();
        if (!streams.isEmpty()) {
            const QVariantList firstStream = streams.first().toList();
            if (!firstStream.isEmpty()) {
                nodeId = firstStream.at(0).toUInt();
                sprops = qdbus_cast<QVariantMap>(firstStream.at(1));
            }
        }
    }
    if (nodeId == 0) {
        qWarning() << "WaylandCapturer: portal Start returned no streams";
        return false;
    }
    const QVariantList size = sprops.value("size").toList();
    if (size.size() == 2) {
        width_ = size.at(0).toInt();
        height_ = size.at(1).toInt();
    }
    if (width_.load() <= 0 || height_.load() <= 0) {
        // 兜底：用主屏幕尺寸
        if (QGuiApplication::primaryScreen()) {
            width_ = QGuiApplication::primaryScreen()->size().width();
            height_ = QGuiApplication::primaryScreen()->size().height();
        }
    }

    // OpenPipeWireRemote → fd（第二个参数是 a{sv} options，不是 parent_window）
    QDBusMessage freply = portal.call("OpenPipeWireRemote",
                                      QVariant::fromValue(QDBusObjectPath(sessionPath_)),
                                      QVariantMap());
    if (freply.type() != QDBusMessage::ReplyMessage || freply.arguments().isEmpty()) {
        qWarning() << "WaylandCapturer: portal OpenPipeWireRemote failed:" << freply.errorMessage()
                   << "args=" << freply.arguments().size();
        return false;
    }
    QDBusUnixFileDescriptor pfd = qdbus_cast<QDBusUnixFileDescriptor>(freply.arguments().at(0));
    if (!pfd.isValid()) {
        qWarning() << "WaylandCapturer: invalid PipeWire fd from portal";
        return false;
    }

    // 复制一份 fd 交给 PipeWire：QDBusUnixFileDescriptor 析构时会关闭它持有的 fd，
    // 而 pw_context_connect_fd 会接管并最终关闭传入的 fd —— 直接用同一个 fd 会造成
    // 双重关闭，可能误关其它线程刚分配到同号 fd（极难排查的间歇性故障）。
    const int rawFd = ::dup(pfd.fileDescriptor());
    if (rawFd < 0) {
        qWarning() << "WaylandCapturer: dup(PipeWire fd) failed:" << strerror(errno);
        return false;
    }
    return setupPipewire(rawFd, nodeId);
}

// ---------------------------------------------------------------------------
// PipeWire 抓帧
// ---------------------------------------------------------------------------
bool WaylandCapturer::setupPipewire(int fd, uint32_t nodeId)
{
    nodeId_ = nodeId;

    loop_ = pw_thread_loop_new("wayland-capture", nullptr);
    if (!loop_) {
        qWarning() << "WaylandCapturer: pw_thread_loop_new failed";
        ::close(fd);   // 连接失败时 fd 仍归本函数所有，必须关闭（旧实现每次重建漏一个 fd）
        return false;
    }

    context_ = pw_context_new(pw_thread_loop_get_loop(loop_), nullptr, 0);
    if (!context_) {
        qWarning() << "WaylandCapturer: pw_context_new failed";
        ::close(fd);
        abortPipewireSetup();
        return false;
    }

    if (pw_thread_loop_start(loop_) < 0) {
        qWarning() << "WaylandCapturer: pw_thread_loop_start failed";
        ::close(fd);
        abortPipewireSetup();
        return false;
    }
    loopStarted_ = true;

    pw_thread_loop_lock(loop_);
    core_ = pw_context_connect_fd(context_, fd, nullptr, 0);
    if (!core_) {
        pw_thread_loop_unlock(loop_);
        qWarning() << "WaylandCapturer: pw_context_connect_fd failed";
        ::close(fd);
        abortPipewireSetup();
        return false;
    }
    bool ok = createStream(nodeId);
    pw_thread_loop_unlock(loop_);
    if (!ok) {
        abortPipewireSetup();
        return false;
    }

    qInfo() << "WaylandCapturer: PipeWire capture started, node" << nodeId_
            << "size" << width_.load() << "x" << height_.load();
    return true;
}

bool WaylandCapturer::createStream(uint32_t nodeId)
{
    // 调用方须持有 loop 锁（pw_thread_loop_lock）
    // 不约束分辨率，让 PipeWire 自动协商（compositor 会返回实际分辨率）
    uint8_t buffer[1024];
    struct spa_pod_builder b = SPA_POD_BUILDER_INIT(buffer, sizeof(buffer));
    const spa_pod* params[3];

    // 枚举格式：不约束 size，只约束 format
    params[0] = static_cast<spa_pod*>(spa_pod_builder_add_object(&b,
        SPA_TYPE_OBJECT_Format, SPA_PARAM_EnumFormat,
        SPA_FORMAT_mediaType, SPA_POD_Id(SPA_MEDIA_TYPE_video),
        SPA_FORMAT_mediaSubtype, SPA_POD_Id(SPA_MEDIA_SUBTYPE_raw),
        SPA_FORMAT_VIDEO_format, SPA_POD_Id(SPA_VIDEO_FORMAT_BGRx)));
    params[1] = static_cast<spa_pod*>(spa_pod_builder_add_object(&b,
        SPA_TYPE_OBJECT_Format, SPA_PARAM_EnumFormat,
        SPA_FORMAT_mediaType, SPA_POD_Id(SPA_MEDIA_TYPE_video),
        SPA_FORMAT_mediaSubtype, SPA_POD_Id(SPA_MEDIA_SUBTYPE_raw),
        SPA_FORMAT_VIDEO_format, SPA_POD_Id(SPA_VIDEO_FORMAT_BGRA)));
    params[2] = static_cast<spa_pod*>(spa_pod_builder_add_object(&b,
        SPA_TYPE_OBJECT_Format, SPA_PARAM_EnumFormat,
        SPA_FORMAT_mediaType, SPA_POD_Id(SPA_MEDIA_TYPE_video),
        SPA_FORMAT_mediaSubtype, SPA_POD_Id(SPA_MEDIA_SUBTYPE_raw),
        SPA_FORMAT_VIDEO_format, SPA_POD_Id(SPA_VIDEO_FORMAT_RGBx)));

    char targetStr[32];
    snprintf(targetStr, sizeof(targetStr), "%u", nodeId);
    stream_ = pw_stream_new(core_, "QtRemoteDesktop-capture",
                            pw_properties_new(
                                PW_KEY_MEDIA_TYPE, "Video",
                                PW_KEY_MEDIA_CATEGORY, "Capture",
                                PW_KEY_MEDIA_ROLE, "Screen",
                                PW_KEY_NODE_TARGET, targetStr,
                                PW_KEY_NODE_ALWAYS_PROCESS, "true",
                                NULL));
    if (!stream_) {
        qWarning() << "WaylandCapturer: pw_stream_new failed";
        return false;
    }

    streamListener_ = new spa_hook;
    pw_stream_add_listener(stream_, streamListener_, &g_streamEvents, this);

    // AUTOCONNECT + target for proper node registration with PipeWire daemon
    int res = pw_stream_connect(stream_, PW_DIRECTION_INPUT, nodeId,
                                static_cast<pw_stream_flags>(PW_STREAM_FLAG_AUTOCONNECT |
                                                             PW_STREAM_FLAG_MAP_BUFFERS),
                                params, 3);
    if (res < 0) {
        qWarning() << "WaylandCapturer: pw_stream_connect failed:" << spa_strerror(res);
        return false;
    }

    qInfo() << "WaylandCapturer: createStream ok, target node" << nodeId
            << "stream state" << pw_stream_get_state(stream_, nullptr);
    streamReady_ = true;
    return true;
}

bool WaylandCapturer::createLink(uint32_t outputNode, uint32_t outputPort, uint32_t inputNode, uint32_t inputPort)
{
    // 调用方须持有 loop 锁
    // 通过 link-factory 手动创建 link（无 WirePlumber 时 AUTOCONNECT 不自动建 link）
    struct pw_properties* props = pw_properties_new(
        PW_KEY_LINK_OUTPUT_NODE, std::to_string(outputNode).c_str(),
        PW_KEY_LINK_INPUT_NODE, std::to_string(inputNode).c_str(),
        "object.linger", "1",
        nullptr);

    if (outputPort != 0) {
        pw_properties_set(props, PW_KEY_LINK_OUTPUT_PORT, std::to_string(outputPort).c_str());
    }
    if (inputPort != 0) {
        pw_properties_set(props, PW_KEY_LINK_INPUT_PORT, std::to_string(inputPort).c_str());
    }

    struct pw_proxy* linkProxy = static_cast<pw_proxy*>(
        pw_core_create_object(core_, "link-factory",
                              PW_TYPE_INTERFACE_Link, PW_VERSION_LINK,
                              &props->dict, 0));
    pw_properties_free(props);

    if (!linkProxy) {
        qWarning() << "WaylandCapturer: pw_core_create_object link-factory failed";
        return false;
    }
    linkProxy_ = linkProxy;

    linkListener_ = new spa_hook;
    pw_proxy_add_listener(linkProxy_, linkListener_, &g_linkEvents, this);

    qInfo() << "WaylandCapturer: created link" << outputNode << "->" << inputNode;
    return true;
}

void WaylandCapturer::teardownPipewire()
{
    if (loop_) {
        pw_thread_loop_stop(loop_);
        pw_thread_loop_lock(loop_);
        if (stream_) {
            pw_stream_disconnect(stream_);
            if (streamListener_) {
                spa_hook_remove(streamListener_);
                delete streamListener_;
                streamListener_ = nullptr;
            }
            pw_stream_destroy(stream_);
            stream_ = nullptr;
        }
        if (linkProxy_) {
            if (linkListener_) {
                spa_hook_remove(linkListener_);
                delete linkListener_;
                linkListener_ = nullptr;
            }
            pw_proxy_destroy(linkProxy_);
            linkProxy_ = nullptr;
        }
        if (core_) {
            pw_core_disconnect(core_);
            core_ = nullptr;
        }
        if (context_) {
            pw_context_destroy(context_);
            context_ = nullptr;
        }
        pw_thread_loop_unlock(loop_);
        pw_thread_loop_destroy(loop_);
        loop_ = nullptr;
    }
    loopStarted_ = false;
    streamError_ = false;
    streamReady_ = false;
}

void WaylandCapturer::closeSession()
{
    if (sessionPath_.isEmpty())
        return;
    QDBusConnection bus = QDBusConnection::sessionBus();
    // mutter 会话：调 Stop；portal 会话：调 Close。统一 try 两者。
    QDBusInterface sessionIface("org.gnome.Mutter.ScreenCast", sessionPath_,
                                "org.gnome.Mutter.ScreenCast.Session", bus);
    if (sessionIface.isValid())
        sessionIface.call("Stop");
    QDBusInterface portalSession("org.freedesktop.portal.Desktop", sessionPath_,
                                 "org.freedesktop.portal.Session", bus);
    if (portalSession.isValid())
        portalSession.call("Close");
    sessionPath_.clear();
}

void WaylandCapturer::streamProcess()
{
    if (!stream_)
        return;

    // 取一次快照：width_/height_ 是原子字段，避免在多处表达式里反复隐式转换，
    // 也保证本帧的几何计算用同一组宽高。
    const int cfgW = width_.load(std::memory_order_relaxed);
    const int cfgH = height_.load(std::memory_order_relaxed);
    if (cfgW <= 0 || cfgH <= 0)
        return;
    pw_buffer* buf = pw_stream_dequeue_buffer(stream_);
    if (!buf)
        return;
    struct spa_buffer* sbuf = buf->buffer;
    struct spa_data* d = &sbuf->datas[0];
    if (d && d->data) {
        int stride = (d->chunk && d->chunk->stride > 0) ? d->chunk->stride : cfgW * 4;
        // [B5] 协商尺寸（parseFormatParam 设置的 width_/height_）是权威值，QImage 允许
        // bytesPerLine 大于 width*4（stride 对齐填充无害）。不得用 stride/4 反推宽度——
        // mutter 对某些缓冲做行对齐填充时会把 padding 当像素，画面错位且污染
        // InputManager 依赖的 QTRD_WAYLAND_WIDTH。只有两种不一致才降级推导：
        //   a) stride < width_*4：真实行宽比协商窄（mutter 分数缩放，buffer 小于协商）；
        //   b) chunk->size < stride*height_：真实行数比协商少。
        int realW = cfgW;
        int realH = cfgH;
        if (stride < cfgW * 4 && stride > 0 && (stride % 4) == 0)
            realW = stride / 4;
        const qint64 bufSize = (d->chunk && d->chunk->size > 0) ? (qint64)d->chunk->size : 0;
        if (bufSize > 0 && stride > 0 && bufSize < qint64(stride) * cfgH) {
            int h = (int)(bufSize / stride);
            if (h > 0 && h <= 8192)
                realH = h;
        }
        if (realW <= 0 || realH <= 0 || stride < realW * 4) {
            qWarning() << "WaylandCapturer: invalid frame geometry w" << realW
                       << "h" << realH << "stride" << stride << ", skip";
            pw_stream_queue_buffer(stream_, buf);
            return;
        }
        // 尺寸变化时同步内部宽高与共享环境变量（供 InputManager 坐标映射）
        if (realW != cfgW || realH != cfgH) {
            qInfo() << "WaylandCapturer: adopt actual size" << realW << "x" << realH
                    << "(was" << cfgW << "x" << cfgH << ") stride" << stride;
            width_ = realW;
            height_ = realH;
            // 环境变量由主线程在 captureFrame 里通过 syncEnvSize() 同步：
            // 本函数运行在 PipeWire 线程，qputenv 写进程环境块不是线程安全的。
        }
        // [R1] 无人观看时把整帧拷贝从"每帧"降到 1fps。PipeWire 的帧生产在
        // 本线程持续运行（光标闪烁/面板刷新会不断出帧），1920x1200 RGBA 每帧
        // ~9MB 的 copy+堆分配在 30fps 下 ≈ 270MB/s，纯属白耗。
        // 不能完全停：resume 后首帧直接从 frame_ 取，完全停会让重连客户端看到
        // 暂停前的旧画面（锁屏等一次性变化会一直显示不对）。1fps 下 frame_
        // 最多陈旧 1s，按帧速率看等价于实时。
        if (paused_.load(std::memory_order_relaxed)) {
            const qint64 nowMs = QDateTime::currentMSecsSinceEpoch();
            if (lastPausedCopyMs_ != 0 && nowMs - lastPausedCopyMs_ < 1000) {
                pw_stream_queue_buffer(stream_, buf);
                return;
            }
            lastPausedCopyMs_ = nowMs;
        }
        QImage img(static_cast<const uchar*>(d->data),
                   realW, realH,
                   stride,
                   QImage::Format_RGB32);
        if (img.isNull()) {
            qWarning() << "WaylandCapturer: QImage construct failed, skip frame (w" << realW
                       << "h" << realH << "stride" << stride << ")";
            pw_stream_queue_buffer(stream_, buf);
            return;
        }
        QMutexLocker lk(&frameMutex_);
        frame_ = img.copy();
        hasFrame_ = true;
        const uint64_t fc = frameCount_.fetch_add(1) + 1;
        // 周期性打印（每 300 帧一次），避免高频日志刷爆日志文件
        if (fc <= 3 || (fc % 300) == 1) {
            qInfo() << "WaylandCapturer: frame" << fc
                    << "size" << realW << "x" << realH
                    << "stride" << stride
                    << "state" << pw_stream_get_state(stream_, nullptr);
        }
    }
    pw_stream_queue_buffer(stream_, buf);
}

void WaylandCapturer::parseFormatParam(const struct spa_pod* param)
{
    // 必须解析到 spa_video_info_raw：旧代码声明 struct spa_video_info info; 后
    // 只填 media_type/media_subtype，就直接读 info.info.raw.size.*（联合体成员从未
    // 初始化）→ 未定义行为，得到随机宽高并写进 QTRD_WAYLAND_WIDTH/HEIGHT，
    // 污染 InputManager 的坐标映射。
    struct spa_video_info_raw raw;
    memset(&raw, 0, sizeof(raw));
    uint32_t mediaType = 0;
    uint32_t mediaSubtype = 0;
    if (spa_format_parse(param, &mediaType, &mediaSubtype) < 0)
        return;
    if (mediaType != SPA_MEDIA_TYPE_video || mediaSubtype != SPA_MEDIA_SUBTYPE_raw)
        return;
    if (spa_format_video_raw_parse(param, &raw) < 0)
        return;
    if (raw.size.width > 0 && raw.size.height > 0) {
        const int w = static_cast<int>(raw.size.width);
        const int h = static_cast<int>(raw.size.height);
        width_ = w;
        height_ = h;
        qInfo() << "WaylandCapturer: negotiated format" << w << "x" << h;
        // 协商后的实际尺寸同步给 InputManager 做坐标映射（offscreen 平台无 QScreen）。
        // 这里在 PipeWire 线程执行，不能调 qputenv（环境块非线程安全），
        // 由主线程在 captureFrame 里 syncEnvSize() 落盘。
    }
}

void WaylandCapturer::abortPipewireSetup()
{
    // 只在 loop 已启动时走常规 teardown（pw_thread_loop_stop 对未启动的 loop
    // 行为不保证）；否则手工销毁已创建的对象。
    if (loopStarted_) {
        teardownPipewire();
        return;
    }
    if (stream_) { pw_stream_destroy(stream_); stream_ = nullptr; }
    if (core_) { pw_core_disconnect(core_); core_ = nullptr; }
    if (context_) { pw_context_destroy(context_); context_ = nullptr; }
    if (loop_) { pw_thread_loop_destroy(loop_); loop_ = nullptr; }
}

void WaylandCapturer::syncEnvSize()
{
    // 只在主线程调用：qputenv 写的是进程级环境块，PipeWire 线程并发写会破坏
    // 环境变量的链表/字符串（glibc setenv 非线程安全）。
    const int w = width_.load(std::memory_order_relaxed);
    const int h = height_.load(std::memory_order_relaxed);
    if (w == lastEnvWidth_ && h == lastEnvHeight_)
        return;
    lastEnvWidth_ = w;
    lastEnvHeight_ = h;
    qputenv("QTRD_WAYLAND_WIDTH", QByteArray::number(w));
    qputenv("QTRD_WAYLAND_HEIGHT", QByteArray::number(h));
}

void WaylandCapturer::setPaused(bool paused)
{
    // 只置标志：帧丢弃/降频由 PipeWire 线程在 streamProcess 里执行，
    // 这样暂停不会阻塞主线程（不能在主线程 stop 流，重建/销毁会死锁）。
    paused_.store(paused, std::memory_order_relaxed);
}

bool WaylandCapturer::captureFrame(QImage& outImage, bool* updated)
{
    // PipeWire 线程只更新 width_/height_，这里（主线程）把变化同步到
    // QTRD_WAYLAND_* 环境变量，供 InputManager 做坐标映射。
    syncEnvSize();

    // PipeWire 流已进入 ERROR（compositor 撤销共享 / 会话失效 / 设备错误）：
    // 如实报失败，让上层走"采集失败"路径（计数、必要时重建），而不是继续
    // 返回 true，让画面永久冻结在最后一帧而看不出任何异常。
    if (streamError_) {
        // ERROR 是终态，PipeWire 不会自己恢复：按 5s 节流重建一次流（teardown
        // 会清 streamError_），否则 compositor 撤销共享后画面永久冻结。
        const qint64 nowMs = QDateTime::currentMSecsSinceEpoch();
        if (streamErrorRetryMs_ == 0 || nowMs - streamErrorRetryMs_ >= 5000) {
            streamErrorRetryMs_ = nowMs;
            qInfo() << "WaylandCapturer: stream ERROR, rebuilding PipeWire stream (attempt)";
            teardownPipewire();
            closeSession();
            initialized_ = false;
            hasFrame_ = false;
            if (initialize()) {
                streamErrorRetryMs_ = 0;
                qInfo() << "WaylandCapturer: PipeWire stream rebuilt";
            } else {
                // teardownPipewire() 已把 streamError_ 清零；若这里不重新置位，
                // 上面 5s 重建门禁永远不会再进入 → compositor 撤销共享后画面
                // 永久冻结，只能重启进程。
                streamError_ = true;
                qWarning() << "WaylandCapturer: PipeWire rebuild failed, will retry in 5s";
            }
        }
        if (updated) *updated = false;
        return false;
    }

    QMutexLocker lk(&frameMutex_);
    // 空帧 / 0 尺寸帧保护：流初始化或格式重协商中可能提交 0 宽高缓冲，
    // 此时不视为有效帧（避免上层 quickFrameChecksum 除零崩溃）
    if (!hasFrame_ || frame_.isNull() || frame_.width() <= 0 || frame_.height() <= 0) {
        static int dbg = 0;
        if (dbg < 10) {
            dbg++;
            qWarning() << "WaylandCapturer::captureFrame FAIL hasFrame_=" << hasFrame_.load()
                       << "isNull=" << frame_.isNull()
                       << "w=" << frame_.width() << "h=" << frame_.height()
                       << "streamReady_=" << streamReady_
                       << "activated_=" << activated_.load()
                       << "frameCount_=" << frameCount_.load();
        }
        if (updated) *updated = false;
        return false;
    }
    // [P1 perf] 帧未更新时直接返回：静止桌面 PipeWire 不产新帧，旧逻辑每 tick
    // 仍 outImage = frame_.copy() 白拷一整帧 8.3MB（30fps ≈ +250MB/s 带宽）。
    const uint64_t fc = frameCount_.load(std::memory_order_relaxed);
    if (fc == lastDeliveredFrame_) {
        // 无新帧时也把缓存帧交给上层：强制帧路径（新客户端接入 / request_keyframe）
        // 需要它，否则静止桌面下 PipeWire 不产新帧，客户端一直拿不到首帧。
        // QImage 为 COW 浅共享，这里不产生像素拷贝。
        outImage = frame_;
        if (updated) *updated = false;
        return true; // 采集通道正常，仅无新帧（与 X11 damage 空帧语义一致）
    }
    lastDeliveredFrame_ = fc;
    // QImage 是 COW 浅共享：这里不发生深拷贝；仅当消费方写像素时才在写侧触发 detach。
    // streamProcess 写 frame_ 是整块替换（不写共享数据），不会强制消费方 detach。
    outImage = frame_;
    if (updated) *updated = true;
    return true;
}
