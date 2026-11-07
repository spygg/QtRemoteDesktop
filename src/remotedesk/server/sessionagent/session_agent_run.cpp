// server/sessionagent/session_agent_run.cpp
//
// 会话代理进程（用户会话内运行）：
//   * 用 WaylandCapturer（Mutter ScreenCast / portal）拿到**真实用户桌面**；
//   * 用 InputManager（强制 uinput）驱动内核设备；
//   * 作为 unix socket 服务端接受 root 服务的连接，帧走 /dev/shm 双槽，
//     输入用 JSON 转发。
//
// 关键点：这不是 root 服务自己 fork 出来的子进程，而是 systemd user unit
// 在登录会话里拉起的独立进程 —— 只有这样才能拿到 session bus / PipeWire /
// portal 的用户授权。
#include "session_agent_run.h"
#include "session_agent_protocol.h"
#include "inputmanager.h"
#include "screencapturer_wayland.h"

#include <QCoreApplication>
#include <QDateTime>
#include <QDebug>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLocalServer>
#include <QLocalSocket>
#include <QObject>
#include <QTimer>

#include <cerrno>
#include <csignal>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <pwd.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include <memory>

#ifdef HAVE_PIPEWIRE

namespace {

volatile sig_atomic_t g_quitRequested = 0;
void onQuitSignal(int) { g_quitRequested = 1; }

QString userName()
{
    if (const passwd* pw = ::getpwuid(::getuid()))
        return QString::fromUtf8(pw->pw_name);
    return QStringLiteral("$USER");
}

// systemd user unit 一定有 XDG_RUNTIME_DIR；手工启动时也可能没有。
bool ensureWaylandEnv()
{
    const QByteArray rt = qgetenv("XDG_RUNTIME_DIR");
    if (rt.isEmpty())
        return false;

    QByteArray cur = qgetenv("WAYLAND_DISPLAY");
    if (!cur.isEmpty() && QFileInfo::exists(QString::fromUtf8(rt) + "/" + QString::fromUtf8(cur)))
        return true;

    const QDir dir(QString::fromUtf8(rt));
    const QStringList socks = dir.entryList(QStringList() << QStringLiteral("wayland-*"),
                                            QDir::System | QDir::Files);
    for (const QString& s : socks) {
        if (s.endsWith(QLatin1String(".lock")))
            continue;
        qputenv("WAYLAND_DISPLAY", s.toUtf8());
        return true;
    }
    return false;
}

void ensureSessionBusEnv()
{
    if (!qgetenv("DBUS_SESSION_BUS_ADDRESS").isEmpty())
        return;
    const QString bus = QStringLiteral("/run/user/%1/bus").arg(::getuid());
    if (QFileInfo::exists(bus))
        qputenv("DBUS_SESSION_BUS_ADDRESS", ("unix:path=" + bus).toUtf8());
}

// portal 授权复用：token 只落在用户自己的 state 目录（0600）。
void setupPortalTokenEnv()
{
    const QByteArray home = qgetenv("HOME");
    if (home.isEmpty())
        return;
    const QByteArray stateEnv = qgetenv("XDG_STATE_HOME");
    QString stateRoot = stateEnv.isEmpty()
        ? QString::fromUtf8(home) + QStringLiteral("/.local/state")
        : QString::fromUtf8(stateEnv);
    const QString dir = stateRoot + QStringLiteral("/remotedesk");
    QDir().mkpath(dir);
    ::chmod(qPrintable(dir), 0700);
    qputenv("QTRD_PORTAL_TOKEN_FILE", (dir + QStringLiteral("/portal_token")).toUtf8());
}

class AgentHost : public QObject
{
public:
    AgentHost(QCoreApplication& app, std::unique_ptr<WaylandCapturer> capturer,
              std::unique_ptr<InputManager> input, int width, int height)
        : app_(app), capturer_(std::move(capturer)), input_(std::move(input)),
          width_(width), height_(height)
    {
        uid_ = ::getuid();
        pid_ = static_cast<int>(::getpid());
        shmPath_ = SessionAgent::shmPath(uid_, pid_);
    }

    ~AgentHost() override { cleanupFiles(); }

    bool start()
    {
        // 单实例：socket 仍可连接说明代理已经在跑（例如 install 后手工再启一次）。
        const QString sock = SessionAgent::socketPath();
        if (QFileInfo::exists(sock)) {
            QLocalSocket probe;
            probe.connectToServer(sock);
            if (probe.waitForConnected(200)) {
                qInfo() << "SessionAgent: another agent is already running at" << sock
                        << "- exiting";
                return false;
            }
        }

        QDir().mkpath(SessionAgent::socketDir());
        ::chmod(qPrintable(SessionAgent::socketDir()), 0700);
        QLocalServer::removeServer(sock);
        if (!server_.listen(sock)) {
            qWarning() << "SessionAgent: cannot listen on" << sock << ":" << server_.errorString();
            return false;
        }
        ::chmod(qPrintable(sock), 0600);

        connect(&server_, &QLocalServer::newConnection, this, [this]() { onNewConnection(); });

        frameTimer_.setInterval(33);   // ~30fps
        connect(&frameTimer_, &QTimer::timeout, this, [this]() { onFrameTick(); });
        frameTimer_.start();

        watchTimer_.setInterval(200);
        connect(&watchTimer_, &QTimer::timeout, this, [this]() {
            if (g_quitRequested)
                app_.quit();
        });
        watchTimer_.start();

        qInfo() << "SessionAgent: ready at" << sock
                << "geometry=" << width_ << "x" << height_;
        return true;
    }

private:
    void onNewConnection()
    {
        QLocalSocket* s = server_.nextPendingConnection();
        if (!s)
            return;

        if (client_) {
            client_->disconnect(this);
            client_->abort();
            client_->deleteLater();
        }
        client_ = s;
        readBuf_.clear();
        for (bool& p : pending_)
            p = false;

        connect(client_, &QLocalSocket::readyRead, this, [this]() { onClientRead(); });
        connect(client_, &QLocalSocket::disconnected, this, [this, s]() {
            if (client_ == s) {
                client_ = nullptr;
                for (bool& p : pending_)
                    p = false;
                readBuf_.clear();
                if (capturer_)
                    capturer_->setPaused(true);
                qInfo() << "SessionAgent: root service disconnected; pausing capture";
            }
        });

        if (capturer_)
            capturer_->setPaused(false);

        sendHello();
        if (shmBase_)
            sendShm();
        if (!lastImage_.isNull())
            sendFrame(lastImage_);
        qInfo() << "SessionAgent: root service connected";
    }

    void sendHello()
    {
        QJsonObject o;
        o.insert(QStringLiteral("t"), QStringLiteral("hello"));
        o.insert(QStringLiteral("ver"), SessionAgent::kProtocolVersion);
        o.insert(QStringLiteral("uid"), uid_);
        o.insert(QStringLiteral("pid"), pid_);
        o.insert(QStringLiteral("capture"), QStringLiteral("wayland"));
        o.insert(QStringLiteral("input"),
                 input_ && input_->isUinputReady() ? QStringLiteral("uinput")
                                                   : QStringLiteral("none"));
        o.insert(QStringLiteral("width"), width_);
        o.insert(QStringLiteral("height"), height_);
        sendJson(o);
    }

    void sendShm()
    {
        QJsonObject o;
        o.insert(QStringLiteral("t"), QStringLiteral("shm"));
        o.insert(QStringLiteral("path"), shmPath_);
        o.insert(QStringLiteral("slotBytes"), slotBytes_);
        sendJson(o);
    }

    void sendJson(const QJsonObject& o)
    {
        if (!client_ || client_->state() != QLocalSocket::ConnectedState)
            return;
        QByteArray line = QJsonDocument(o).toJson(QJsonDocument::Compact);
        line.append('\n');
        client_->write(line);
    }

    bool ensureShm(int needBytes)
    {
        if (shmBase_ && needBytes <= slotBytes_)
            return true;
        if (needBytes <= 0 || needBytes > SessionAgent::kMaxFrameBytes)
            return false;

        unmapShm();
        const QString diag = shmPath_;

        const int fd = ::open(qPrintable(diag), O_RDWR | O_CREAT | O_TRUNC, 0600);
        if (fd < 0) {
            qWarning() << "SessionAgent: cannot create shm" << diag << strerror(errno);
            return false;
        }
        const size_t total = static_cast<size_t>(needBytes) * SessionAgent::kSlots;
        if (::ftruncate(fd, static_cast<off_t>(total)) != 0) {
            qWarning() << "SessionAgent: ftruncate failed for" << diag;
            ::close(fd);
            return false;
        }
        void* p = ::mmap(nullptr, total, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
        if (p == MAP_FAILED) {
            qWarning() << "SessionAgent: mmap failed for" << diag;
            ::close(fd);
            return false;
        }
        shmFd_ = fd;
        shmBase_ = static_cast<uchar*>(p);
        slotBytes_ = needBytes;
        for (bool& pb : pending_)
            pb = false;
        sendShm();
        qInfo() << "SessionAgent: frame ring ready" << diag << "slotBytes=" << slotBytes_;
        return true;
    }

    void unmapShm()
    {
        if (shmBase_) {
            ::munmap(shmBase_, static_cast<size_t>(slotBytes_) * SessionAgent::kSlots);
            shmBase_ = nullptr;
        }
        if (shmFd_ >= 0) {
            ::close(shmFd_);
            shmFd_ = -1;
        }
        slotBytes_ = 0;
    }

    void sendFrame(const QImage& img)
    {
        if (!client_ || img.isNull())
            return;
        const int stride = img.bytesPerLine();
        const int need = stride * img.height();
        if (!ensureShm(need))
            return;

        int slot = -1;
        for (int i = 0; i < SessionAgent::kSlots; ++i) {
            if (!pending_[i]) {
                slot = i;
                break;
            }
        }
        if (slot < 0)
            return;   // 两槽都还没被 root ack：丢帧，避免覆盖正在拷贝的像素

        memcpy(shmBase_ + static_cast<size_t>(slot) * slotBytes_, img.constBits(),
               static_cast<size_t>(need));
        pending_[slot] = true;

        QJsonObject o;
        o.insert(QStringLiteral("t"), QStringLiteral("frame"));
        o.insert(QStringLiteral("seq"), static_cast<double>(++seq_));
        o.insert(QStringLiteral("slot"), slot);
        o.insert(QStringLiteral("w"), img.width());
        o.insert(QStringLiteral("h"), img.height());
        o.insert(QStringLiteral("stride"), stride);
        sendJson(o);
    }

    void onFrameTick()
    {
        if (!capturer_ || !client_)
            return;
        QImage img;
        bool updated = false;
        if (!capturer_->captureFrame(img, &updated) || img.isNull())
            return;
        if (img.width() != width_ || img.height() != height_) {
            width_ = img.width();
            height_ = img.height();
            if (input_) {
                input_->setScreenSize(width_, height_);
                qputenv("QTRD_WAYLAND_WIDTH", QByteArray::number(width_));
                qputenv("QTRD_WAYLAND_HEIGHT", QByteArray::number(height_));
            }
            qInfo() << "SessionAgent: capture size changed to" << width_ << "x" << height_;
        }
        lastImage_ = img;
        // 静止桌面同样保留 lastImage_，新 root 连接时直接补发。
        if (updated || !sentOnce_) {
            sentOnce_ = true;
            sendFrame(img);
        }
    }

    void onClientRead()
    {
        if (!client_)
            return;
        readBuf_ += client_->readAll();
        for (;;) {
            const int idx = readBuf_.indexOf('\n');
            if (idx < 0)
                break;
            const QByteArray line = readBuf_.left(idx).trimmed();
            readBuf_.remove(0, idx + 1);
            if (line.isEmpty())
                continue;
            const QJsonDocument doc = QJsonDocument::fromJson(line);
            if (doc.isObject())
                handleControl(doc.object());
        }
    }

    void handleControl(const QJsonObject& o)
    {
        const QString t = o.value(QStringLiteral("t")).toString();
        if (t == QLatin1String("ack")) {
            const int slot = o.value(QStringLiteral("slot")).toInt(-1);
            if (slot >= 0 && slot < SessionAgent::kSlots)
                pending_[slot] = false;
            return;
        }
        if (t == QLatin1String("ping")) {
            QJsonObject p;
            p.insert(QStringLiteral("t"), QStringLiteral("pong"));
            sendJson(p);
            return;
        }
        if (t == QLatin1String("need_frame")) {
            if (!lastImage_.isNull())
                sendFrame(lastImage_);
            return;
        }
        if (t == QLatin1String("pause")) {
            if (capturer_)
                capturer_->setPaused(o.value(QStringLiteral("on")).toBool());
            return;
        }
        if (t != QLatin1String("input") || !input_)
            return;

        const QString kind = o.value(QStringLiteral("k")).toString();
        if (kind == QLatin1String("move")) {
            input_->injectMouseMove(o.value(QStringLiteral("x")).toInt(),
                                    o.value(QStringLiteral("y")).toInt());
        } else if (kind == QLatin1String("button")) {
            input_->injectMouseButton(o.value(QStringLiteral("x")).toInt(),
                                      o.value(QStringLiteral("y")).toInt(),
                                      o.value(QStringLiteral("b")).toInt(),
                                      o.value(QStringLiteral("down")).toBool());
        } else if (kind == QLatin1String("wheel")) {
            input_->injectWheel(o.value(QStringLiteral("d")).toInt());
        } else if (kind == QLatin1String("key")) {
            if (o.value(QStringLiteral("char")).toBool()
                && o.value(QStringLiteral("keycode")).toInt() > 0x7f) {
                // uinput 只会按物理键码注入，无法把任意 Unicode 码点写进 Wayland
                // 合成器；这里明确记录一次，避免表现为"无声丢字"。
                if (!unicodeWarned_) {
                    qWarning() << "SessionAgent: non-ASCII text input is not supported "
                                  "through uinput; character dropped";
                    unicodeWarned_ = true;
                }
                return;
            }
            input_->injectKeyboard(o.value(QStringLiteral("keycode")).toInt(),
                                   o.value(QStringLiteral("code")).toString(),
                                   o.value(QStringLiteral("down")).toBool(),
                                   o.value(QStringLiteral("ctrl")).toBool(),
                                   o.value(QStringLiteral("alt")).toBool(),
                                   o.value(QStringLiteral("shift")).toBool(),
                                   false,
                                   o.value(QStringLiteral("char")).toBool(),
                                   o.value(QStringLiteral("meta")).toBool());
        } else if (kind == QLatin1String("mods")) {
            input_->updateModifiers(o.value(QStringLiteral("ctrl")).toBool(),
                                    o.value(QStringLiteral("alt")).toBool(),
                                    o.value(QStringLiteral("shift")).toBool());
        }
    }

    void cleanupFiles()
    {
        if (client_) {
            QJsonObject bye;
            bye.insert(QStringLiteral("t"), QStringLiteral("bye"));
            bye.insert(QStringLiteral("reason"), QStringLiteral("agent shutdown"));
            sendJson(bye);
            client_->flush();
            client_->disconnect(this);
        }
        server_.close();
        QLocalServer::removeServer(SessionAgent::socketPath());
        unmapShm();
        if (!shmPath_.isEmpty())
            ::unlink(qPrintable(shmPath_));
    }

    QCoreApplication& app_;
    std::unique_ptr<WaylandCapturer> capturer_;
    std::unique_ptr<InputManager> input_;

    QLocalServer server_;
    QLocalSocket* client_ = nullptr;
    QByteArray readBuf_;

    QTimer frameTimer_;
    QTimer watchTimer_;

    int uid_ = 0;
    int pid_ = 0;
    int width_ = 0;
    int height_ = 0;

    QString shmPath_;
    int shmFd_ = -1;
    uchar* shmBase_ = nullptr;
    int slotBytes_ = 0;
    bool pending_[SessionAgent::kSlots] = {false, false};
    quint64 seq_ = 0;

    QImage lastImage_;
    bool sentOnce_ = false;
    bool unicodeWarned_ = false;
};

} // namespace

int SessionAgentRun::run(int argc, char* argv[])
{
    QCoreApplication app(argc, argv);

    if (!ensureWaylandEnv()) {
        qInfo() << "SessionAgent: no Wayland session detected; nothing to do (X11 stays "
                   "on the existing capture path)";
        return 0;
    }
    ensureSessionBusEnv();
    setupPortalTokenEnv();

    qInfo() << "SessionAgent: starting in user session, WAYLAND_DISPLAY="
            << qgetenv("WAYLAND_DISPLAY");

    ::signal(SIGTERM, onQuitSignal);
    ::signal(SIGINT, onQuitSignal);

    std::unique_ptr<WaylandCapturer> capturer(new WaylandCapturer());
    if (!capturer->initialize()) {
        qWarning() << "SessionAgent: Wayland capture unavailable "
                      "(portal authorization cancelled or no ScreenCast backend); "
                      "root service will keep using its local fallback";
        return 0;
    }

    const int w = capturer->width();
    const int h = capturer->height();
    if (w <= 0 || h <= 0) {
        qWarning() << "SessionAgent: capture negotiated no geometry; exiting";
        return 0;
    }

    auto input = std::unique_ptr<InputManager>(new InputManager());
    input->setForceUinput(true);
    input->setScreenSize(w, h);

    if (::access("/dev/uinput", W_OK) != 0) {
        qWarning() << "SessionAgent: /dev/uinput is not writable by" << userName()
                   << "- input will fall back to root-side injection.";
        qWarning() << "SessionAgent: to enable user-session uinput run:"
                   << QStringLiteral("sudo usermod -aG input ") + userName()
                   << "(then log out and back in)";
    }
    input->prewarmUinput();
    if (!input->isUinputReady()) {
        qWarning() << "SessionAgent: uinput devices unavailable ("
                   << "/dev/uinput permission or group membership); "
                   << "reporting input=none so the root service injects input itself";
    }

    qputenv("QTRD_WAYLAND_WIDTH", QByteArray::number(w));
    qputenv("QTRD_WAYLAND_HEIGHT", QByteArray::number(h));

    AgentHost host(app, std::move(capturer), std::move(input), w, h);
    if (!host.start())
        return 0;

    qInfo() << "SessionAgent: capture geometry" << w << "x" << h;
    const int rc = app.exec();
    qInfo() << "SessionAgent: exiting (rc=" << rc << ")";
    return rc;
}

#else // !HAVE_PIPEWIRE

int SessionAgentRun::run(int argc, char* argv[])
{
    Q_UNUSED(argc)
    Q_UNUSED(argv)
    qInfo() << "SessionAgent: built without PipeWire support; exiting "
               "(root service keeps using local capture)";
    return 0;
}

#endif // HAVE_PIPEWIRE
