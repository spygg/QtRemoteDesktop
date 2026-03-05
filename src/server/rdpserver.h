// server/rdp_server.h
#ifndef RDP_SERVER_H
#define RDP_SERVER_H

#include <QHostAddress>
#include <QImage>
#include <QJsonObject>
#include <QObject>
#include <QSslKey>
#include <QSslSocket>
#include <QTcpServer>
#include <QTcpSocket>
#include <memory>

class WebSocketServer;
class ScreenCapturer;

#ifdef USE_FFMPEG
class VideoEncoder;
#endif

class InputManager;

class RDPServer : public QObject {
    Q_OBJECT

public:
    explicit RDPServer(QObject* parent = nullptr);
    ~RDPServer();

    bool initialize(quint16 wsPort = 8080);
    void start();

    void loadSslConfig();
private slots:
    void onClientConnected(const QString& clientId);
    void onClientDisconnected(const QString& clientId);
    void onInputReceived(const QString& clientId, const QJsonObject& input);
    void onFrameCaptured(const QImage& frame);
    void onEncodedFrame(const QByteArray& data, bool isKeyframe, qint64 timestamp);
    void onHttpNewConnection();
    void onHttpRequest();
    void onCodecConfigChanged(const QByteArray& extradata);

    void onFrameForImageMode(const QImage& frame); // 图片模式下的帧处理槽

    void onModeChangeRequested(const QString& mode);

private:
    void setupHttpServer();
    QByteArray loadHtmlResource();

    void sendJpegFrame(const QImage& frame); // JPEG 压缩并发送

    // std::unique_ptr<QTcpServer> httpServer_;
    // 自定义 SSL TCP 服务器
    class SslTcpServer : public QTcpServer {
    public:
        SslTcpServer(RDPServer* server)
            : m_server(server)
        {
        }

    protected:
        void incomingConnection(qintptr socketDescriptor) override
        {
            m_server->handleIncomingSslConnection(socketDescriptor);
        }

    private:
        RDPServer* m_server;
    };
    friend class SslTcpServer;
    void handleIncomingSslConnection(qintptr socketDescriptor);

    std::unique_ptr<SslTcpServer> httpServer_; // 改为自定义服务器
    std::unique_ptr<WebSocketServer> wsServer_;
    std::unique_ptr<ScreenCapturer> screenCapturer_;
#ifdef USE_FFMPEG
    std::unique_ptr<VideoEncoder> videoEncoder_;
#endif

    std::unique_ptr<InputManager> inputManager_;

    bool isRunning_ = false;
    quint16 httpPort_;
    quint16 wsPort_;

    QRect screenGeometry_; // 存储屏幕几何信息

    QSslConfiguration* sslConfiguration_;

    // bool useVideoMode_ = true; // 默认真实用视频编码模式

    enum class ServerMode { Video,
        Image };
    ServerMode currentMode_ = ServerMode::Video; // 默认为视频模式

    void switchToImageMode();
    bool switchToVideoMode(); // 返回是否切换成功
};

#endif
