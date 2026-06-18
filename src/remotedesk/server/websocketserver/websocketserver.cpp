#include "websocketserver.h"
#include <QDataStream>
#include <QDebug>
#include <QJsonDocument>
#include <QJsonObject>
#include <QUrl>
#include <QUrlQuery>

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
    qInfo() << "WS new connection, path:" << url.path() << "URL:" << url.toString();
    if (url.path() == "/capture") {
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

    if (url.path() == "/secure-input") {
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

    emit clientConnected(clientId);
}

void WebSocketServer::onSocketDisconnected()
{
    QWebSocket* socket = qobject_cast<QWebSocket*>(sender());
    if (!socket)
        return;

    QString clientId = socketToId_.take(socket);
    clients_.remove(clientId);
    socket->deleteLater();

    emit clientDisconnected(clientId);
}

void WebSocketServer::onTextMessageReceived(const QString& message)
{
    QWebSocket* socket = qobject_cast<QWebSocket*>(sender());
    if (!socket)
        return;
    QString clientId = socketToId_[socket];

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

        if (message.size() < (int)(9 + pathLen + dataLen)) return;

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
    QWebSocket* socket = clients_.value(clientId);
    if (socket) {
        socket->close(QWebSocketProtocol::CloseCodeNormal, "Authentication failed");
        socket->deleteLater();
        clients_.remove(clientId);
        socketToId_.remove(socket);
        clientTokens_.remove(clientId);
    }
}

void WebSocketServer::closeSecureInput()
{
    if (secureInputSource_) {
        secureInputSource_->close();
        secureInputSource_ = nullptr;
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

    for (QWebSocket* socket : clients_.values()) {
        if (socket->state() == QAbstractSocket::ConnectedState) {
            socket->sendBinaryMessage(packet);
        }
    }
}

void WebSocketServer::sendJson(const QString& clientId, const QJsonObject& data)
{
    QWebSocket* socket = clients_.value(clientId);
    if (socket && socket->state() == QAbstractSocket::ConnectedState) {
        QJsonDocument doc(data);
        socket->sendTextMessage(QString::fromUtf8(doc.toJson(QJsonDocument::Compact)));
    }
}

void WebSocketServer::broadcastJson(const QJsonObject& data)
{
    if (clients_.isEmpty())
        return;

    QJsonDocument doc(data);
    QByteArray message = doc.toJson(QJsonDocument::Compact);

    for (QWebSocket* socket : clients_.values()) {
        if (socket->state() == QAbstractSocket::ConnectedState) {
            socket->sendTextMessage(QString::fromUtf8(message));
        }
    }
}

void WebSocketServer::broadcastBinary(const QByteArray& data)
{
    if (clients_.isEmpty())
        return;

    for (QWebSocket* socket : clients_.values()) {
        if (socket->state() == QAbstractSocket::ConnectedState) {
            socket->sendBinaryMessage(data);
        }
    }
}

void WebSocketServer::sendBinaryToClient(const QString& clientId, const QByteArray& data)
{
    QWebSocket* socket = clients_.value(clientId);
    if (socket && socket->state() == QAbstractSocket::ConnectedState) {
        socket->sendBinaryMessage(data);
    }
}
