// server/rdp_server.h
#ifndef RDP_SERVER_H
#define RDP_SERVER_H

#include <QHostAddress>
#include <QImage>
#include <QJsonObject>
#include <QMutex>
#include <QObject>
#include <QQueue>
#include <QSslKey>
#include <QSslSocket>
#include <QTcpServer>
#include <QTcpSocket>
#include <QThread>
#include <QWaitCondition>
#include <atomic>
#include <memory>

class WebSocketServer;
class ScreenCapturer;
class AuthManager;
class FileTransferService;

#ifdef USE_FFMPEG
class VideoEncoder;
#endif

class InputManager;

// 独立线程 JPEG 压缩器：捕获帧不经主线程阻塞，直接在后台压缩
class JpegCompressor : public QObject {
    Q_OBJECT
public:
    explicit JpegCompressor(QObject* parent = nullptr);
    ~JpegCompressor();

    void start() { thread_.start(); }
    void enqueue(const QImage& frame);
    void shutdown();

signals:
    void jpegCompressed(const QByteArray& data);

private slots:
    void processLoop();

private:
    QThread thread_;
    QMutex mutex_;
    QWaitCondition cond_;
    QQueue<QImage> queue_;
    std::atomic<bool> abort_ { false };
    enum { kMaxQueueSize = 5 };
};

class RDPServer : public QObject {
    Q_OBJECT

public:
    explicit RDPServer(QObject* parent = nullptr);
    ~RDPServer();

    bool initialize(const QString& configPath = QString(), bool useSslOverride = true, bool serviceMode = false);
    void start();
    bool startCapture();
    bool isCaptureConnected() const;
    bool isCaptureSourceConnected() const;

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

    void onJpegCompressed(const QByteArray& data);

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
    void handleShellExec(QTcpSocket* socket, const QByteArray& body);
    QString extractSessionToken(const QByteArray& request);
    QByteArray buildHttpResponse(int statusCode, const QString& statusText,
        const QString& contentType, const QByteArray& body,
        const QString& extraHeaders = QString());

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
    std::unique_ptr<JpegCompressor> jpegCompressor_;

    AuthManager* authManager_ = nullptr;
    FileTransferService* fileTransferService_ = nullptr;
    QThread* transferThread_ = nullptr;

    bool isRunning_ = false;
    quint16 httpPort_;
    quint16 wsPort_;

    QRect screenGeometry_;
    QPoint lastCursorPos_ { -1, -1 };

    bool useSsl_ = false;
    bool serviceMode_ = false;
    bool screenLocked_ = false;
    bool secureInputRunning_ = false;
    bool captureAvailable_ = true;
#ifdef _WIN32
    int secureInputPid_ = 0;
#endif
    QSslConfiguration* sslConfiguration_;

    enum class ServerMode { Video,
        Image };
    ServerMode currentMode_ = ServerMode::Video;

    void switchToImageMode();
    bool switchToVideoMode();

    void loadServerConfig(const QString& configPath);
    void saveServerConfig(const QString& configPath);
    void startSecureInputProcess();
    void stopSecureInputProcess();

public:
    static QStringList getLocalIpAddr();
};

#endif
