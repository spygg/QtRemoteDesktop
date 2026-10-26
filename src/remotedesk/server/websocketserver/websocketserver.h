#ifndef WEBSOCKETSERVER_H
#define WEBSOCKETSERVER_H

#include <QHostAddress>
#include <QMap>
#include <QJsonObject>
#include <QMutex>
#include <QObject>
#include <QSet>
#include <QTimer>
#include <QUuid>
#include <QWebSocket>
#include <QWebSocketServer>
#include <atomic>

class WebSocketServer : public QObject {
    Q_OBJECT
public:
    explicit WebSocketServer(QWebSocketServer::SslMode mode, QObject* parent = nullptr);
    ~WebSocketServer();

    bool listen(const QHostAddress& address = QHostAddress::Any, quint16 port = 8080);

    // 发送视频帧给所有客户端
    void broadcastFrame(const QByteArray& data, bool isKeyframe, qint64 timestamp);

    // 直接发送一帧给指定客户端（用于新客户端接入时立即回放最近关键帧，
    // 绕过 videoStarted_ 等待门，避免静态桌面永远等不到关键帧而黑屏）。
    void sendFrameToClient(const QString& clientId, const QByteArray& data,
                           bool isKeyframe, qint64 timestamp);

    // 发送 JSON 给指定客户端
    void sendJson(const QString& clientId, const QJsonObject& data);
    QStringList clients() const { return clients_.keys(); }
    // 仅判空/计数的调用点（每帧出帧、光标广播）用这个：clients() 会每次
    // 构造一份含所有 clientId 的 QStringList，每帧堆分配纯属浪费。
    int clientCount() const { return clients_.size(); }
    // 是否存在需要视频帧的 WS 客户端（排除仅走 RTP 的 WebRTC 客户端）。
    // 供服务端判断是否需要周期补关键帧——纯 ffmpeg/MSE 模式下没有
    // WebRTC 会话，若只检查 webrtcSessions_ 就永远不补帧，静态画面冻结。
    bool hasVideoClients() const {
        for (auto it = clients_.constBegin(); it != clients_.constEnd(); ++it) {
            if (!mediaExcludedClients_.contains(it.key()))
                return true;
        }
        return false;
    }
    QString clientToken(const QString& clientId) const;
    void dropClient(const QString& clientId);

    // [C5-①] 下载分块背压：sendBinaryToClient 排入下载字节时累加，
    // socket 实际写出时扣减。文件传输工作线程在产出下一块前轮询该值，
    // 低于阈值才继续——否则 GB 级目录会把信号队列 + 套接字写队列撑爆（OOM）。
    void addDownloadBacklog(const QString& clientId, qint64 delta);
    qint64 downloadBacklog() const { return downloadBacklog_.load(std::memory_order_relaxed); }

    // 跨线程存活性查询：clients_ 只在主线程读写，而文件传输工作线程需要判断
    // 下载客户端是否还活着（否则断线后仍会把整个目录打包完再"发"给死连接）。
    // 维护一份只含在线 clientId 的集合（最多 kMaxClients 条，天然有界）。
    bool isClientAlive(const QString& clientId) const;

    void broadcastCodecConfig(const QByteArray& extra);
    void broadcastJson(const QJsonObject& data);
    void setSslConfiguration(const QSslConfiguration& config);

    void broadcastBinary(const QByteArray& data);

    // 指定走 WebRTC 视频的客户端（跳过 WS 视频帧广播，避免重复传流）
    void setMediaExcludedClients(const QSet<QString>& clients);

    // 发送二进制数据给指定客户端
    void sendBinaryToClient(const QString& clientId, const QByteArray& data);

    // 发送 JSON 给 capture helper 进程
    void sendToCaptureSource(const QJsonObject& data);
    bool isCaptureSourceConnected() const {
        return captureSource_ && captureSource_->state() == QAbstractSocket::ConnectedState;
    }

    void sendToSecureInput(const QJsonObject& data);
    bool isSecureInputConnected() const {
        return secureInputSource_ && secureInputSource_->state() == QAbstractSocket::ConnectedState;
    }
    void closeSecureInput();

signals:
    void clientConnected(const QString& clientId);
    void clientDisconnected(const QString& clientId);
    void inputReceived(const QString& clientId, const QJsonObject& data);
    void codecChangeRequested(const QString& codec);
    void modeChangeRequested(const QString& mode);
    void fileChunkReceived(const QString& clientId, const QString& path, const QByteArray& data);
    void captureFrameReceived(const QByteArray& jpegData);
    // 采集源（helper）上报的 H.264 编码帧：[1字节类型(0x01=IDR/0x02=P)][u32 长度][i64 时间戳][数据]
    // 与服务端 → 浏览器的 13 字节头完全一致，服务端可直接复用 onEncodedFrame 分发。
    void captureVideoFrameReceived(const QByteArray& data, bool isKeyframe, qint64 timestamp);
    void captureMessageReceived(const QJsonObject& msg);
    void captureSourceConnected();
    void captureSourceDisconnected();
    void shellConnected(QWebSocket* socket);

private slots:
    void onNewConnection();
    void onSocketDisconnected();
    void onTextMessageReceived(const QString& message);
    void onBinaryMessageReceived(const QByteArray& message);

private:
    // 单个已完成握手的 socket 的路由与挂接（onNewConnection 逐个 pending 调用）
    void handleNewSocket(QWebSocket* socket);
    // 释放某客户端的下载积压份额（断线/被踢时调用）。只扣该客户端自己的份额，
    // 不再像旧实现那样把全局计数清零——否则其它客户端正在受控的下载节流会被
    // 一并解除，GB 级目录传输重新撑爆信号队列与套接字写队列。
    void releaseDownloadBacklog(const QString& clientId);
    // 记录客户端最近一次活动时间，配合 idleTimer_ 清理半开连接
    void touchClient(const QString& clientId);
    QWebSocketServer* server_;
    QMap<QString, QWebSocket*> clients_;
    QSet<QString> mediaExcludedClients_;
    QSet<QString> videoStarted_;
    // 每客户端“已交给套接字但尚未写出”的字节数（QWebSocket 5.9 未公开 bytesToWrite，
    // 用 sendXxxMessage 的返回值累加、bytesWritten 递减来估算）。用于慢客户端背压：
    // 超过阈值即丢弃其媒体帧，避免发送队列无限增长吃光内存。
    QMap<QString, qint64> pendingBytes_;
    // [C5-①] 下载分块未写出字节数（跨线程原子量：工作线程读，主线程增减）
    std::atomic<qint64> downloadBacklog_{0};
    // 分客户端下载未写出份额（仅主线程读写）：断线时只扣当事人，见 releaseDownloadBacklog
    QMap<QString, qint64> downloadBacklogByClient_;
    // 在线客户端快照（互斥量保护，工作线程可读）：仅用于跨线程判断下载客户端是否断线
    mutable QMutex aliveMutex_;
    QSet<QString> aliveClients_;
    // 每客户端最近一次上行消息时间（毫秒时钟）：半开连接（对端掉电/断网无 FIN）
    // 永远不会触发 disconnected，会一直占着 clients_ 且让 hasVideoClients() 恒真
    // → 服务端为不存在的观众持续编码推流。idleTimer_ 定期清理。
    QMap<QString, qint64> lastActivityMs_;
    QTimer idleTimer_;
    QMap<QWebSocket*, QString> socketToId_;
    // 在途交互式 shell 连接（/api/shell/ws）：每条会 spawn 一个 PTY/bash，
    // 独立于 clients_ 限流。
    QSet<QWebSocket*> shellSockets_;
    // shell 连接最近一次 pong 时间（毫秒时钟）：shell socket 不进 clients_，
    // 不受上面的 idle 清理覆盖。对端掉电/断网（无 FIN）时 disconnected 永不触发，
    // bash + PTY + 一个并发配额会永久泄漏，只能靠 ping/pong 判活。
    QMap<QWebSocket*, qint64> shellLastPongMs_;
    QMap<QString, QString> clientTokens_;
    QSslConfiguration sslConfig_;
    QWebSocket* captureSource_ = nullptr;
    // /capture 上最近一次收到帧/消息的时间：捕获 helper 正常时每帧都有上行数据，
    // 长时间静默说明连接已半开（对端掉电/断网无 FIN），需主动清理，
    // 否则 captureSource_ 句柄与 helper 进程常驻，且 isCaptureSourceConnected() 恒真。
    qint64 captureLastDataMs_ = 0;
    QWebSocket* secureInputSource_ = nullptr;
    // /secure-input 是单向通道（只有服务端→helper），不能用"静默时长"判活：
    // 对端掉电/崩溃无 FIN 时 socket 会常驻，isSecureInputConnected() 恒真，
    // 锁屏时服务端就不再重启 helper，密码永远注入不进去。改为周期性 ping +
    // 记录最近一次 pong 时间，超时即丢弃该连接。
    qint64 secureInputLastPongMs_ = 0;
};

#endif // WEBSOCKETSERVER_H
