#include "websocketserver.h"
#include <QDataStream>
#include <QDebug>
#include <QJsonDocument>
#include <QJsonObject>
#include <QUrl>
#include <QUrlQuery>

// 单客户端待发缓冲上限：慢客户端（网络拥塞、标签页被挂起/切到后台）会导致
// sendBinaryMessage 的发送队列无限增长，最终吃光服务端内存。超过该阈值时主动
// 丢弃该客户端的媒体帧（图像/视频），待其把积压发完再自然恢复。
// 只作用于媒体广播，不碰控制类 JSON（体量小且不能丢）。
static const qint64 kMaxSocketBacklogBytes = 8 * 1024 * 1024;

WebSocketServer::WebSocketServer(QWebSocketServer::SslMode mode, QObject* parent)
    : QObject(parent)
    , server_(new QWebSocketServer(QStringLiteral("RemoteDesktopServer"),
          mode,
          this))
{
    connect(server_, &QWebSocketServer::newConnection,
        this, &WebSocketServer::onNewConnection);
}

WebSocketServer::~WebSocketServer()
{
    // 先断开信号，防止删除时触发 onSocketDisconnected
    for (QWebSocket* socket : clients_.values()) {
        disconnect(socket, nullptr, this, nullptr);
    }
    if (captureSource_) {
        disconnect(captureSource_, nullptr, this, nullptr);
        captureSource_->deleteLater();
        captureSource_ = nullptr;
    }

    server_->close();
    qDeleteAll(clients_);
    clients_.clear();
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
    QWebSocket* socket = server_->nextPendingConnection();

    QUrl url = socket->requestUrl();
    // 只记 path，不打完整 URL：URL 的 ?token= 携带会话令牌，写入日志文件会泄漏；
    // 连接属低频事件，降为 qDebug 避免默认日志噪音
    qDebug() << "WS new connection, path:" << url.path();

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
            captureSource_->deleteLater();
        }
        captureSource_ = socket;
        emit captureSourceConnected();
        connect(socket, &QWebSocket::disconnected, this, [this, socket]() {
            qWarning() << "Helper disconnected from /capture";
            if (captureSource_ == socket) {
                captureSource_ = nullptr;
            }
            socket->deleteLater();
        });
        connect(socket, &QWebSocket::binaryMessageReceived, this, [this](const QByteArray& msg) {
            if (msg.size() < 5) return;
            QDataStream stream(msg);
            stream.setByteOrder(QDataStream::BigEndian);
            quint8 frameType;
            quint32 dataLen;
            stream >> frameType >> dataLen;
            if (frameType != 0x03) return;
            emit captureFrameReceived(msg.mid(5));
        });
        connect(socket, &QWebSocket::textMessageReceived, this, [this](const QString& text) {
            QJsonDocument doc = QJsonDocument::fromJson(text.toUtf8());
            if (doc.isObject())
                emit captureMessageReceived(doc.object());
        });
        return;
    }

    if (url.path() == "/api/shell/ws") {
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
            secureInputSource_->deleteLater();
        }
        secureInputSource_ = socket;
        connect(socket, &QWebSocket::disconnected, this, [this, socket]() {
            qWarning() << "Secure input helper disconnected";
            if (secureInputSource_ == socket)
                secureInputSource_ = nullptr;
            socket->deleteLater();
        });
        return;
    }

    // 普通远程客户端
    QString clientId = QUuid::createUuid().toString();

    QString token;
    #if QT_VERSION >= QT_VERSION_CHECK(5, 12, 0)
        token = QUrlQuery(url).queryItemValue("token");
    #else
        QUrlQuery query(url);
        token = query.queryItemValue("token");
    #endif

    clients_[clientId] = socket;
    socketToId_[socket] = clientId;
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
    });

    emit clientConnected(clientId);
}

void WebSocketServer::onSocketDisconnected()
{
    QWebSocket* socket = qobject_cast<QWebSocket*>(sender());
    if (!socket)
        return;

    QString clientId = socketToId_.take(socket);
    clients_.remove(clientId);
    videoStarted_.remove(clientId);
    pendingBytes_.remove(clientId);
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

    QJsonParseError error;
    QJsonDocument doc = QJsonDocument::fromJson(message.toUtf8(), &error);
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
    } else {
        emit inputReceived(clientId, obj);
    }
}

void WebSocketServer::onBinaryMessageReceived(const QByteArray& message)
{
    if (message.size() < 1) return;
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

        emit fileChunkReceived(path, data);
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
    // 断开信号连接，避免 close() 触发的 disconnected 再次走 onSocketDisconnected 造成重复处理
    disconnect(socket, nullptr, this, nullptr);
    socketToId_.remove(socket);
    clientTokens_.remove(clientId);
    videoStarted_.remove(clientId);
    pendingBytes_.remove(clientId);
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

    // 构造二进制包： [1字节帧类型] [4字节大端长度] [8字节大端时间戳] [数据]
    QByteArray packet;
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

    for (auto it = clients_.constBegin(); it != clients_.constEnd(); ++it) {
        QWebSocket* socket = it.value();
        if (socket->state() == QAbstractSocket::ConnectedState) {
            pendingBytes_[it.key()] += qMax<qint64>(0,
                socket->sendTextMessage(QString::fromUtf8(message)));
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
    QWebSocket* socket = clients_.value(clientId);
    if (socket && socket->state() == QAbstractSocket::ConnectedState) {
        pendingBytes_[clientId] += qMax<qint64>(0, socket->sendBinaryMessage(data));
    }
}
