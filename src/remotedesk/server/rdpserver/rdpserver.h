// server/rdp_server.h
#ifndef RDP_SERVER_H
#define RDP_SERVER_H

#include <QHostAddress>
#include <QHash>
#include <QImage>
#include <QJsonObject>
#include <QMutex>
#include <QObject>
#include <QQueue>
#include <QSet>
#include <QSslKey>
#include <QSslSocket>
#include <QTcpServer>
#include <QTcpSocket>
#include <QThread>
#include <QWaitCondition>
#include <atomic>
#include <memory>

#include "shell.h"

class WebSocketServer;
class ScreenCapturer;
class AuthManager;
class FileTransferService;
class ClipboardService;
class QProcess;

#ifdef USE_FFMPEG
class VideoEncoder;
#endif

#ifdef USE_WEBRTC
class WebRtcSession;
#endif

class InputManager;

// HTTP 请求解析状态：将请求头与 POST body 在多次 readyRead 之间累积，
// 避免在主线程用 waitForReadyRead 同步阻塞等待 TCP 分片的 body 到齐。
struct HttpParseState {
    bool headerDone = false;     // 请求头是否已解析（含 Content-Length）
    int contentLength = 0;       // 期望的 POST body 字节数
    QString headerText;          // 已解析的请求头文本（用于路径/方法/鉴权提取）
    QByteArray buffer;           // 累积的原始字节（请求头 + 已到 body 片段）
};

// 独立线程 JPEG 压缩器：捕获帧不经主线程阻塞，直接在后台压缩
class JpegCompressor : public QObject {
    Q_OBJECT
public:
    explicit JpegCompressor(QObject* parent = nullptr);
    ~JpegCompressor();

    void start() { thread_.start(); }
    void enqueue(const QImage& frame);
    void shutdown();
    void setQuality(int q) { quality_ = q; }
    // 缩放档位：JPEG 线程内先缩放再压缩，避免全帧缩放占用主线程
    void setScalePercent(int p) { scalePercent_ = p; }

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
    std::atomic<int> quality_ { 35 };
    std::atomic<int> scalePercent_ { 100 };
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
    bool restartCapture();
    bool isCaptureConnected() const;
    bool isCaptureSourceConnected() const;
    bool isScreenLocked() const { return screenLocked_; }

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
    void onShellConnected(QWebSocket* socket);
    // logind PrepareForSleep（Linux）：挂起前暂停捕获、恢复后重建捕获流
    void onPrepareForSleep(bool sleeping);
#ifdef USE_WEBRTC
    void onWebRtcMessage(const QString& clientId, const QJsonObject& msg);
#endif

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
    int videoBitrateFor(int encW, int encH, int fps) const;
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
    std::unique_ptr<ClipboardService> clipboardService_;

    AuthManager* authManager_ = nullptr;
    FileTransferService* fileTransferService_ = nullptr;
    QThread* transferThread_ = nullptr;

    bool isRunning_ = false;
    quint16 httpPort_;
    quint16 wsPort_;

    QRect screenGeometry_;
    QImage lastCapturedFrame_;
    QPoint lastCursorPos_ { -1, -1 };
    qint64 lastCursorQueryMs_ = 0;
    int videoBaseBitrate_ = 0; // 视频模式目标码率（过载降质后用于恢复）
    QJsonObject lastScreenInfo_;

    bool useSsl_ = false;
    bool serviceMode_ = false;
    int configFps_ = 30;
    int configQuality_ = 60;
    int configScale_ = 75;
    int userScale_ = 75;
    bool screenLocked_ = false;
    bool secureInputRunning_ = false;
    bool captureAvailable_ = true;
    QString shellCurrentDir_;

    // 每个 HTTP 连接的请求解析状态（跨 readyRead 累积，避免主线程阻塞）
    QHash<QTcpSocket*, HttpParseState> httpParseState_;
#ifdef _WIN32
    int secureInputPid_ = 0;
#endif
    QSslConfiguration* sslConfiguration_;
    QByteArray lastCodecExtra_;

    enum class ServerMode { Video,
        Image };
    ServerMode currentMode_ = ServerMode::Video;

    void switchToImageMode();
    bool switchToVideoMode();
    void reinitVideoEncoderForScale();
    bool hwEncodeAvailable() const;

    void loadServerConfig(const QString& configPath);
    void saveServerConfig(const QString& configPath);
    void startSecureInputProcess();
    void stopSecureInputProcess();
    void injectPasteShortcut();

    // ---- 防睡眠 / 休眠恢复 ----
    // 首个客户端连接时阻止被控端睡眠/熄屏，全部断开时释放
    void updateSleepInhibit(bool active);
    // Linux：订阅 logind PrepareForSleep 信号（幂等，可在 start() 调用）
    void connectSleepSignals();

    // ---- 系统操作（快捷键面板） ----
    // 执行被控端系统动作：lock/show_desktop/task_manager/logout/reboot/poweroff
    void handleSystemAction(const QString& action, const QString& clientId);

    bool sleepSignalsConnected_ = false;
    bool sleepInhibitActive_ = false;
    bool preparedForSleep_ = false;
#ifdef Q_OS_LINUX
    enum class InhibitBackend { None, SessionManager, ScreenSaver, Login1 };
    InhibitBackend sleepInhibitBackend_ = InhibitBackend::None;
    quint32 sleepInhibitCookie_ = 0; // org.gnome.SessionManager / ScreenSaver 返回的 cookie
    int sleepInhibitFd_ = -1;        // login1 Inhibit 返回的 fd（最后手段），关闭即释放
#endif
    QProcess* sleepInhibitProcess_ = nullptr; // macOS: caffeinate 子进程

#ifdef USE_WEBRTC
    void startWebRtcSession(const QString& clientId);
    void stopWebRtcSession(const QString& clientId);
    void sendWebRtcToClient(const QString& clientId, const QJsonObject& data);
    QMap<QString, WebRtcSession*> webrtcSessions_;
    QSet<QString> webrtcExcluded_;
    QVector<QString> webrtcIceServers_;
#endif

public:
    static QStringList getLocalIpAddr();
};

#endif
