#include "websocketserver.h"
#include <QDataStream>
#include <QDateTime>
#include <QDebug>
#include <QJsonDocument>
#include <QJsonObject>
#include <QUrl>
#include <QUrlQuery>

// 单客户端待发缓冲上限：慢客户端（网络拥塞、标签页被挂起/切到后台）会导致
// sendBinaryMessage 的发送队列无限增长，最终吃光服务端内存。超过该阈值时主动
// 丢弃该客户端的媒体帧（图像/视频），待其把积压发完再自然恢复。
// 只作用于媒体广播，不碰控制类 JSON（体量小且不能丢）。
// 阈值同时就是**延迟上限**：稳态积压 ≈ 阈值，延迟 = 阈值 ÷ 链路吞吐。
// 8MB 在 2~4MB/s 的 WiFi/百兆链路上意味着 2~4 秒常驻延迟（用户实测"连上
// 流畅、5 分钟后 2~3 秒"即此因）。512KB 对应亚秒级延迟；视频模式触顶丢
// P 帧后最多等 0.5s（GOP=fps/2）即有新 IDR 重新同步。
static const qint64 kMaxSocketBacklogBytes = 512 * 1024;

// 普通远程客户端连接数上限。每条连接都会在 clients_ / socketToId_ / pendingBytes_
// 里留一份状态，且会被纳入视频广播列表；无上限时局域网内一堆（或失控脚本反复）
// 连接会线性抬高每帧广播开销并吃掉内存。超出直接拒绝，不排队。
static const int kMaxClients = 32;

// 单条入站消息大小上限。上传/输入都是小块；文本通道除普通键鼠事件外还承载
// 剪贴板（截图 PNG → base64 常见 1.5~4MB），因此文本上限必须显著高于 1MB，
// 否则"复制截图 → 远端粘贴"会被静默丢弃。协议层上限按文本上限的 2 倍留出
// JSON 包装余量。
static const qint64 kMaxInboundTextBytes = 32 * 1024 * 1024;
static const qint64 kMaxInboundMessageBytes = 64 * 1024 * 1024;

// 半开连接清理阈值：前端每 15s 发一次 ping，正常连接不可能 180s 无任何上行消息。
static const qint64 kClientIdleTimeoutMs = 180000;
static const int kIdleSweepIntervalMs = 30000;

WebSocketServer::WebSocketServer(QWebSocketServer::SslMode mode, QObject* parent)
    : QObject(parent)
    , server_(new QWebSocketServer(QStringLiteral("RemoteDesktopServer"),
          mode,
          this))
{
    connect(server_, &QWebSocketServer::newConnection,
        this, &WebSocketServer::onNewConnection);

    idleTimer_.setInterval(kIdleSweepIntervalMs);
    connect(&idleTimer_, &QTimer::timeout, this, [this]() {
        const qint64 now = QDateTime::currentMSecsSinceEpoch();
        const QStringList ids = clients_.keys();
        for (const QString& id : ids) {
            QWebSocket* s = clients_.value(id);
            if (!s || s->state() != QAbstractSocket::ConnectedState)
                continue;
            const qint64 last = lastActivityMs_.value(id, now);
            if (now - last > kClientIdleTimeoutMs) {
                qWarning() << "WS client idle timeout, dropping:" << id;
                // 复用 dropClient 的清理路径（断开信号连接 + 从各表摘除 + emit）
                dropClient(id);
            }
        }
        // /capture 半开清理：helper 正常时每帧都有上行数据，180s 静默 = 连接已死。
        // 不清理的话 captureSource_ 一直指向僵尸 socket，服务端误以为有采集源在线。
        if (captureSource_ && captureLastDataMs_ > 0 && now - captureLastDataMs_ > kClientIdleTimeoutMs) {
            qWarning() << "Capture source idle timeout, dropping";
            QWebSocket* dead = captureSource_;
            captureSource_ = nullptr;
            captureLastDataMs_ = 0;
            emit captureSourceDisconnected();
            disconnect(dead, nullptr, this, nullptr);
            dead->close();
            dead->deleteLater();
        }
        // /secure-input 单向通道：靠 ping/pong 判活（见 secureInputLastPongMs_ 注释）。
        if (secureInputSource_) {
            QWebSocket* s = secureInputSource_;
            if (s->state() != QAbstractSocket::ConnectedState) {
                qWarning() << "Secure input helper socket not connected, dropping";
                secureInputSource_ = nullptr;
                secureInputLastPongMs_ = 0;
                disconnect(s, nullptr, this, nullptr);
                s->deleteLater();
            } else if (secureInputLastPongMs_ > 0
                       && now - secureInputLastPongMs_ > 3 * kIdleSweepIntervalMs) {
                // 三个扫描周期（90s）没有 pong = 对端已失联（无 FIN 的半开连接）
                qWarning() << "Secure input helper ping timeout, dropping";
                secureInputSource_ = nullptr;
                secureInputLastPongMs_ = 0;
                disconnect(s, nullptr, this, nullptr);
                s->close();
                s->deleteLater();
            } else {
                s->ping();
            }
        }
        // /api/shell/ws 半开清理：浏览器会自动回 pong。三个扫描周期（90s）
        // 无 pong 说明对端已失联，主动关闭让 RDPServer 走 disconnected
        // 清理（kill bash / 释放 PTY / 归还并发配额）。
        const QList<QWebSocket*> shells = shellSockets_.values();
        for (QWebSocket* s : shells) {
            if (!s)
                continue;
            if (s->state() != QAbstractSocket::ConnectedState) {
                qWarning() << "Shell WS not connected, dropping";
                s->close();
                s->deleteLater();
                continue;
            }
            const qint64 lastPong = shellLastPongMs_.value(s, now);
            if (now - lastPong > 3 * kIdleSweepIntervalMs) {
                qWarning() << "Shell WS ping timeout, dropping";
                s->close();
                s->deleteLater();
            } else {
                s->ping();
            }
        }
    });
    idleTimer_.start();
}

WebSocketServer::~WebSocketServer()
{
    idleTimer_.stop();
    // 先断开信号，防止删除时触发 onSocketDisconnected
    for (QWebSocket* socket : clients_.values()) {
        disconnect(socket, nullptr, this, nullptr);
    }
    if (captureSource_) {
        disconnect(captureSource_, nullptr, this, nullptr);
        captureSource_->deleteLater();
        captureSource_ = nullptr;
    }
    // /secure-input 的 helper 同样是本类创建/持有的 socket；漏掉会随服务退出
    // 留下一个未关闭的连接（对端 helper 也会一直重连等待）。
    if (secureInputSource_) {
        disconnect(secureInputSource_, nullptr, this, nullptr);
        secureInputSource_->close();
        secureInputSource_->deleteLater();
        secureInputSource_ = nullptr;
    }

    server_->close();
    qDeleteAll(clients_);
    clients_.clear();
    {
        QMutexLocker lk(&aliveMutex_);
        aliveClients_.clear();
    }
    socketToId_.clear();
}

bool WebSocketServer::listen(const QHostAddress& address, quint16 port)
{
    if (!server_->listen(address, port)) {
        qCritical() << "WebSocket server failed to listen:" << server_->errorString();
        return false;
    }

    // 如果是 SecureMode，输出 WSS 字样
    if (server_->secureMode() == QWebSocketServer::SecureMode) {
        qInfo() << "WebSocket server (WSS) listening on" << address.toString() << ":" << port;
    } else {
        qInfo() << "WebSocket server (WS) listening on" << address.toString() << ":" << port;
    }

    return true;
}

void WebSocketServer::broadcastCodecConfig(const QByteArray& extra)
{
    QJsonObject obj;
    obj["type"] = "codec_config";
    obj["extradata"] = QString::fromLatin1(extra.toBase64());
    broadcastJson(obj);
}

void WebSocketServer::onNewConnection()
{
    // While 循环取完全部 pending 连接；对端完成握手后立即断开的竞态下
    // nextPendingConnection() 可能返回 nullptr，必须判空后才能解引用。
    while (QWebSocket* socket = server_->nextPendingConnection())
        handleNewSocket(socket);
}

void WebSocketServer::handleNewSocket(QWebSocket* socket)
{
    QUrl url = socket->requestUrl();
    // 只记 path，不打完整 URL：URL 的 ?token= 携带会话令牌，写入日志文件会泄漏；
    // 连接属低频事件，降为 qDebug 避免默认日志噪音
    qDebug() << "WS new connection, path:" << url.path();

    // 入站消息大小上限必须在任何 early-return 之前设置：/capture、/secure-input、
    // /api/shell/ws 都是先 return 的特权通道，原实现把这一步放在后面，导致它们
    // 完全不设协议层上限（分片重组可先吃到 16MB+ 才被上层丢弃）。
#if QT_VERSION >= QT_VERSION_CHECK(5, 15, 0)
    socket->setMaxAllowedIncomingMessageSize(kMaxInboundMessageBytes);
#endif

    // 内部通道（helper / secure-input）必须来自本机回环地址：
    // 服务端监听 0.0.0.0，若不加限制，局域网内任意主机可连 /capture 伪造
    // 画面、劫持捕获控制，或连 /secure-input 混入锁屏输入。
    auto rejectIfRemote = [](QWebSocket* s, const char* what) {
        qWarning() << "WS rejected (non-loopback peer):" << what
                   << "peer=" << s->peerAddress().toString();
        s->close(QWebSocketProtocol::CloseCodePolicyViolated, QStringLiteral("loopback required"));
        s->deleteLater();
    };

    if (url.path() == "/capture") {
        if (!socket->peerAddress().isLoopback()) {
            rejectIfRemote(socket, "/capture");
            return;
        }
        qInfo() << "Helper connected to service WS via /capture";
        if (captureSource_) {
            qInfo() << "Replacing existing capture source";
            // 先断开旧源的所有信号：仅 deleteLater 的话，旧 socket 在本轮事件循环
            // 结束前仍可能投递已排队的帧/消息，与新 helper 的帧混流导致画面抖动。
            QWebSocket* old = captureSource_;
            disconnect(old, nullptr, this, nullptr);
            old->close();
            old->deleteLater();
        }
        captureSource_ = socket;
        captureLastDataMs_ = QDateTime::currentMSecsSinceEpoch();
        emit captureSourceConnected();
        connect(socket, &QWebSocket::disconnected, this, [this, socket]() {
            qWarning() << "Helper disconnected from /capture";
            if (captureSource_ == socket) {
                captureSource_ = nullptr;
                emit captureSourceDisconnected();
            }
            socket->deleteLater();
        });
        connect(socket, &QWebSocket::binaryMessageReceived, this, [this, socket](const QByteArray& msg) {
            // 已被新 helper 替换的旧连接不再产生帧（信号可能断线前已入队）
            if (captureSource_ != socket)
                return;
            captureLastDataMs_ = QDateTime::currentMSecsSinceEpoch();
            if (msg.size() < 5) return;
            QDataStream stream(msg);
            stream.setByteOrder(QDataStream::BigEndian);
            quint8 frameType;
            quint32 dataLen;
            stream >> frameType >> dataLen;
            if (frameType == 0x03) {
                // JPEG：[0x03][u32 len][jpeg]
                // 校验声明长度：旧实现完全忽略 dataLen 直接 mid(5)，畸形/错误
                // 长度会把后续内容一起当 JPEG 广播。
                if (dataLen == 0 || quint32(msg.size() - 5) < dataLen)
                    return;
                // fromRawData 零拷贝视图（msg 在本同步 emit 期间有效）：
                // mid() 每帧都整帧深拷贝，1080p 截图数百 KB~数 MB、30fps，
                // 全发生在主线程。
                emit captureFrameReceived(
                    QByteArray::fromRawData(msg.constData() + 5, int(dataLen)));
            } else if (frameType == 0x01 || frameType == 0x02) {
                // H.264：[0x01|0x02][u32 len][i64 ts][data]（13 字节头，与服务端→浏览器一致）
                if (msg.size() < 13) return;
                qint64 ts = 0;
                stream >> ts;
                if (dataLen == 0 || quint32(msg.size() - 13) < dataLen)
                    return;
                // 这里必须复制（mid）而不能用 fromRawData 视图：onEncodedFrame 在
                // 关键帧时会 `lastKeyframeData_ = data` 长期缓存（新客户端接入时
                // 回放），裸视图在 msg 析构后会悬空 → 发送时读已释放内存。
                emit captureVideoFrameReceived(msg.mid(13, int(dataLen)),
                    frameType == 0x01, ts);
            }
        });
        connect(socket, &QWebSocket::textMessageReceived, this, [this, socket](const QString& text) {
            if (captureSource_ != socket)
                return;
            captureLastDataMs_ = QDateTime::currentMSecsSinceEpoch();
            QJsonDocument doc = QJsonDocument::fromJson(text.toUtf8());
            if (doc.isObject())
                emit captureMessageReceived(doc.object());
        });
        return;
    }

    if (url.path() == "/api/shell/ws") {
        // shell 通道不进入 clients_（不参与媒体广播/背压），但每条连接都会在
        // RDPServer 侧创建 PTY + bash 子进程。必须单独限流，否则一个已登录
        // 用户反复开连接即可无限制 spawn 进程、耗尽句柄。
        // 用 destroyed 而非 disconnected 归还配额：认证失败、异常销毁等所有
        // 路径最终都会删除 socket 对象，不会漏计。
        static const int kMaxShellClients = 8;
        if (shellSockets_.size() >= kMaxShellClients) {
            qWarning() << "Shell WS rejected: too many interactive shells (" << shellSockets_.size() << ")";
            socket->close(QWebSocketProtocol::CloseCodePolicyViolated,
                          QStringLiteral("too many interactive shells"));
            socket->deleteLater();
            return;
        }
        shellSockets_.insert(socket);
        shellLastPongMs_[socket] = QDateTime::currentMSecsSinceEpoch();
        connect(socket, &QWebSocket::pong, this, [this, socket](quint64, const QByteArray&) {
            shellLastPongMs_[socket] = QDateTime::currentMSecsSinceEpoch();
        });
        connect(socket, &QObject::destroyed, this, [this, socket]() {
            shellSockets_.remove(socket);
            shellLastPongMs_.remove(socket);
        });
        qInfo() << "Interactive shell connected via WS";
        emit shellConnected(socket);
        return;
    }

    if (url.path() == "/secure-input") {
        if (!socket->peerAddress().isLoopback()) {
            rejectIfRemote(socket, "/secure-input");
            return;
        }
        qInfo() << "Secure input helper connected";
        if (secureInputSource_) {
            // 与 /capture 同理：断开旧 helper 的信号并主动关闭，避免替换窗口内
            // 旧连接的消息继续生效（锁屏输入这类通道尤其不能串号）。
            QWebSocket* old = secureInputSource_;
            disconnect(old, nullptr, this, nullptr);
            old->close();
            old->deleteLater();
        }
        secureInputSource_ = socket;
        secureInputLastPongMs_ = QDateTime::currentMSecsSinceEpoch();
        // 心跳：idleTimer_ 每 30s ping 一次，pong 回来才刷新活跃时间；
        // 单向通道没有上行数据，这是唯一能发现半开连接的办法。
        connect(socket, &QWebSocket::pong, this, [this, socket](quint64, const QByteArray&) {
            if (secureInputSource_ == socket)
                secureInputLastPongMs_ = QDateTime::currentMSecsSinceEpoch();
        });
        connect(socket, &QWebSocket::disconnected, this, [this, socket]() {
            qWarning() << "Secure input helper disconnected";
            if (secureInputSource_ == socket) {
                secureInputSource_ = nullptr;
                secureInputLastPongMs_ = 0;
            }
            socket->deleteLater();
        });
        return;
    }

    // 普通远程客户端
    // 连接数上限：超限直接拒绝，避免无限连接抬高广播开销与内存占用
    if (clients_.size() >= kMaxClients) {
        qWarning() << "WS rejected: too many clients (" << clients_.size() << ")";
        socket->close(QWebSocketProtocol::CloseCodePolicyViolated,
                      QStringLiteral("too many connections"));
        socket->deleteLater();
        return;
    }

    // Qt 5.15+ 可在协议层限制入站消息大小（分片会在接收阶段就被拒绝）；
    // 更早版本没有该 API，靠下面 onTextMessageReceived/onBinaryMessageReceived 的
    // 手动长度检查兜底。（上限已在 handleNewSocket 开头统一设置）

    QString clientId = QUuid::createUuid().toString();

    QString token;
    #if QT_VERSION >= QT_VERSION_CHECK(5, 12, 0)
        token = QUrlQuery(url).queryItemValue("token");
    #else
        QUrlQuery query(url);
        token = query.queryItemValue("token");
    #endif
    if (token.isEmpty()) {
        // 兜底：session cookie 现为 HttpOnly（防 XSS 窃取），前端 JS 读不到，
        // 无法拼 ?token=。浏览器对同源 WS 握手会自动携带 Cookie 头，这里
        // 直接从握手请求里解析 session=，保证 HttpOnly 后登录仍可用。
        const QByteArray cookieHeader = socket->request().rawHeader(QByteArrayLiteral("Cookie"));
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

    clients_[clientId] = socket;
    socketToId_[socket] = clientId;
    {
        QMutexLocker lk(&aliveMutex_);
        aliveClients_.insert(clientId);
    }
    lastActivityMs_[clientId] = QDateTime::currentMSecsSinceEpoch();
    if (!token.isEmpty())
        clientTokens_[clientId] = token;

    connect(socket, &QWebSocket::disconnected, this, &WebSocketServer::onSocketDisconnected);
    connect(socket, &QWebSocket::textMessageReceived, this, &WebSocketServer::onTextMessageReceived);
    connect(socket, &QWebSocket::binaryMessageReceived, this, &WebSocketServer::onBinaryMessageReceived);
    // 背压估算：套接字每写出 n 字节，就从该客户端的待发计数中扣减。
    connect(socket, &QWebSocket::bytesWritten, this, [this, socket](qint64 n) {
        const QString id = socketToId_.value(socket);
        if (id.isEmpty())
            return;
        const qint64 v = pendingBytes_.value(id) - n;
        pendingBytes_[id] = v > 0 ? v : 0;
        // [C5-①] 扣减该客户端自己的下载未写出份额，再据此扣减全局总量
        // （近似：字节总量对账，限流用途足够）。只扣自己的份额，避免
        // 把别人的积压也算进来提前解除限流。
        auto it = downloadBacklogByClient_.find(id);
        if (it != downloadBacklogByClient_.end() && it.value() > 0) {
            const qint64 take = qMin(n, it.value());
            it.value() -= take;
            qint64 cur = downloadBacklog_.load(std::memory_order_relaxed);
            while (cur > 0) {
                const qint64 t = qMin(take, cur);
                if (downloadBacklog_.compare_exchange_weak(cur, cur - t,
                        std::memory_order_relaxed))
                    break;
            }
            if (it.value() <= 0)
                downloadBacklogByClient_.erase(it);
        }
    });

    emit clientConnected(clientId);
}

void WebSocketServer::addDownloadBacklog(const QString& clientId, qint64 delta)
{
    if (delta == 0 || clientId.isEmpty())
        return;
    downloadBacklogByClient_[clientId] += delta;
    downloadBacklog_.fetch_add(delta, std::memory_order_relaxed);
}

void WebSocketServer::releaseDownloadBacklog(const QString& clientId)
{
    auto it = downloadBacklogByClient_.find(clientId);
    if (it == downloadBacklogByClient_.end())
        return;
    const qint64 own = it.value();
    downloadBacklogByClient_.erase(it);
    if (own <= 0)
        return;
    qint64 cur = downloadBacklog_.load(std::memory_order_relaxed);
    while (cur > 0) {
        const qint64 take = qMin(own, cur);
        if (downloadBacklog_.compare_exchange_weak(cur, cur - take,
                std::memory_order_relaxed))
            break;
    }
}

bool WebSocketServer::isClientAlive(const QString& clientId) const
{
    if (clientId.isEmpty())
        return false;
    QMutexLocker lk(&aliveMutex_);
    return aliveClients_.contains(clientId);
}

void WebSocketServer::touchClient(const QString& clientId)
{
    if (!clientId.isEmpty())
        lastActivityMs_[clientId] = QDateTime::currentMSecsSinceEpoch();
}

void WebSocketServer::onSocketDisconnected()
{
    QWebSocket* socket = qobject_cast<QWebSocket*>(sender());
    if (!socket)
        return;

    QString clientId = socketToId_.take(socket);
    clients_.remove(clientId);
    {
        QMutexLocker lk(&aliveMutex_);
        aliveClients_.remove(clientId);
    }
    videoStarted_.remove(clientId);
    pendingBytes_.remove(clientId);
    lastActivityMs_.remove(clientId);
    // [C5-①] 只释放该客户端自己的下载积压份额。旧实现无条件全局清零，
    // 会在多客户端并发下载时把别人的节流一并解除（大目录传输重新撑爆内存）。
    // 若该客户端的传输任务仍在跑，工作线程的查询回调会返回其份额 0，不会死等。
    releaseDownloadBacklog(clientId);
    // clientId 是每条连接新建的 UUID，若不清理 clientTokens_，
    // 每次断线都会残留一条 token→会话映射，长期运行（含频繁重连）单调增长。
    clientTokens_.remove(clientId);
    socket->deleteLater();

    emit clientDisconnected(clientId);
}

void WebSocketServer::onTextMessageReceived(const QString& message)
{
    QWebSocket* socket = qobject_cast<QWebSocket*>(sender());
    if (!socket)
        return;
    // 用 value() 而非 operator[]，避免已移除（dropClient）的 socket 往 QMap 里插入空 key
    QString clientId = socketToId_.value(socket);
    if (clientId.isEmpty())
        return;
    touchClient(clientId);

    // 先用 UTF-8 字节数判上限再解析：QString::size() 是 UTF-16 码元数，
    // 中文/emoji 一个字符占 3~4 字节，按字符判会让实际入站量超上限数倍
    //（32M 字符的 CJK ≈ 96MB UTF-8）。toUtf8() 本来就要为 JSON 解析做一次，
    // 提前到这里只转换一次，不增加拷贝。
    const QByteArray utf8 = message.toUtf8();
    if (utf8.size() > kMaxInboundTextBytes) {
        qWarning() << "Oversized text message from client, dropped:" << utf8.size();
        // 不能静默丢弃：前端剪贴板写入后若无任何回应，会一直显示"已粘贴成功"，
        // 远端剪贴板却停在旧内容（大截图粘贴失败尤其明显且无从排查）。
        QJsonObject err;
        err.insert(QStringLiteral("type"), QStringLiteral("error"));
        err.insert(QStringLiteral("context"), QStringLiteral("inbound_too_large"));
        err.insert(QStringLiteral("size"), utf8.size());
        err.insert(QStringLiteral("limit"), kMaxInboundTextBytes);
        err.insert(QStringLiteral("message"),
                   QStringLiteral("消息过大：%1 字节（上限 %2）")
                       .arg(utf8.size())
                       .arg(kMaxInboundTextBytes));
        sendJson(clientId, err);
        return;
    }

    QJsonParseError error;
    QJsonDocument doc = QJsonDocument::fromJson(utf8, &error);
    if (error.error != QJsonParseError::NoError) {
        qWarning() << "Invalid JSON from client:" << error.errorString();
        return;
    }
    if (!doc.isObject())
        return;

    QJsonObject obj = doc.object();
    QString type = obj["type"].toString();

    if (type == "change_codec") {
        QString codec = obj["codec"].toString();
        emit codecChangeRequested(codec);
    } else if (type == "set_mode") { // 处理模式切换请求
        QString mode = obj["mode"].toString();
        emit modeChangeRequested(mode);
    } else if (type == "ping") {
        // [B12] 心跳应答：前端每 15s 一跳，45s 无任何服务端消息即判半开连接自愈
        sendJson(clientId, QJsonObject{ { "type", "pong" } });
    } else {
        emit inputReceived(clientId, obj);
    }
}

void WebSocketServer::onBinaryMessageReceived(const QByteArray& message)
{
    if (message.size() < 1) return;

    // 入站消息大小上限：上传/输入都是分块小消息，超大帧基本可判异常，丢弃即可
    // （Qt < 5.15 没有协议层限制 API，这里是唯一防线）
    if (message.size() > kMaxInboundMessageBytes) {
        qWarning() << "Oversized binary message from client, dropped:" << message.size();
        return;
    }

    QWebSocket* src = qobject_cast<QWebSocket*>(sender());
    // 上传数据块必须绑定来源客户端：二进制帧本身不带身份，从 socket 反查
    QString clientId;
    if (src)
        clientId = socketToId_.value(src);
    touchClient(clientId);

    quint8 frameType = static_cast<quint8>(message[0]);

    if (frameType == 0x10) {
        // 文件上传数据块: [0x10][4-byte path length][path UTF8][4-byte data length][data]
        if (message.size() < 9) return;
        QDataStream stream(message);
        stream.setByteOrder(QDataStream::BigEndian);
        stream.skipRawData(1); // skip frame type

        quint32 pathLen, dataLen;
        stream >> pathLen >> dataLen;

        // 用 64 位累加避免 pathLen+dataLen 溢出绕过长度检查（恶意消息可越界读）
        quint64 total = 9ULL + static_cast<quint64>(pathLen) + static_cast<quint64>(dataLen);
        if (message.size() < (qint64)total) return;

        QString path = QString::fromUtf8(message.constData() + 9, pathLen);
        QByteArray data(message.constData() + 9 + pathLen, dataLen);

        emit fileChunkReceived(clientId, path, data);
    }
    // Other binary types ignored
}

QString WebSocketServer::clientToken(const QString& clientId) const
{
    return clientTokens_.value(clientId);
}

void WebSocketServer::dropClient(const QString& clientId)
{
    QWebSocket* socket = clients_.take(clientId);
    if (!socket) return;
    {
        QMutexLocker lk(&aliveMutex_);
        aliveClients_.remove(clientId);
    }
    // 断开信号连接，避免 close() 触发的 disconnected 再次走 onSocketDisconnected 造成重复处理
    disconnect(socket, nullptr, this, nullptr);
    socketToId_.remove(socket);
    clientTokens_.remove(clientId);
    videoStarted_.remove(clientId);
    pendingBytes_.remove(clientId);
    lastActivityMs_.remove(clientId);
    releaseDownloadBacklog(clientId);
    socket->close(QWebSocketProtocol::CloseCodeNormal, "Authentication failed");
    socket->deleteLater();
    emit clientDisconnected(clientId);
}

void WebSocketServer::setMediaExcludedClients(const QSet<QString>& clients)
{
    mediaExcludedClients_ = clients;
}

void WebSocketServer::closeSecureInput()
{
    if (secureInputSource_) {
        QWebSocket* s = secureInputSource_;
        secureInputSource_ = nullptr;
        secureInputLastPongMs_ = 0;
        s->close();
        s->deleteLater(); // close() 异步，disconnected lambda 因已置空不会再执行，需显式释放
    }
}

void WebSocketServer::setSslConfiguration(const QSslConfiguration& config)
{
    sslConfig_ = config;
    server_->setSslConfiguration(sslConfig_); // 设置 SSL 配置（仅当 mode 为 SecureMode 时有效）
}

void WebSocketServer::sendToCaptureSource(const QJsonObject& data)
{
    if (captureSource_ && captureSource_->state() == QAbstractSocket::ConnectedState) {
        QJsonDocument doc(data);
        captureSource_->sendTextMessage(QString::fromUtf8(doc.toJson(QJsonDocument::Compact)));
    } else {
        qWarning() << "sendToCaptureSource: no connected helper";
    }
}

void WebSocketServer::sendToSecureInput(const QJsonObject& data)
{
    if (secureInputSource_ && secureInputSource_->state() == QAbstractSocket::ConnectedState) {
        QJsonDocument doc(data);
        secureInputSource_->sendTextMessage(QString::fromUtf8(doc.toJson(QJsonDocument::Compact)));
    }
}

void WebSocketServer::broadcastFrame(const QByteArray& data, bool isKeyframe, qint64 timestamp)
{
    if (clients_.isEmpty())
        return;

    // [V-3] 先统计有资格收帧的客户端数：纯 WebRTC 场景（全部 mediaExcluded）
    // 原实现仍无条件构建整帧 packet（关键帧 2MB × 30fps = 每秒 60MB 白拷贝）。
    bool anyEligible = false;
    for (auto it = clients_.constBegin(); it != clients_.constEnd(); ++it) {
        if (!mediaExcludedClients_.contains(it.key())
                && it.value()->state() == QAbstractSocket::ConnectedState) {
            anyEligible = true;
            break;
        }
    }
    if (!anyEligible)
        return;

    // 构造二进制包： [1字节帧类型] [4字节大端长度] [8字节大端时间戳] [数据]
    QByteArray packet;
    packet.reserve(data.size() + 13);
    QDataStream stream(&packet, QIODevice::WriteOnly);
    stream.setByteOrder(QDataStream::BigEndian);

    stream << quint8(isKeyframe ? 0x01 : 0x02);
    stream << quint32(data.size());
    stream << qint64(timestamp);
    packet.append(data);

    for (auto it = clients_.constBegin(); it != clients_.constEnd(); ++it) {
        if (mediaExcludedClients_.contains(it.key()))
            continue; // 走 WebRTC 的客户端由 RTP 收流，跳过 WS 视频帧
        QWebSocket* socket = it.value();
        if (socket->state() != QAbstractSocket::ConnectedState)
            continue;

        // 慢客户端背压：积压超过上限则丢弃本帧，并清除其“已开始收流”标记，
        // 使其等到下一个关键帧再干净恢复（直接丢 P 帧会造成花屏）。
        if (pendingBytes_.value(it.key()) > kMaxSocketBacklogBytes) {
            videoStarted_.remove(it.key());
            continue;
        }

        // 每客户端从关键帧起点开始收流：
        // 在收到下一个 IDR 之前丢弃 P 帧，避免从 GOP 中间加入导致黑屏/花屏。
        if (!videoStarted_.contains(it.key())) {
            if (!isKeyframe)
                continue; // 还没到关键帧，跳过纯增量帧
            videoStarted_.insert(it.key());
        }
        pendingBytes_[it.key()] += qMax<qint64>(0, socket->sendBinaryMessage(packet));
    }
}

void WebSocketServer::sendJson(const QString& clientId, const QJsonObject& data)
{
    QWebSocket* socket = clients_.value(clientId);
    if (socket && socket->state() == QAbstractSocket::ConnectedState) {
        QJsonDocument doc(data);
        pendingBytes_[clientId] += qMax<qint64>(0,
            socket->sendTextMessage(QString::fromUtf8(doc.toJson(QJsonDocument::Compact))));
    }
}

void WebSocketServer::sendFrameToClient(const QString& clientId, const QByteArray& data,
                                        bool isKeyframe, qint64 timestamp)
{
    QWebSocket* socket = clients_.value(clientId);
    if (!socket || socket->state() != QAbstractSocket::ConnectedState)
        return;
    if (mediaExcludedClients_.contains(clientId))
        return; // 走 WebRTC 的客户端由 RTP 收流，不回放 WS 帧
    // 慢客户端背压：积压过高时跳过（关键帧重放会由后续 pumpKeyframe 再补）
    if (pendingBytes_.value(clientId) > kMaxSocketBacklogBytes)
        return;

    QByteArray packet;
    QDataStream stream(&packet, QIODevice::WriteOnly);
    stream.setByteOrder(QDataStream::BigEndian);
    stream << quint8(isKeyframe ? 0x01 : 0x02);
    stream << quint32(data.size());
    stream << qint64(timestamp);
    packet.append(data);

    if (isKeyframe)
        videoStarted_.insert(clientId); // 关键帧已就位，后续 P 帧不再丢弃
    pendingBytes_[clientId] += qMax<qint64>(0, socket->sendBinaryMessage(packet));
}

void WebSocketServer::broadcastJson(const QJsonObject& data)
{
    if (clients_.isEmpty())
        return;

    QJsonDocument doc(data);
    QByteArray message = doc.toJson(QJsonDocument::Compact);
    // [P7] UTF-8→QString 转换提到循环外：同一条消息发给所有客户端，
    // 旧实现每客户端重复转换一遍（QString 构造含 16 位宽字符分配）。
    const QString text = QString::fromUtf8(message);

    for (auto it = clients_.constBegin(); it != clients_.constEnd(); ++it) {
        QWebSocket* socket = it.value();
        if (socket->state() == QAbstractSocket::ConnectedState) {
            pendingBytes_[it.key()] += qMax<qint64>(0,
                socket->sendTextMessage(text));
        }
    }
}

void WebSocketServer::broadcastBinary(const QByteArray& data)
{
    if (clients_.isEmpty())
        return;

    for (auto it = clients_.constBegin(); it != clients_.constEnd(); ++it) {
        if (mediaExcludedClients_.contains(it.key()))
            continue; // 走 WebRTC 的客户端由 RTP 收流，跳过 WS 图像帧（与视频路径一致）
        QWebSocket* socket = it.value();
        if (socket->state() != QAbstractSocket::ConnectedState)
            continue;
        // 慢客户端背压：图像帧为帧内编码，丢一帧不影响后续解码，直接跳过即可。
        if (pendingBytes_.value(it.key()) > kMaxSocketBacklogBytes)
            continue;
        pendingBytes_[it.key()] += qMax<qint64>(0, socket->sendBinaryMessage(data));
    }
}

void WebSocketServer::sendBinaryToClient(const QString& clientId, const QByteArray& data)
{
    // [C5-①] 下载分块通道：唯一调用方是文件下载。绝不能因积压超限丢弃——
    // tar/分块流丢一块就是流损坏。丢弃改为上游节流（工作线程轮询
    // downloadBacklog() 暂停产出），这里无条件排队并累加未写出计数。
    QWebSocket* socket = clients_.value(clientId);
    if (socket && socket->state() == QAbstractSocket::ConnectedState) {
        const qint64 queued = qMax<qint64>(0, socket->sendBinaryMessage(data));
        pendingBytes_[clientId] += queued;
        addDownloadBacklog(clientId, queued);
    }
}
