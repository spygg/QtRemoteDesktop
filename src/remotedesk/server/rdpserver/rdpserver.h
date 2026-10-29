// server/rdp_server.h
#ifndef RDP_SERVER_H
#define RDP_SERVER_H

#include <QElapsedTimer>
#include <QFile>
#include <QHostAddress>
#include <QHash>
#include <QImage>
#include <QJsonObject>
#include <QJsonArray>
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
#include <chrono>
#include <memory>
#include <thread>
#include <vector>
#include "codecdefs.h" // CodecType / HwEncodeMode（不依赖 FFmpeg，JPEG 回退构建可用）

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

// 时长/冷却判据统一用单调时钟（steady_clock）：编码器重建的冷却、IDR 请求超时、
// PLI 复用间隔、解锁会话的有效期都是"经过多久"的语义。墙钟会被 NTP 校时或用户改
// 系统时间拉动，差值突变——向前跳会误判冷却已过而连续重建，向后跳则让兜底永不触发。
// 仅用于内部时长判断，码流时间戳与日志时间仍是墙钟。
// +1 保证返回值恒 > 0（0 保留给"未设置"）。
inline qint64 monoNowMs()
{
    static const auto origin = std::chrono::steady_clock::now();
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - origin).count() + 1;
}

// HTTP 请求解析状态：将请求头与 POST body 在多次 readyRead 之间累积，
// 避免在主线程用 waitForReadyRead 同步阻塞等待 TCP 分片的 body 到齐。
struct HttpParseState {
    bool headerDone = false;     // 请求头是否已解析（含 Content-Length）
    bool dispatched = false;     // 请求是否已交给处理器（防止同一 socket 重复派发）
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
    // 采集启动中（Linux Wayland 初始化会在主线程跑最长 5s 的嵌套事件循环，
    // 期间巡检/切分辨率等操作若重入会析构正在 start() 的采集器）。
    bool isCaptureStarting() const { return captureStarting_; }
    bool isCaptureSourceConnected() const;
    bool isScreenLocked() const { return screenLocked_; }

    // [A8] /shell（终端页）的画质/帧率/缩放/分辨率下拉框此前把消息发给了终端
    // WebSocket，服务端只当终端输入，配置被静默丢弃。这个入口让 HTTP 侧走与
    // 浏览器 WS 完全相同的处理路径。
    // clientId 传空串是安全的：所有 sendJson 都按 clientId 查表，查不到即 no-op，
    // 而 broadcastJson 照常把结果发给所有在线页面。
    void applyConfigFromHttp(const QJsonObject& cfg);

    // [A17] 剪贴板轮询的挂起状态由"helper 通道活跃"与"没有观众"两个条件合成：
    // X11 CLI 后端每秒 fork 两次 xclip（实测 20s 内 33 次 execve ≈14 万进程/天），
    // 无人观看时纯属白烧。
    void updateClipboardSuspend();
    bool clipSuspendedByHelper_ = false;

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
    // IME 工作线程结束（主线程）：回收 std::thread，否则 joinable 状态会让
    // 后续所有 ime_switch 请求被 "busy" 永久拒绝。
    void onImeWorkerFinished();
#ifdef USE_WEBRTC
    void onWebRtcMessage(const QString& clientId, const QJsonObject& msg);
#endif

signals:
    void requestFileList(const QString& clientId, const QString& path);
    void requestDownload(const QString& clientId, const QString& path);
    void requestUploadStart(const QString& clientId, const QString& path, qint64 size);
    void requestUploadDone(const QString& clientId, const QString& path);
    // 观看模式过滤后的上传数据块中继（WS 二进制块 → 文件服务，跨线程 queued）
    void fileChunkFromClient(const QString& clientId, const QString& path, const QByteArray& data);
    // IME 工作线程 → 主线程结果回传（auto=跨线程 queued 投递）
    void imeResultReady(const QString& clientId, const QJsonObject& state);
    // IME 工作线程退出通知（所有返回路径都发，主线程据此 join + 复位）
    void imeWorkerFinished();

private:
    // 返回 false = HTTP 端口绑定失败（致命，initialize 应中止启动）
    bool setupHttpServer();
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
    // [N6] 退役编码器墓地：编码线程卡死在驱动调用（shutdownRetire 失败）时，
    // 对象断开全部信号后**故意泄漏**于此，绝不析构（析构路径 qFatal 会连坐服务）。
    // 线程真正退出后由 reapRetiredEncoders() 安全回收，避免反复卡死无限累积。
    std::vector<std::unique_ptr<VideoEncoder>> retiredEncoders_;
    // 缓存最近一帧关键帧，用于新客户端接入时立即回放以引导出图
    // （静态桌面抓取去重不会主动出帧，否则新客户端永远等不到关键帧而黑屏）。
    QByteArray lastKeyframeData_;
    qint64 lastKeyframeTs_ = 0;
    // [A2] 出站视频时间戳的单调基准 + 缓存 IDR 的落库时刻。
    // 缓存 IDR 回放只能沿用它的原始 ts（属于过去），实测让客户端帧时间线倒退
    // 422~3563ms；helper 重启后编码器 ts 从 0 重新计时同样会倒退。两者都统一在
    // onEncodedFrame 出口抬到"上一帧 +1"之上（见 replayCachedKeyframe 注释）。
    qint64 lastSentVideoTs_ = 0;
    qint64 lastKeyframeWallMs_ = 0;
    // 把缓存 IDR 回放给单个客户端，返回是否真的发了。
    // maxAgeMs > 0 时超过该时长就不再发：静止桌面的缓存帧内容仍然正确，但新客户端
    // 宁可等一次 pumpKeyframe（约一个 RTT）拿当前帧，也不先铺一帧两秒前的画面
    // 再立刻重绘，还省掉一次几十 KB 的重复关键帧。
    bool replayCachedKeyframe(const QString& clientId, qint64 maxAgeMs);
    // [A15] 作废"编码器相关的进程内缓存"（关键帧 / SPS-PPS / 复用基准）。
    // 服务模式下编码器在 helper 进程里：本进程拿不到"内容变了没"的信号，缓存的
    // IDR/SPS 一旦与 helper 的新编码器不一致，新接入的浏览器就会先收到一条
    // 尺寸/参数集都不匹配的旧码流（花屏），直到 helper 的新 IDR 到达。
    void invalidateCachedIdr();
    // [A9] 内部画质码率（80/60/35/15）→ 档位名（high/medium/low/verylow）。
    // 下发给前端与下发给 helper 必须同一口径，否则前端下拉框回落显示"中"。
    QString qualityName() const;
    // 编码器尺寸不匹配触发的重建节流时间戳（单调时钟毫秒，避免每帧都尝试重建阻塞主线程）
    qint64 lastSizeReinitMs_ = 0;
    // [PLI-REUSE] 静止桌面下的关键帧请求（PLI）优化状态。
    // 背景：浏览器在"长时间收不到可解码帧"时会周期性发 PLI 索要一帧 IDR。旧实现用
    // forceNextFrame() 响应，但非 requestNewIdr 分支产出的其实是 **P 帧**，浏览器
    // 不满足 → 每 ~3s 再要一次 → 静止桌面下形成
    //   「无帧 → PLI → 全屏抓屏 + 编码 → 仍非 IDR → 再 PLI」
    // 的永久循环。这正是静止时视频模式 CPU 远高于图片模式（后者 0 帧 0 编码）的
    // 唯一来源，也是本文件里 CAP-EMIT force frame 每 ~3.06s 刷一条的原因。
    //
    // 修复分两层：
    //  ① 语义层：PLI 必须用 IDR 响应，否则循环不会停。
    //  ② 成本层：静止画面下不重新抓屏/编码，直接重发缓存 IDR——sendFrame 内部按
    //     rtpClock_.elapsed() 取当前时间戳（不会回退，Chrome 不会当重复帧丢弃），
    //     而 IDR 自包含，解码不需要参考帧。
    //
    // 复用安全性判据 = 采集器的 contentChangeCount()（"桌面内容真实变化"计数）：
    //   缓存 IDR 时记下其值 staticIdrEpoch_；PLI 到来时若仍相等 ⇒ 自该 IDR 起桌面
    //   逐像素未变 ⇒ 重发缓存 IDR 与"重新抓屏编码出的 IDR"等价。
    //   注意**不能**用"自缓存以来编码帧数 == 0"作判据：openh264 有 1 帧流水线延迟，
    //   重建编码器逼 IDR 时必然多吐 1 个 P 帧（同画面），该判据永不成立（实测 =1）。
    quint64 staticIdrEpoch_ = 0;
    bool staticIdrEpochValid_ = false;
    // 上次 PLI 复用的时刻（单调时钟毫秒，见 rdpserver.cpp monoNowMs()；冷却防止 PLI 风暴下重复重发）。
    qint64 lastPliReuseMs_ = 0;
    // IDR 请求待命时刻（单调时钟毫秒，0=无请求）。requestKeyframe() 只设标志，部分软编（openh264）
    // 不认 flush + pict_type=I 仍输出 P 帧，需要超时兜底重建编码器才能真的产出 IDR。
    qint64 idrRequestMs_ = 0;
    QTimer* idrFallbackTimer_ = nullptr;
    // PLI 处理计数（诊断：复用缓存 IDR vs 重新出帧）。
    quint64 pliReuseCount_ = 0;
    quint64 pliEncodeCount_ = 0;
#endif
    // [A3] 每个客户端的"显式关键帧请求"时刻（单调时钟毫秒）。
    // 前端在网络抖动/缓冲溢出/配额满时会每 500ms 重试一轮，无冷却时每次都触发
    // helper 重建编码器（XP 实测 ≈0.25 核·秒）。一个卡住的客户端就能把单核机
    // 从 28% 拉到 55%，两个叠加可到 42 次重建/20s，机器被长期锁在"半载 + 卡帧"。
    QHash<QString, qint64> kfRequestMsByClient_;
    quint64 kfCooldownSkips_ = 0;

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

    // [B8] IME 切换工作线程（原先 detach 后无法回收）：由成员持有生命周期，
    // 析构时 join。同一时刻只允许一个在飞（切换请求极低频）。
    std::unique_ptr<std::thread> imeWorker_;

    bool isRunning_ = false;
    quint16 httpPort_;
    quint16 wsPort_;

    QRect screenGeometry_;
    QImage lastCapturedFrame_;
    QPoint lastCursorPos_ { -1, -1 };
    qint64 lastCursorQueryMs_ = 0;
    int videoBaseBitrate_ = 0; // 视频模式目标码率（过载降质后用于恢复）
    QJsonObject lastScreenInfo_;
    // 服务模式的多屏输出快照：服务进程在 Session 0 枚举不到用户会话的显示器，
    // 由 helper 的 capture_outputs 消息喂进来，新客户端接入时随 server_config 下发。
    QJsonArray lastCaptureOutputs_;
    int lastCaptureOutputIdx_ = 0;

    bool useSsl_ = true; // 默认启用 HTTPS/WSS：WebRTC 需安全上下文；无 OpenSSL 时自动降级
    bool serviceMode_ = false;
    // 服务模式：helper 上报的视频能力（capture_caps）。captureVideoCapable_ 为 true
    // 表示 helper 侧可编 H.264，服务端据此允许进入视频模式（编码帧由 helper 上报）。
    bool captureVideoCapable_ = false;
    bool captureHwEncode_ = false;
    QString captureCodecName_ = QStringLiteral("h264");
    int configFps_ = 30;
    int configQuality_ = 60;
    int configScale_ = 75;
    // [PACE-FACTOR] 慢读回机器（VMware SVGA 无 3D 加速）的采集节流倍数：
    // 采集间隔 = 实测单帧耗时 × 该值，因此占空比 ≈ 1/(1+factor)。
    // 2 ⇒ 33%（默认，兼顾帧率）；4 ⇒ 20%；9 ⇒ 10%（拿帧率换 CPU）。
    // 只在读屏慢到触发 pace 的机器上生效，快机恒为全速。
    double configPaceFactor_ = 2.0;
    // [ADAPT-CAP] helper 的慢机自适应结果（运行期上限，不落盘）。0 = 未自适应。
    // 只作为"上限"参与：用户/画质档位给出的 fps/scale 都要被它钳一次，否则一次
    // 模式切换（前端重发 config 或画质档位从 userScale_ 重算 configScale_）就会把
    // 自适应结果冲回默认值，机器重新掉进 1.2fps/43% CPU 的坑；下一次也不会再降
    // （helper 的 adaptDecided 已置真）。重启后从用户配置重新评估，机器条件变化能自愈。
    int adaptFpsCap_ = 0;
    int adaptScaleCap_ = 0;
    double adaptPaceFactor_ = 0.0;
    // 应用自适应上限（0/未设置时原值返回）
    int cappedFps(int fps) const { return adaptFpsCap_ > 0 ? qMin(fps, adaptFpsCap_) : fps; }
    int cappedScale(int scale) const { return adaptScaleCap_ > 0 ? qMin(scale, adaptScaleCap_) : scale; }
    double cappedPaceFactor() const { return qMax(configPaceFactor_, adaptPaceFactor_); }
    CodecType configCodec_ = CodecType::H264;        // 编码协议（h264/hevc/vp8/vp9/av1）
    // WebRTC 固定 H.264，进入会话时临时改写 configCodec_（编码器只认这个成员），
    // 用户原本的选择单独保存：落盘写回原值，离开视频模式时恢复。
    CodecType codecBeforeWebRtc_ = CodecType::H264;
    bool codecOverriddenForWebRtc_ = false;
    HwEncodeMode configHwEncodeMode_ = HwEncodeMode::Auto; // 硬件编码开关（auto/on/off）
    int userScale_ = 75;
    bool screenLocked_ = false;
    // helper 上报的锁屏输入桌面名（screen_locked 的 desktop 字段，如 "Winlogon"、
    // "Welcome"）。服务进程自己 OpenInputDesktop 会失败，只能靠它把 secure-input
    // 投到登录 UI 真正所在的桌面。空 = 不知道，回退 winsta0\Winlogon。
    QString lockedDesktop_;
    // helper 上报的锁屏种类（screen_locked 的 kind）："screensaver" = 屏保恢复锁屏
    // （用户会话还活着，输入桌面在 Screen-saver）、"logon" = 登录界面锁屏（欢迎屏幕/
    // 切换用户，桌面已是 Winlogon）、"unknown" = 连输入桌面都打不开。XP 的这两种能给的
    // 操作不一样，解锁失败时要用它说清是哪一种，而不是笼统一句"密码不对"。
    QString lockedKind_;
    // helper 随锁屏一起上报的提示文案（"屏保恢复锁屏…"/"登录界面锁屏…"）。新客户端在
    // 已锁状态接入时直接回放它，避免服务端自己再写一句通用文案 —— 实测 XP 上那句
    // "点「解锁」由服务端注入密码"是错的，两处各写一份就会有一处忘改。
    QString lockedHint_;
    // [Win-解锁] 一键免密解锁用的明文密码（server_config.json 的 unlockPassword，
    // 局域网自用场景）。锁屏 UI 在 winlogon 安全桌面，只能经 secure-input 注入。
    QString unlockPassword_;
    // 前端页面标题的自定义系统名称（server_config.json 的 systemName，如
    // "办公室-242"）。空则回退主机名。多台被控机靠它区分标签页。
    QString systemName_;
    bool secureInputRunning_ = false;
    // secure-input 进程实际投放到的桌面名（不含 "winsta0\" 前缀）。锁屏期间桌面会变
    // （XP：屏保桌面 → 登录桌面），名字与当前锁屏桌面不符就要重投进程。
    QString secureInputDesktop_;
    // 解锁序列的有效期（单调时钟毫秒，见本文件 monoNowMs()）。序列里 SAS 会把输入
    // 桌面从 Screen-saver 切到 Winlogon —— 这是流程自己造成的正常切换，此时"桌面不符"
    // 不能算进程不健康。
    // 不加这个门禁的实测后果：helper 在 SAS 后 0.9s 补报 screen_locked=true，
    // 健康判定据此杀掉正在写密码的进程，第一次尝试全废，XP 解锁从 ~2s 拖到 ~9s。
    // 用到期时间而非 bool：终判定时器若因 helper 掉线没跑，也不会把门禁永久留着。
    qint64 secureInputUnlockUntilMs_ = 0;
    bool captureAvailable_ = true;
    // 采集启动重入保护：startCapture() 全程置位（Wayland 初始化/切换视频模式
    // 都在其中），期间巡检定时器与 set_resolution 必须跳过，否则会释放/重启
    // 正在执行 start() 的采集器（use-after-free）。
    bool captureStarting_ = false;
    QString shellCurrentDir_;
    QMap<QString, QString> shellCwdByToken_; // [B9] 会话令牌 → shell CWD
    // 在途 shell exec 进程数上限：每个请求常驻一个 QProcess + 一个 HTTP socket
    // （最长 5 分钟）。无上限时脚本/误操作可同时拉起任意多进程吃光句柄与内存。
    int shellExecActive_ = 0;
    // [B-1] 客户端 → 按下的键集合（code → 最近 keydown 载荷，保留 keycode 供
    // Windows VK 回放）。断开时逐个补发 keyup，否则按键永久卡死（无限自动
    // 重复 / 修饰键卡住全部变组合键）。
    QMap<QString, QHash<QString, QJsonObject>> pressedKeysByClient_;
    // 观看模式（只读）客户端：只推送画面，任何"能操作被控端"的消息在
    // onInputReceived 入口丢弃。按连接记录，断线时清理（clientId 可能被复用）。
    QSet<QString> viewOnlyClients_;

    // 每个 HTTP 连接的请求解析状态（跨 readyRead 累积，避免主线程阻塞）
    QHash<QTcpSocket*, HttpParseState> httpParseState_;
    // 每个 socket 的直链下载传输状态（streaming 写出，写完自动清理）。
    // 存指针而非值：QHash 的插入/扩容都要求值可拷贝，而状态里持有独占文件句柄。
    QHash<QTcpSocket*, HttpDownloadState*> httpDownloadStates_;
#ifdef _WIN32
    int secureInputPid_ = 0;
    // 保留进程句柄（而非仅 PID）用于超时终止：PID 在进程退出后可被系统回收，
    // 5s 超时后按 PID 重新 OpenProcess 有极小概率杀掉一个无关的新进程。
    void* secureInputProcHandle_ = nullptr;
    // 启动代次：5s 超时定时器只在代次未变时生效。否则旧进程的定时器会
    // 误杀 stop 之后新拉起的进程（旧实现没有可取消的定时器句柄）。
    quint64 secureInputGen_ = 0;
#endif
    QSslConfiguration* sslConfiguration_;
    QByteArray lastCodecExtra_;

    enum class ServerMode { Video,
        Image };
    ServerMode currentMode_ = ServerMode::Video;

    void switchToImageMode();
    bool switchToVideoMode();
    void reinitVideoEncoderForScale();
    // 退役当前编码器并重建（reinitVideoEncoderForScale 的通用实现）。
    // onlyIfIdle=true 时先做原子空闲判定：有在飞帧则不做任何改动、返回 Deferred，
    // 由调用方稍后重试（IDR 兜底定时器用，避免主线程白等一整帧）。
    // NotApplicable 表示当前没有可重建的编码器/捕获源（如服务模式画面在 helper），
    // 与 Deferred 必须区分：前者重试永远不会成功，调用方应放弃该请求而不是空转。
    enum class RebuildResult { Done,
        Deferred,
        NotApplicable };
    RebuildResult rebuildVideoEncoder(bool onlyIfIdle);
    bool hwEncodeAvailable() const;
    // 服务端能否进入视频模式（H.264 编码）：
    //  ① 本地原始帧源可用（直连 / Linux 服务模式进程内捕获）→ 支持；
    //  ② 服务模式下画面在 helper 进程，若 helper 已连且声明具备视频能力 → 支持
    //     （编码在 helper 侧完成，编码帧经 /capture 上报）。
    bool videoModeSupported() const;
    // 服务模式远端视频源是否可用：helper 已连且声明支持视频（capture_caps.video=true）
    bool remoteVideoSourceAvailable() const;
    // 服务模式：向 helper 下发视频参数（codec/hw_encode/fps/scale/quality）与
    // capture_control 动作（video_on/video_off）。helper 据此进入/退出 H.264 编码。
    void sendVideoParamsToCaptureSource(const QString& action);
    // 创建 VideoEncoder 并接好全部信号（此前两处创建点接线不一致，
    // 前台模式那处漏了 encoderReady/encoderOverload/reinitRequired）
    void createVideoEncoder();
#ifdef USE_FFMPEG
    VideoEncoder* ensureVideoEncoder();
    // 回收墓地中线程已退出的退役编码器（见 retiredEncoders_）
    void reapRetiredEncoders();
#endif

    void loadServerConfig(const QString& configPath);
    void saveServerConfig(const QString& configPath);
    // 页面标题实际使用的名字：systemName_ 非空则用它，否则主机名，再退"远程桌面"。
    // 前端首次加载 / 改完名字后的 ack 都用它，避免"输入框显示 A、标签页显示 B"。
    QString effectiveSystemName() const;
    // 把当前 systemName 广播给所有已连接页面（改一台上百个标签页一起变）
    void broadcastSystemName();
    // index.html 的烤制缓存（标题/WS 端口都写进去了）。改名后必须失效，
    // 否则刷新页面还是旧标题 —— 缓存和失效放在同一把锁里，HTTP 线程安全。
    mutable QMutex htmlCacheMutex_;
    mutable QByteArray htmlCache_;
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
    void handleSystemAction(const QString& action, const QString& clientId,
                            const QString& password = QString());

    // ---- 输入法切换 ----
    // ibus panel 的 hotkey grab（如 Super+space）对 XTEST 注入的合成按键不响应，
    // 远程桌面里无法用注入快捷键触发切换；改为服务端直接调用 `ibus engine <next>`
    // 循环切换 preload-engines 列表（ibus CLI 走自身 socket 地址文件，不依赖 X grab）。
    void handleImeCycle(const QString& clientId);

    // ---- 观看模式（只读） ----
    // 切换某连接的观看模式：开启后该连接只能看画面，鼠标/键盘/剪贴板写入/
    // 系统操作/输入法切换一律丢弃，画质/帧率/分辨率等纯显示设置不受影响。
    // 判定放在服务端（而非只靠前端 send()）是为了防止改过的客户端或其它注入路径绕过。
    void setClientViewOnly(const QString& clientId, bool enabled);
    // 释放某客户端已按下的所有键：断线，以及进入观看模式（此后的 keyup 会被拦掉）。
    void releasePressedKeys(const QString& clientId);

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
    // 编码器重建冷却时间戳（单调时钟毫秒，见 rdpserver.cpp monoNowMs()）：
    // pumpKeyframe 检测到无 IDR 时重建编码器，
    // 重建本身有线程开销且部分平台 MPP 库重复 init 会崩，需冷却后再重建。
    qint64 lastReinitMs_ = 0;

    // 强制出一帧关键帧：静态桌面抓取去重不会主动出帧，新客户端（FFmpeg/WebRTC）
    // 接入时需要一帧关键帧来引导出图，否则视频流永远起不来。
    // requestNewIdr=true 时（显式 request_keyframe）即使编码器已产过 IDR 也强制
    // 重新产一帧，用于客户端刚重置等帧门（WS videoStarted_）的场景。
    // allowStaticReuse=true 时（仅浏览器 PLI）允许在"桌面静止且编码器最后输出就是
    // 关键帧"的情况下直接重发缓存 IDR，跳过全屏抓屏 + H.264 IDR 编码（[PLI-REUSE]）。
    void pumpKeyframe(bool requestNewIdr = false, bool allowStaticReuse = false);

public:
    static QStringList getLocalIpAddr();
};

#endif
