#ifndef WEBSOCKETSERVER_H
#define WEBSOCKETSERVER_H

#include <QHostAddress>
#include <QMap>
#include <QObject>
#include <QUuid>
#include <QWebSocket>
#include <QWebSocketServer>

class WebSocketServer : public QObject {
    Q_OBJECT
public:
    explicit WebSocketServer(QWebSocketServer::SslMode mode, QObject* parent = nullptr);
    ~WebSocketServer();

    bool listen(const QHostAddress& address = QHostAddress::Any, quint16 port = 8080);

    // 发送视频帧给所有客户端
    void broadcastFrame(const QByteArray& data, bool isKeyframe, qint64 timestamp);

    // 发送 JSON 给指定客户端
    void sendJson(const QString& clientId, const QJsonObject& data);
    QStringList clients() const { return clients_.keys(); }

    void broadcastCodecConfig(const QByteArray& extra);
    void broadcastJson(const QJsonObject& data);
    void setSslConfiguration(const QSslConfiguration& config); // 新增

    void broadcastBinary(const QByteArray& data);

    // 发送二进制数据给指定客户端
    void sendBinaryToClient(const QString& clientId, const QByteArray& data);

signals:
    void clientConnected(const QString& clientId);
    void clientDisconnected(const QString& clientId);
    void inputReceived(const QString& clientId, const QJsonObject& data);
    void codecChangeRequested(const QString& codec); // 客户端请求切换编码器
    void modeChangeRequested(const QString& mode); // 新增
    void fileChunkReceived(const QString& path, const QByteArray& data); // 文件上传数据块

private slots:
    void onNewConnection();
    void onSocketDisconnected();
    void onTextMessageReceived(const QString& message);
    void onBinaryMessageReceived(const QByteArray& message);

private:
    QWebSocketServer* server_;
    QMap<QString, QWebSocket*> clients_;
    QMap<QWebSocket*, QString> socketToId_;
    QSslConfiguration sslConfig_;
};

#endif // WEBSOCKETSERVER_H
