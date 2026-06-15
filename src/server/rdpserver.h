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
#include <QThread>
#include <memory>

class WebSocketServer;
class ScreenCapturer;
class AuthManager;
class FileTransferService;

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

    void onFrameForImageMode(const QImage& frame);

    void onModeChangeRequested(const QString& mode);

signals:
    void requestFileList(const QString& clientId, const QString& path);
    void requestDownload(const QString& clientId, const QString& path);
    void requestUploadStart(const QString& clientId, const QString& path, qint64 size);
    void requestUploadDone(const QString& clientId, const QString& path);

private:
    void setupHttpServer();
    QByteArray loadHtmlResource();
    QByteArray loadLoginHtml();
    void serveLoginPage(QTcpSocket* socket);
    void handleLoginPost(QTcpSocket* socket, const QByteArray& body);
    void handleApiUsers(QTcpSocket* socket);
    void handleApiAddUser(QTcpSocket* socket, const QByteArray& body);
    void handleApiDeleteUser(QTcpSocket* socket, const QByteArray& body);
    QString extractSessionToken(const QByteArray& request);
    QByteArray buildHttpResponse(int statusCode, const QString& statusText,
                                 const QString& contentType, const QByteArray& body,
                                 const QString& extraHeaders = QString());

    void sendJpegFrame(const QImage& frame);

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

    std::unique_ptr<SslTcpServer> httpServer_;
    std::unique_ptr<WebSocketServer> wsServer_;
    std::unique_ptr<ScreenCapturer> screenCapturer_;
#ifdef USE_FFMPEG
    std::unique_ptr<VideoEncoder> videoEncoder_;
#endif

    std::unique_ptr<InputManager> inputManager_;

    AuthManager* authManager_ = nullptr;
    FileTransferService* fileTransferService_ = nullptr;
    QThread* transferThread_ = nullptr;

    bool isRunning_ = false;
    quint16 httpPort_;
    quint16 wsPort_;

    QRect screenGeometry_;
    QPoint lastCursorPos_{-1, -1};

    QSslConfiguration* sslConfiguration_;

    enum class ServerMode { Video,
        Image };
    ServerMode currentMode_ = ServerMode::Video;

    void switchToImageMode();
    bool switchToVideoMode();
};

#endif
