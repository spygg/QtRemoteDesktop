// server/rdp_server.h
#ifndef RDP_SERVER_H
#define RDP_SERVER_H

#include <QElapsedTimer>
#include <QFile>
#include <QHostAddress>
#include <QHash>
#include <QImage>
#include <QJsonObject>
#include <QMutex>
#include <QObject>
#include <QQueue>
#include <QSet>
#include <QStringList>
#include <QSslKey>
#include <QSslSocket>
#include <QTcpServer>
#include <QTcpSocket>
#include <QThread>
#include <QWaitCondition>
#include <atomic>
#include <memory>
#include "videoencoder.h"

#include "shell.h"
#include "filetransferservice.h" // TarEntry（目录 tar 流式下载）

class WebSocketServer;
class ScreenCapturer;
class AuthManager;
class FileTransferService;
class ClipboardService;
class XdndMonitor;
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
    enum { kMaxQueueSize = 1 }; // 只编最新帧，避免编码慢时 FIFO 队列造成画面延迟累积
};

// HTTP 直链下载（/api/file）的传输状态：大文件必须分块流式写出，
// 一次性 write 整份数据会让主线程长时间阻塞（数 GB 文件 = 整个远程会话冻结），
// 因此按 socket 的写缓冲背压，配合 bytesWritten 信号持续泵送。
struct HttpDownloadState {
    std::unique_ptr<QFile> file; // 单文件：从磁盘流式读；tar 流模式下为当前正在读取的成员文件
    QByteArray payload;          // 兼容保留（旧整包路径）；HTTP 目录下载已改走 tarQueue_ 流式生成
    qint64 totalSize = 0;        // 未分片时的完整长度（用于 Content-Range: bytes s-e/total）
    qint64 offset = 0;           // 下一个待写字节位置
    qint64 end = 0;              // 本次响应最后一个字节位置（含）
    // ---- [P1 perf] 目录 tar 流式生成状态（GB 级目录不再整包进内存）----
    bool tarStream = false;      // true 时由 generateTarChunk 按需产出字节
    QList<FileTransferService::TarEntry> tarQueue; // 待打包条目（collectTarEntries 产出）
    int tarQueueIdx = 0;
    qint64 tarFileRemaining = 0; // 当前成员文件剩余字节（file 为空时按零填充保持长度）
    qint64 curEntrySize = 0;     // 当前成员文件声明大小（对齐填充计算用）
    int tarPadRemaining = 0;     // 当前条目的对齐填充 / tar 结束块剩余字节
    QByteArray pendingHeader;    // 待发的 512B tar header（可分片发出）
    bool tarEndSent = false;
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
    void broadcastCursorPos(); // [流畅度] 光标位置独立定时器广播（与视频帧解耦）
    void onEncodedFrame(const QByteArray& data, bool isKeyframe, qint64 timestamp);
    void onHttpNewConnection();
    void onHttpRequest();
    void onCodecConfigChanged(const QByteArray& extradata);

    void onJpegCompressed(const QByteArray& data);

    void onModeChangeRequested(const QString& mode);
    void onShellConnected(QWebSocket* socket);
    // logind PrepareForSleep（Linux）：挂起前暂停捕获、恢复后重建捕获流
    void onPrepareForSleep(bool sleeping);
    // IME 工作线程结果落地（主线程）：sendJson 操作 QWebSocket 必须在主线程
    void onImeResultReady(const QString& clientId, const QJsonObject& state);
#ifdef USE_WEBRTC
    void onWebRtcMessage(const QString& clientId, const QJsonObject& msg);
#endif

signals:
    void requestFileList(const QString& clientId, const QString& path);
    void requestDownload(const QString& clientId, const QString& path);
    void requestUploadStart(const QString& clientId, const QString& path, qint64 size);
    void requestUploadDone(const QString& clientId, const QString& path);
    // IME 工作线程 → 主线程结果回传（auto=跨线程 queued 投递）
    void imeResultReady(const QString& clientId, const QJsonObject& state);

private:
    void setupHttpServer();
    QByteArray loadHtmlResource();
    QByteArray loadLoginHtml();
    void serveLoginPage(QTcpSocket* socket);
    void handleLoginPost(QTcpSocket* socket, const QByteArray& body);
    void handleApiUsers(QTcpSocket* socket);
    void handleApiAddUser(QTcpSocket* socket, const QByteArray& body);
    void handleApiDeleteUser(QTcpSocket* socket, const QByteArray& body);
    void handleShellExec(QTcpSocket* socket, const QByteArray& body, const QString& sessionToken);
    // [B9] 按会话令牌隔离的 shell 工作目录（旧实现用进程级 QDir::setCurrent +
    // 单一 shellCurrentDir_，多客户端 cd 互相干扰且污染全进程 CWD）
    QString shellCwdFor(const QString& sessionToken);
    void setShellCwd(const QString& sessionToken, const QString& path);
    QString extractSessionToken(const QByteArray& request);

    // HTTP 直链文件下载：GET /api/file?path=<受限根内的绝对路径>
    // 存在的意义：浏览器的拖拽/下载 API 对 WebSocket 分块传输的数据无能为力
    // （必须先把全部数据预取进内存再生成 Blob，且无法拖到桌面）。
    // 走标准 HTTP GET 后，浏览器可用 DownloadURL 把远程文件直接拖到本机桌面，
    // 由内核/浏览器接管断点续传与磁盘写入，不受 32MB 预取上限限制。
    void handleApiFileDownload(QTcpSocket* socket, const QString& path, const QString& headerText);
    // 按 socket 写缓冲背压泵送下一批数据（bytesWritten 触发）
    void pumpFileDownload(QTcpSocket* socket);
    // 目录 tar 流式生成：按需产出最多 maxBytes 字节（长度与 totalSize 严格一致）
    QByteArray generateTarChunk(HttpDownloadState* st, int maxBytes);
    void cleanupHttpDownload(QTcpSocket* socket);
    int videoBitrateFor(int encW, int encH, int fps, CodecType codec = CodecType::H264) const;
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
    // 缓存最近一帧关键帧，用于新客户端接入时立即回放以引导出图
    // （静态桌面抓取去重不会主动出帧，否则新客户端永远等不到关键帧而黑屏）。
    QByteArray lastKeyframeData_;
    qint64 lastKeyframeTs_ = 0;
    // 编码器尺寸不匹配触发的重建节流时间戳（避免每帧都尝试重建阻塞主线程）
    qint64 lastSizeReinitMs_ = 0;
#endif

    std::unique_ptr<InputManager> inputManager_;
    std::unique_ptr<JpegCompressor> jpegCompressor_;
    std::unique_ptr<ClipboardService> clipboardService_;

    // 远端屏幕文件拖拽侦测（XDND）：拖远端文件到屏幕边缘 → 浏览器下载。
    // 专用线程轮询 XdndSelection 所有权（见 server/xdndmonitor/xdndmonitor.h）。
    QThread* xdndThread_ = nullptr;
    XdndMonitor* xdndWorker_ = nullptr;
    QJsonObject lastRemoteDragMsg_;   // 最近一次拖拽状态广播（新客户端接入时补发）
    // 剪贴板读取是异步的（CLI 模式在工作线程里跑 xclip），这些是新接入、
    // 正等待初始剪贴板内容的客户端：读取结果回来后一次性推送。
    QStringList pendingClipboardClientIds_;

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

    bool useSsl_ = true; // 默认启用 HTTPS/WSS：WebRTC 需安全上下文；无 OpenSSL 时自动降级
    bool serviceMode_ = false;
    int configFps_ = 30;
    int configQuality_ = 60;
    int configScale_ = 75;
    CodecType configCodec_ = CodecType::H264;        // 编码协议（h264/hevc/vp8/vp9/av1）
    HwEncodeMode configHwEncodeMode_ = HwEncodeMode::Auto; // 硬件编码开关（auto/on/off）
    int userScale_ = 75;
    bool screenLocked_ = false;
    bool secureInputRunning_ = false;
    bool captureAvailable_ = true;
    QString shellCurrentDir_;
    QMap<QString, QString> shellCwdByToken_; // [B9] 会话令牌 → shell CWD
    // [B-1] 客户端 → 按下的键集合（code → 最近 keydown 载荷，保留 keycode 供
    // Windows VK 回放）。断开时逐个补发 keyup，否则按键永久卡死（无限自动
    // 重复 / 修饰键卡住全部变组合键）。
    QMap<QString, QHash<QString, QJsonObject>> pressedKeysByClient_;

    // 每个 HTTP 连接的请求解析状态（跨 readyRead 累积，避免主线程阻塞）
    QHash<QTcpSocket*, HttpParseState> httpParseState_;
    // 每个 socket 的直链下载传输状态（streaming 写出，写完自动清理）。
    // 存指针而非值：QHash 的插入/扩容都要求值可拷贝，而状态里持有独占文件句柄。
    QHash<QTcpSocket*, HttpDownloadState*> httpDownloadStates_;
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
    // 创建 VideoEncoder 并接好全部信号（此前两处创建点接线不一致，
    // 前台模式那处漏了 encoderReady/encoderOverload/reinitRequired）
    void createVideoEncoder();
    VideoEncoder* ensureVideoEncoder();

    void loadServerConfig(const QString& configPath);
    void saveServerConfig(const QString& configPath);
    QTimer* configSaveTimer_ = nullptr; // [P2] 去抖落盘定时器
    QTimer* cursorTimer_ = nullptr;     // [流畅度] 光标位置 33Hz 独立广播定时器
    // [P2] 配置去抖落盘：单条 config 消息可同时改 fps/scale/codec/hw_encode，
    // 逐字段同步写盘 = 一条消息 4 次全量 JSON 序列化+文件 IO。聚合到 800ms 一次。
    void scheduleSaveConfig();
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

    // ---- 输入法切换 ----
    // ibus panel 的 hotkey grab（如 Super+space）对 XTEST 注入的合成按键不响应，
    // 远程桌面里无法用注入快捷键触发切换；改为服务端直接调用 `ibus engine <next>`
    // 循环切换 preload-engines 列表（ibus CLI 走自身 socket 地址文件，不依赖 X grab）。
    void handleImeCycle(const QString& clientId);

    bool sleepSignalsConnected_ = false;
    bool sleepInhibitActive_ = false;
    bool preparedForSleep_ = false;
#ifdef Q_OS_LINUX
    enum class InhibitBackend { None, SessionManager, ScreenSaver, Login1 };
    InhibitBackend sleepInhibitBackend_ = InhibitBackend::None;
    quint32 sleepInhibitCookie_ = 0; // org.gnome.SessionManager / ScreenSaver 返回的 cookie
    int sleepInhibitFd_ = -1;        // login1 Inhibit 返回的 fd（最后手段），关闭即释放
    // [P1 perf] DBus 后端链异步化（原 BlockWithGui 同步调用最坏阻塞主线程 3s×3）
    void inhibitAcquireSessionManager();
    void inhibitAcquireScreenSaver();
    void inhibitAcquireLogin1();
    void inhibitRelease();
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

    // 周期关键帧定时器：仅当存在视频客户端时每 2s 强制出一帧 IDR。
    // 静态桌面下抓取去重（checksum 相同即丢帧）会让编码器长时间不产新帧，
    // 而编码器一旦已产过 IDR（hasIdr_=true）便不会主动再产。此时任何在 GOP
    // 中途加入、或刚重置等帧门的客户端都会永久拿不到 IDR 而丢弃全部 P 帧
    // → 画面完全冻结（ffmpeg/MSE 路径静默黑屏的主因）。
    // 定时器必须**无条件编译**：ffmpeg/MSE 客户端不在 webrtcSessions_ 里，
    // 若随 USE_WEBRTC 一起被裁掉，纯 WS 视频模式就再没有任何补关键帧机制。
    QTimer* webrtcKfTimer_ = nullptr;
    // 编码器重建冷却时间戳：pumpKeyframe 检测到无 IDR 时重建编码器，
    // 重建本身有线程开销且部分平台 MPP 库重复 init 会崩，需冷却后再重建。
    qint64 lastReinitMs_ = 0;

    // 强制出一帧关键帧：静态桌面抓取去重不会主动出帧，新客户端（FFmpeg/WebRTC）
    // 接入时需要一帧关键帧来引导出图，否则视频流永远起不来。
    // requestNewIdr=true 时（显式 request_keyframe）即使编码器已产过 IDR 也强制
    // 重新产一帧，用于客户端刚重置等帧门（WS videoStarted_）的场景。
    void pumpKeyframe(bool requestNewIdr = false);

public:
    static QStringList getLocalIpAddr();
};

#endif
