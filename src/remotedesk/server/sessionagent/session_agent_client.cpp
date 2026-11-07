// server/sessionagent/session_agent_client.cpp
#include "session_agent_client.h"
#include "session_agent_protocol.h"

#include <QDateTime>
#include <QDebug>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QLocalSocket>
#include <QMutexLocker>
#include <QTimer>

#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

namespace {

// 从 /run/systemd/seats/seat0 读出当前 ACTIVE 会话，再取其 UID。
// 不依赖 logind 的 D-Bus：服务进程可能先于 system bus 可用而启动。
int activeSeatUid()
{
    QFile seat(QStringLiteral("/run/systemd/seats/seat0"));
    if (!seat.open(QIODevice::ReadOnly))
        return -1;
    QString sid;
    for (const QByteArray& line : seat.readAll().split('\n')) {
        if (line.startsWith("ACTIVE="))
            sid = QString::fromUtf8(line.mid(7)).trimmed();
    }
    seat.close();
    if (sid.isEmpty())
        return -1;

    QFile sess(QStringLiteral("/run/systemd/sessions/") + sid);
    if (!sess.open(QIODevice::ReadOnly))
        return -1;
    int uid = -1;
    for (const QByteArray& line : sess.readAll().split('\n')) {
        if (line.startsWith("UID="))
            uid = line.mid(4).trimmed().toInt();
    }
    return uid;
}

bool isNumericDirName(const QString& name)
{
    if (name.isEmpty())
        return false;
    for (const QChar c : name) {
        if (!c.isDigit())
            return false;
    }
    return true;
}

// 返回可用的 socket 路径；uidOut 为路径所属 uid（-1 表示未知，例如环境变量指定）。
QString discoverSocket(int* uidOut)
{
    *uidOut = -1;

    const QByteArray envSock = qgetenv("QTRD_AGENT_SOCKET");
    if (!envSock.isEmpty())
        return QString::fromUtf8(envSock);

    const int seatUid = activeSeatUid();
    if (seatUid > 0) {
        const QString p = QStringLiteral("/run/user/%1/remotedesk/session-agent.sock")
                              .arg(seatUid);
        if (QFileInfo::exists(p)) {
            *uidOut = seatUid;
            return p;
        }
    }

    // 兜底：扫描 /run/user/*。多用户机器上可能有多个代理，取第一个可用的；
    // hello 里的 uid 必须与目录 uid 一致（防止误连到别人的会话）。
    const QDir runUser(QStringLiteral("/run/user"));
    for (const QString& name : runUser.entryList(QDir::Dirs | QDir::NoDotAndDotDot)) {
        if (!isNumericDirName(name))
            continue;
        const QString p = runUser.filePath(name) + QStringLiteral("/remotedesk/session-agent.sock");
        if (QFileInfo::exists(p)) {
            *uidOut = name.toInt();
            return p;
        }
    }
    return QString();
}

} // namespace

SessionAgentClient::SessionAgentClient() = default;

SessionAgentClient& SessionAgentClient::shared()
{
    static SessionAgentClient* instance = new SessionAgentClient();
    return *instance;
}

void SessionAgentClient::start()
{
    if (started_)
        return;
    if (qgetenv("QTRD_DISABLE_SESSION_AGENT") == QByteArrayLiteral("1")) {
        qInfo() << "SessionAgent: disabled by QTRD_DISABLE_SESSION_AGENT=1";
        return;
    }
    started_ = true;
    tickTimer_ = new QTimer(this);
    tickTimer_->setInterval(1000);
    connect(tickTimer_, &QTimer::timeout, this, &SessionAgentClient::tick);
    tickTimer_->start();
    qInfo() << "SessionAgent: discovery enabled";
}

bool SessionAgentClient::takeFrame(QImage& out, bool* updated)
{
    QMutexLocker lk(&frameMutex_);
    if (latestFrame_.isNull())
        return false;
    out = latestFrame_;
    if (updated)
        *updated = (latestSeq_ != deliveredSeq_);
    deliveredSeq_ = latestSeq_;
    return true;
}

void SessionAgentClient::requestFrame()
{
    if (!socket_ || !handshaken_)
        return;
    QJsonObject o;
    o.insert(QStringLiteral("t"), QStringLiteral("need_frame"));
    sendJson(o);
}

void SessionAgentClient::setPaused(bool paused)
{
    if (!socket_ || !handshaken_)
        return;
    QJsonObject o;
    o.insert(QStringLiteral("t"), QStringLiteral("pause"));
    o.insert(QStringLiteral("on"), paused);
    sendJson(o);
}

bool SessionAgentClient::injectMouseMove(int x, int y)
{
    if (!inputActive())
        return false;
    QJsonObject o;
    o.insert(QStringLiteral("t"), QStringLiteral("input"));
    o.insert(QStringLiteral("k"), QStringLiteral("move"));
    o.insert(QStringLiteral("x"), x);
    o.insert(QStringLiteral("y"), y);
    sendJson(o);
    return true;
}

bool SessionAgentClient::injectMouseButton(int x, int y, int button, bool isDown)
{
    if (!inputActive())
        return false;
    QJsonObject o;
    o.insert(QStringLiteral("t"), QStringLiteral("input"));
    o.insert(QStringLiteral("k"), QStringLiteral("button"));
    o.insert(QStringLiteral("x"), x);
    o.insert(QStringLiteral("y"), y);
    o.insert(QStringLiteral("b"), button);
    o.insert(QStringLiteral("down"), isDown);
    sendJson(o);
    return true;
}

bool SessionAgentClient::injectWheel(int delta)
{
    if (!inputActive())
        return false;
    QJsonObject o;
    o.insert(QStringLiteral("t"), QStringLiteral("input"));
    o.insert(QStringLiteral("k"), QStringLiteral("wheel"));
    o.insert(QStringLiteral("d"), delta);
    sendJson(o);
    return true;
}

bool SessionAgentClient::injectKeyboard(int keycode, const QString& code, bool isDown,
                                        bool ctrl, bool alt, bool shift,
                                        bool isChar, bool meta)
{
    if (!inputActive())
        return false;
    QJsonObject o;
    o.insert(QStringLiteral("t"), QStringLiteral("input"));
    o.insert(QStringLiteral("k"), QStringLiteral("key"));
    o.insert(QStringLiteral("keycode"), keycode);
    o.insert(QStringLiteral("code"), code);
    o.insert(QStringLiteral("down"), isDown);
    o.insert(QStringLiteral("ctrl"), ctrl);
    o.insert(QStringLiteral("alt"), alt);
    o.insert(QStringLiteral("shift"), shift);
    o.insert(QStringLiteral("char"), isChar);
    o.insert(QStringLiteral("meta"), meta);
    sendJson(o);
    return true;
}

bool SessionAgentClient::updateModifiers(bool ctrl, bool alt, bool shift)
{
    if (!inputActive())
        return false;
    QJsonObject o;
    o.insert(QStringLiteral("t"), QStringLiteral("input"));
    o.insert(QStringLiteral("k"), QStringLiteral("mods"));
    o.insert(QStringLiteral("ctrl"), ctrl);
    o.insert(QStringLiteral("alt"), alt);
    o.insert(QStringLiteral("shift"), shift);
    sendJson(o);
    return true;
}

void SessionAgentClient::connectToAgent()
{
    lastAttemptMs_ = QDateTime::currentMSecsSinceEpoch();

    int uid = -1;
    const QString path = discoverSocket(&uid);
    if (path.isEmpty()) {
        if (!discoveryLogged_) {
            qInfo() << "SessionAgent: no user session agent socket yet, will keep probing";
            discoveryLogged_ = true;
        }
        return;
    }
    discoveryLogged_ = false;
    expectedUid_ = uid;

    if (socket_)
        resetLink(QStringLiteral("reconnect"));

    socket_ = new QLocalSocket(this);
    connect(socket_, &QLocalSocket::connected, this, &SessionAgentClient::onConnected);
    connect(socket_, &QLocalSocket::readyRead, this, &SessionAgentClient::onReadyRead);
    connect(socket_, &QLocalSocket::disconnected, this, &SessionAgentClient::onDisconnected);
#if QT_VERSION >= QT_VERSION_CHECK(5, 15, 0)
    connect(socket_, &QLocalSocket::errorOccurred, this,
            [this](QLocalSocket::LocalSocketError) { onDisconnected(); });
#else
    connect(socket_, QOverload<QLocalSocket::LocalSocketError>::of(&QLocalSocket::error),
            this, [this](QLocalSocket::LocalSocketError) { onDisconnected(); });
#endif

    handshaken_ = false;
    connectDeadlineMs_ = lastAttemptMs_ + 300;   // connect 300ms
    qInfo() << "SessionAgent: connecting to" << path
            << "expectedUid=" << expectedUid_;
    socket_->connectToServer(path, QIODevice::ReadWrite);
}

void SessionAgentClient::onConnected()
{
    if (!socket_)
        return;
    lastMsgMs_ = QDateTime::currentMSecsSinceEpoch();
    connectDeadlineMs_ = lastMsgMs_ + 1000;   // hello 1s
}

void SessionAgentClient::onDisconnected()
{
    if (!socket_)
        return;
    resetLink(QStringLiteral("agent disconnected"));
}

void SessionAgentClient::tick()
{
    if (stopRequested_)
        return;
    if (qgetenv("QTRD_DISABLE_SESSION_AGENT") == QByteArrayLiteral("1"))
        return;

    const qint64 now = QDateTime::currentMSecsSinceEpoch();

    if (!socket_) {
        // 每 3s 尝试一次；失败不刷屏。
        if (now - lastAttemptMs_ >= 3000)
            connectToAgent();
        return;
    }

    const bool connected = socket_->state() == QLocalSocket::ConnectedState;
    if (!connected) {
        if (now >= connectDeadlineMs_) {
            resetLink(QStringLiteral("connect timeout"));
            lastAttemptMs_ = now;   // 别在同一秒里反复重试
        }
        return;
    }

    if (!handshaken_) {
        if (now >= connectDeadlineMs_) {
            resetLink(QStringLiteral("hello timeout"));
            lastAttemptMs_ = now;
        }
        return;
    }

    // 心跳：每 2s 发 ping；超过 6s 没有任何消息判定代理已死。
    if (now - lastMsgMs_ > 6000) {
        resetLink(QStringLiteral("heartbeat timeout"));
        lastAttemptMs_ = now;
        return;
    }
    if (now - lastPingMs_ >= 2000) {
        lastPingMs_ = now;
        QJsonObject o;
        o.insert(QStringLiteral("t"), QStringLiteral("ping"));
        sendJson(o);
    }
}

void SessionAgentClient::onReadyRead()
{
    // 缓存本地指针：handleMessage() 里的 bye/协议错误会走 resetLink()，
    // 把 socket_ 置空并安排在事件循环后释放。若继续解引用成员 socket_，
    // 代理退出（SIGTERM 发 bye）的同一秒就会在循环条件上 SEGV。
    QLocalSocket* sock = socket_;
    if (!sock)
        return;

    // QLocalSocket 的 canReadLine 基于内部缓冲区，按行消费。
    while (socket_ == sock && sock->canReadLine()) {
        const QByteArray line = sock->readLine().trimmed();
        if (line.isEmpty())
            continue;
        lastMsgMs_ = QDateTime::currentMSecsSinceEpoch();
        QJsonParseError err{};
        const QJsonDocument doc = QJsonDocument::fromJson(line, &err);
        if (err.error != QJsonParseError::NoError || !doc.isObject()) {
            qWarning() << "SessionAgent: bad control message:" << line.left(200);
            continue;
        }
        handleMessage(doc.object());
    }
}

void SessionAgentClient::handleMessage(const QJsonObject& obj)
{
    const QString type = obj.value(QStringLiteral("t")).toString();

    if (type == QLatin1String("hello")) {
        const int ver = obj.value(QStringLiteral("ver")).toInt();
        agentUid_ = obj.value(QStringLiteral("uid")).toInt(-1);
        agentPid_ = obj.value(QStringLiteral("pid")).toInt();
        const QString capture = obj.value(QStringLiteral("capture")).toString();
        inputAvailable_ = obj.value(QStringLiteral("input")).toString() == QLatin1String("uinput");
        width_ = obj.value(QStringLiteral("width")).toInt(0);
        height_ = obj.value(QStringLiteral("height")).toInt(0);

        if (ver != SessionAgent::kProtocolVersion) {
            resetLink(QStringLiteral("protocol version mismatch"));
            return;
        }
        if (expectedUid_ > 0 && agentUid_ != expectedUid_) {
            qWarning() << "SessionAgent: refusing agent uid" << agentUid_
                       << "(socket owner expected" << expectedUid_ << ")";
            resetLink(QStringLiteral("uid mismatch"));
            return;
        }

        handshaken_ = true;
        connectDeadlineMs_ = 0;
        lastPingMs_ = 0;

        if (capture == QLatin1String("none")) {
            const QString note = obj.value(QStringLiteral("note")).toString();
            qWarning() << "SessionAgent: portal authorization cancelled or no capture backend;"
                       << "falling back to local capture. note:" << note;
            return;
        }

        qInfo() << "SessionAgent: hello from uid" << agentUid_ << "pid" << agentPid_
                << "capture=" << capture << "geometry=" << width_ << "x" << height_
                << "input=" << (inputAvailable_ ? "uinput" : "root-fallback");
        return;
    }

    if (!handshaken_)
        return;

    if (type == QLatin1String("shm")) {
        reloadShm(obj.value(QStringLiteral("path")).toString(),
                  obj.value(QStringLiteral("slotBytes")).toInt(0));
        return;
    }

    if (type == QLatin1String("frame")) {
        const quint64 seq = static_cast<quint64>(obj.value(QStringLiteral("seq")).toDouble());
        const int slot = obj.value(QStringLiteral("slot")).toInt(-1);
        const int w = obj.value(QStringLiteral("w")).toInt(0);
        const int h = obj.value(QStringLiteral("h")).toInt(0);
        const int stride = obj.value(QStringLiteral("stride")).toInt(0);

        if (!shmBase_ || slot < 0 || slot >= SessionAgent::kSlots
            || w <= 0 || h <= 0 || stride <= 0
            || stride * h > shmSlotBytes_) {
            qWarning() << "SessionAgent: drop invalid frame seq=" << seq
                       << "slot=" << slot << "geom=" << w << "x" << h
                       << "stride=" << stride << "slotBytes=" << shmSlotBytes_;
            return;
        }

        uchar* base = shmBase_ + static_cast<size_t>(slot) * shmSlotBytes_;
        const QImage view(base, w, h, stride, QImage::Format_RGB32);
        if (view.isNull()) {
            qWarning() << "SessionAgent: null frame view";
            return;
        }
        {
            QMutexLocker lk(&frameMutex_);
            // 必须 copy：槽会被代理在收到 ack 后立即复用。
            latestFrame_ = view.copy();
            latestSeq_ = seq;
        }
        width_ = w;
        height_ = h;

        QJsonObject ack;
        ack.insert(QStringLiteral("t"), QStringLiteral("ack"));
        ack.insert(QStringLiteral("seq"), static_cast<double>(seq));
        ack.insert(QStringLiteral("slot"), slot);
        sendJson(ack);

        if (!streaming_)
            markStreaming(true);
        return;
    }

    if (type == QLatin1String("bye")) {
        resetLink(QStringLiteral("agent said bye: ")
                      + obj.value(QStringLiteral("reason")).toString());
        return;
    }

    if (type == QLatin1String("error")) {
        qWarning() << "SessionAgent: agent error:"
                   << obj.value(QStringLiteral("msg")).toString();
        return;
    }
    // ping/pong 只更新时间戳，无需额外处理。
}

void SessionAgentClient::reloadShm(const QString& path, int slotBytes)
{
    unmapShm();
    if (path.isEmpty() || slotBytes <= 0 || slotBytes > SessionAgent::kMaxFrameBytes) {
        qWarning() << "SessionAgent: invalid shm description path=" << path
                   << "slotBytes=" << slotBytes;
        return;
    }

    const int fd = ::open(qPrintable(path), O_RDONLY);
    if (fd < 0) {
        qWarning() << "SessionAgent: cannot open shm" << path;
        return;
    }
    struct stat st {};
    if (::fstat(fd, &st) != 0
        || st.st_size < static_cast<off_t>(slotBytes) * SessionAgent::kSlots) {
        qWarning() << "SessionAgent: shm too small" << path << "size=" << st.st_size
                   << "need=" << (slotBytes * SessionAgent::kSlots);
        ::close(fd);
        return;
    }
    void* p = ::mmap(nullptr, static_cast<size_t>(slotBytes) * SessionAgent::kSlots,
                     PROT_READ, MAP_SHARED, fd, 0);
    if (p == MAP_FAILED) {
        qWarning() << "SessionAgent: mmap failed for" << path;
        ::close(fd);
        return;
    }

    shmFd_ = fd;
    shmBase_ = static_cast<uchar*>(p);
    shmSlotBytes_ = slotBytes;
    shmPath_ = path;
    qInfo() << "SessionAgent: mapped frame ring" << path
            << "slotBytes=" << slotBytes;
}

void SessionAgentClient::unmapShm()
{
    if (shmBase_) {
        ::munmap(shmBase_, static_cast<size_t>(shmSlotBytes_) * SessionAgent::kSlots);
        shmBase_ = nullptr;
    }
    if (shmFd_ >= 0) {
        ::close(shmFd_);
        shmFd_ = -1;
    }
    shmSlotBytes_ = 0;
    shmPath_.clear();
}

void SessionAgentClient::sendJson(const QJsonObject& obj)
{
    if (!socket_ || socket_->state() != QLocalSocket::ConnectedState)
        return;
    QByteArray line = QJsonDocument(obj).toJson(QJsonDocument::Compact);
    line.append('\n');
    socket_->write(line);
}

void SessionAgentClient::markStreaming(bool on)
{
    if (streaming_ == on)
        return;
    streaming_ = on;
    setCaptureChangePending();
    if (on) {
        qInfo() << "SessionAgent: streaming real user desktop ("
                << width_ << "x" << height_ << ", agent pid" << agentPid_ << ")";
    } else {
        qWarning() << "SessionAgent: agent capture lost, service will fall back to local capture";
    }
}

void SessionAgentClient::setCaptureChangePending()
{
    captureChangePending_ = true;
}

void SessionAgentClient::resetLink(const QString& reason)
{
    const bool wasActive = streaming_;
    if (socket_) {
        socket_->disconnect(this);
        if (socket_->state() != QLocalSocket::UnconnectedState)
            socket_->abort();
        socket_->deleteLater();
        socket_ = nullptr;
    }
    handshaken_ = false;
    inputAvailable_ = false;
    connectDeadlineMs_ = 0;
    lastPingMs_ = 0;
    unmapShm();
    {
        QMutexLocker lk(&frameMutex_);
        latestFrame_ = QImage();
        latestSeq_ = 0;
        deliveredSeq_ = 0;
    }
    if (wasActive) {
        markStreaming(false);
    } else if (!reason.isEmpty()) {
        // 代理未运行时每 3s 都会走一次失败连接，日志按 30s 节流。
        const qint64 now = QDateTime::currentMSecsSinceEpoch();
        if (now - lastResetLogMs_ >= 30000) {
            lastResetLogMs_ = now;
            qInfo() << "SessionAgent: link reset (" << reason << ")";
        }
    }
}
