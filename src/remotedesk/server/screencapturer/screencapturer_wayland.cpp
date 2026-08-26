// server/screencapturer_wayland.cpp
// Wayland 屏幕捕获实现（GNOME Mutter ScreenCast 优先，xdg-desktop-portal 回退）
#include "screencapturer_wayland.h"

#include <QCoreApplication>
#include <QDeadlineTimer>
#include <QDebug>
#include <QDBusConnection>
#include <QDBusInterface>
#include <QDBusMessage>
#include <QDBusObjectPath>
#include <QDBusReply>
#include <QDBusUnixFileDescriptor>
#include <QEventLoop>
#include <QGuiApplication>
#include <QList>
#include <QPair>
#include <QScreen>
#include <QThread>
#include <QVariantMap>

#include <pipewire/pipewire.h>
#include <spa/param/video/format-utils.h>
#include <spa/param/video/raw.h>
#include <spa/param/video/raw-utils.h>
#include <spa/param/format-utils.h>
#include <spa/pod/builder.h>
#include <spa/pod/vararg.h>
#include <spa/utils/result.h>

// ---------------------------------------------------------------------------
// PipeWire 事件回调
// ---------------------------------------------------------------------------
namespace {

void onStreamProcess(void* data)
{
    static_cast<WaylandCapturer*>(data)->streamProcess();
}

void onStreamParamChanged(void* data, uint32_t id, const struct spa_pod* param)
{
    auto* self = static_cast<WaylandCapturer*>(data);
    if (id == SPA_PARAM_Format) {
        if (param) {
            self->parseFormatParam(param);
            qInfo() << "WaylandCapturer: param_changed Format, size" << self->width() << "x" << self->height();
        } else {
            qInfo() << "WaylandCapturer: param_changed Format (cleared)";
        }
    } else {
        qInfo() << "WaylandCapturer: param_changed id" << id;
    }
}

void onStreamStateChanged(void* data, enum pw_stream_state old, enum pw_stream_state state, const char* error)
{
    Q_UNUSED(data)
    Q_UNUSED(old)
    qInfo() << "WaylandCapturer: stream state" << state << "error:" << (error ? error : "none");
}

const pw_stream_events g_streamEvents = {
    .version = PW_VERSION_STREAM_EVENTS,
    .destroy = nullptr,
    .state_changed = onStreamStateChanged,
    .control_info = nullptr,
    .io_changed = nullptr,
    .param_changed = onStreamParamChanged,
    .add_buffer = nullptr,
    .remove_buffer = nullptr,
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

    pw_init(nullptr, nullptr);

    // 1) GNOME Mutter 私有 ScreenCast（无需交互授权）
    if (setupMutterScreenCast()) {
        initialized_ = true;
        // offscreen 平台下 QGuiApplication 拿不到真实分辨率，共享给 InputManager 做坐标映射
        qputenv("QTRD_WAYLAND_WIDTH", QByteArray::number(width_));
        qputenv("QTRD_WAYLAND_HEIGHT", QByteArray::number(height_));
        return true;
    }

    // 2) 回退：标准 xdg-desktop-portal ScreenCast（可能需要用户确认）
    qWarning() << "WaylandCapturer: Mutter ScreenCast unavailable, falling back to portal...";
    if (setupPortalScreenCast()) {
        initialized_ = true;
        qputenv("QTRD_WAYLAND_WIDTH", QByteArray::number(width_));
        qputenv("QTRD_WAYLAND_HEIGHT", QByteArray::number(height_));
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
            qInfo() << "WaylandCapturer: monitor size from props" << width_ << "x" << height_;
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

    // TODO(临时验证): mutter 需要显式 size 才能协商，先硬编码实际分辨率
    if (width_ <= 0 || height_ <= 0) {
        width_ = 1714;
        height_ = 918;
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
    if (!context_) { qWarning() << "WaylandCapturer: pw_context_new failed"; return false; }

    // 先启动 loop（core 连接是异步的，需要 loop 处理事件）
    if (pw_thread_loop_start(loop_) < 0) {
        qWarning() << "WaylandCapturer: pw_thread_loop_start failed";
        return false;
    }

    pw_thread_loop_lock(loop_);
    core_ = pw_context_connect(context_, nullptr, 0);  // 默认 PipeWire socket
    if (!core_) {
        pw_thread_loop_unlock(loop_);
        qWarning() << "WaylandCapturer: pw_context_connect failed";
        return false;
    }
    // core 连接为异步；pw_stream_connect 的请求会在 core 就绪后由 loop 处理
    bool ok = createStream(nodeId_);
    pw_thread_loop_unlock(loop_);
    if (!ok)
        return false;
    qInfo() << "WaylandCapturer: PipeWire capture started (mutter), node" << nodeId_
            << "size" << width_ << "x" << height_;
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
    QDBusMessage creply = portal.call("CreateSession", QVariantMap());
    if (creply.type() != QDBusMessage::ReplyMessage) {
        qWarning() << "WaylandCapturer: portal CreateSession failed:" << creply.errorMessage();
        return false;
    }
    const QVariantList cargs = creply.arguments();
    if (cargs.isEmpty()) return false;
    sessionPath_ = qdbus_cast<QDBusObjectPath>(cargs.at(0)).path();
    if (sessionPath_.isEmpty())
        sessionPath_ = cargs.at(0).toString();
    if (sessionPath_.isEmpty()) {
        qWarning() << "WaylandCapturer: portal CreateSession empty handle";
        return false;
    }

    // SelectSources: types=1(Monitor), multiple=false
    QVariantMap selOptions;
    selOptions.insert("types", 1u);
    selOptions.insert("multiple", false);
    QDBusMessage sreply = portal.call("SelectSources",
                                      QVariant::fromValue(QDBusObjectPath(sessionPath_)),
                                      selOptions);
    if (sreply.type() != QDBusMessage::ReplyMessage) {
        qWarning() << "WaylandCapturer: portal SelectSources failed:" << sreply.errorMessage();
        return false;
    }

    // Start → streams 里有 node id
    QDBusMessage startReply = portal.call("Start",
                                          QVariant::fromValue(QDBusObjectPath(sessionPath_)),
                                          QString(), QVariantMap());
    if (startReply.type() != QDBusMessage::ReplyMessage) {
        qWarning() << "WaylandCapturer: portal Start failed:" << startReply.errorMessage();
        return false;
    }
    const QVariantList sargs = startReply.arguments();
    if (sargs.size() < 2) return false;
    const uint response = sargs.at(0).toUInt();
    if (response != 0) {
        qWarning() << "WaylandCapturer: portal Start rejected (need interactive authorization?)";
        return false;
    }
    const QVariantMap results = qdbus_cast<QVariantMap>(sargs.at(1));
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
    if (width_ <= 0 || height_ <= 0) {
        // 兜底：用主屏幕尺寸
        if (QGuiApplication::primaryScreen()) {
            width_ = QGuiApplication::primaryScreen()->size().width();
            height_ = QGuiApplication::primaryScreen()->size().height();
        }
    }

    // OpenPipeWireRemote → fd
    QDBusMessage freply = portal.call("OpenPipeWireRemote",
                                      QVariant::fromValue(QDBusObjectPath(sessionPath_)),
                                      QString());
    if (freply.type() != QDBusMessage::ReplyMessage || freply.arguments().isEmpty()) {
        qWarning() << "WaylandCapturer: portal OpenPipeWireRemote failed";
        return false;
    }
    QDBusUnixFileDescriptor pfd = qdbus_cast<QDBusUnixFileDescriptor>(freply.arguments().at(0));
    if (!pfd.isValid()) {
        qWarning() << "WaylandCapturer: invalid PipeWire fd from portal";
        return false;
    }

    return setupPipewire(pfd.fileDescriptor(), nodeId);
}

// ---------------------------------------------------------------------------
// PipeWire 抓帧
// ---------------------------------------------------------------------------
bool WaylandCapturer::setupPipewire(int fd, uint32_t nodeId)
{
    nodeId_ = nodeId;

    loop_ = pw_thread_loop_new("wayland-capture", nullptr);
    if (!loop_) { qWarning() << "WaylandCapturer: pw_thread_loop_new failed"; return false; }

    context_ = pw_context_new(pw_thread_loop_get_loop(loop_), nullptr, 0);
    if (!context_) { qWarning() << "WaylandCapturer: pw_context_new failed"; return false; }

    if (pw_thread_loop_start(loop_) < 0) {
        qWarning() << "WaylandCapturer: pw_thread_loop_start failed";
        return false;
    }

    pw_thread_loop_lock(loop_);
    core_ = pw_context_connect_fd(context_, fd, nullptr, 0);
    if (!core_) {
        pw_thread_loop_unlock(loop_);
        qWarning() << "WaylandCapturer: pw_context_connect_fd failed";
        return false;
    }
    bool ok = createStream(nodeId);
    pw_thread_loop_unlock(loop_);
    if (!ok)
        return false;

    qInfo() << "WaylandCapturer: PipeWire capture started, node" << nodeId_
            << "size" << width_ << "x" << height_;
    return true;
}

bool WaylandCapturer::createStream(uint32_t nodeId)
{
    // 调用方须持有 loop 锁（pw_thread_loop_lock）
    // mutter ScreenCast 输出 BGRx（与 QImage::Format_RGB32 小端布局一致）
    uint8_t buffer[1024];
    struct spa_pod_builder b = SPA_POD_BUILDER_INIT(buffer, sizeof(buffer));
    const spa_pod* params[2];
    params[0] = buildVideoFormat(&b, SPA_VIDEO_FORMAT_BGRx, width_, height_);
    params[1] = buildVideoFormat(&b, SPA_VIDEO_FORMAT_BGRA, width_, height_);

    stream_ = pw_stream_new(core_, "QtRemoteDesktop-capture",
                            pw_properties_new(
                                PW_KEY_MEDIA_TYPE, "Video",
                                PW_KEY_MEDIA_CATEGORY, "Capture",
                                PW_KEY_MEDIA_ROLE, "Screen",
                                NULL));
    if (!stream_) {
        qWarning() << "WaylandCapturer: pw_stream_new failed";
        return false;
    }

    streamListener_ = new spa_hook;
    pw_stream_add_listener(stream_, streamListener_, &g_streamEvents, this);

    int res = pw_stream_connect(stream_, PW_DIRECTION_INPUT, nodeId,
                                static_cast<pw_stream_flags>(PW_STREAM_FLAG_AUTOCONNECT |
                                                             PW_STREAM_FLAG_MAP_BUFFERS),
                                params, 2);
    if (res < 0) {
        qWarning() << "WaylandCapturer: pw_stream_connect failed:" << spa_strerror(res);
        return false;
    }
    // 激活 stream：否则停留在 paused，不会触发格式协商
    pw_stream_set_active(stream_, true);
    streamReady_ = true;
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
    // 等 format 协商完成拿到尺寸后再取帧
    if (width_ <= 0 || height_ <= 0)
        return;
    pw_buffer* buf = pw_stream_dequeue_buffer(stream_);
    if (!buf)
        return;
    struct spa_buffer* sbuf = buf->buffer;
    struct spa_data* d = &sbuf->datas[0];
    if (d && d->data) {
        // 32bpp：BGRA/RGBx 均可按 Format_RGB32（小端 BGRA）包装；
        // 若实际为 RGBx，颜色通道会是反的，但布局安全（4 字节/像素）。
        int stride = (d->chunk && d->chunk->stride > 0) ? d->chunk->stride : width_ * 4;
        QImage img(static_cast<const uchar*>(d->data),
                   width_, height_,
                   stride,
                   QImage::Format_RGB32);
        QMutexLocker lk(&frameMutex_);
        frame_ = img.copy();
        hasFrame_ = true;
        frameCount_++;
    }
    pw_stream_queue_buffer(stream_, buf);
}

void WaylandCapturer::parseFormatParam(const struct spa_pod* param)
{
    struct spa_video_info info;
    if (spa_format_parse(param, &info.media_type, &info.media_subtype) < 0)
        return;
    if (info.media_type == SPA_MEDIA_TYPE_video &&
        info.media_subtype == SPA_MEDIA_SUBTYPE_raw &&
        info.info.raw.size.width > 0 && info.info.raw.size.height > 0) {
        width_ = info.info.raw.size.width;
        height_ = info.info.raw.size.height;
        qInfo() << "WaylandCapturer: negotiated format" << width_ << "x" << height_;
    }
}

bool WaylandCapturer::captureFrame(QImage& outImage, bool* updated)
{
    QMutexLocker lk(&frameMutex_);
    if (!hasFrame_) {
        if (updated) *updated = false;
        return false;
    }
    outImage = frame_.copy();
    if (updated) *updated = true;
    return true;
}
