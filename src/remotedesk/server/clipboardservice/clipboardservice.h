#ifndef CLIPBOARD_SERVICE_H
#define CLIPBOARD_SERVICE_H

#include <QObject>
#include <QString>
#include <QByteArray>

class QTimer;

#ifdef Q_OS_LINUX
class QProcess;
class QThread;

// xclip/xsel 后端必须用阻塞式 QProcess::waitForFinished 读写（单次最长 2.5s），
// 而且"常驻 selection owner"进程只能在固定线程里 start/kill。这些阻塞操作原先跑在
// 主线程：轮询定时器每 1.2s 触发一次，等于主线程事件循环被持续占用（画面/输入卡顿）。
// 因此把 CLI 后端整体放进独立工作线程，主线程只发请求、收结果。
//
// 本类只在工作线程内创建与使用（QProcess 不能跨线程操作），故不设父对象、
// 由 QThread::finished → deleteLater 回收。
class ClipboardCliWorker : public QObject {
    Q_OBJECT

public:
    ClipboardCliWorker(const QString& bin, bool imageCapable);
    ~ClipboardCliWorker();

public slots:
    void read();                                            // 阻塞读取，结果经 contentRead 返回
    void write(const QString& mime, const QByteArray& data); // 阻塞写入，结果经 contentWritten 返回

signals:
    void contentRead(const QString& mime, const QByteArray& data);
    void contentWritten(bool ok, const QString& mime, const QByteArray& data);

private:
    void readImpl(QString& mime, QByteArray& data) const;

    QString bin_;
    bool imageCapable_ = false; // xclip 支持 image/png，xsel 不支持
    bool xclip_ = false;
    // 常驻的 selection owner：必须存活才能向其他 X 客户端提供数据
    // （局部 QProcess 析构会 kill 子进程 → owner 消失 → 远端粘贴为空）
    QProcess* owner_ = nullptr;
};
#endif

// 共享剪贴板服务：支持文本（text/plain）与图片（image/png）两种内容类型。
// - GUI 模式（QGuiApplication）：QClipboard 原生读写，dataChanged 信号驱动远端→客户端同步，
//   全部在主线程完成（QClipboard 仅限 GUI 线程访问，且不阻塞）。
// - CLI 模式（QCoreApplication + xclip）：xclip/xsel 读写 X11 CLIPBOARD，无变化信号，
//   用轮询定时器驱动同步；所有阻塞式 QProcess 调用都交给工作线程执行。
//
// 对外接口一律异步：调用 requestXxx() 发起请求，结果通过 contentReady/contentApplied 返回。
// 两种模式下均用 AutoConnection——GUI 模式同线程（同步应答，语义与旧实现一致），
// CLI 模式跨线程（队列投递，主线程不被阻塞）。
class ClipboardService : public QObject {
    Q_OBJECT

public:
    explicit ClipboardService(QObject* parent = nullptr);
    ~ClipboardService();

    void start();

    // 请求读取当前系统剪贴板内容，结果经 contentReady 返回（无论有无内容都会应答）。
    void requestContent();
    // 请求把客户端内容写入系统剪贴板，结果经 contentApplied 返回（ok 表示是否真正写入）。
    void requestSetContent(const QString& mime, const QByteArray& data);

    bool isAvailable() const { return available_; }

signals:
    // 剪贴板内容发生变化（系统复制或客户端写入）→ 由上层广播给所有客户端
    void contentChanged(const QString& mime, const QByteArray& data);
    // requestContent() 的应答
    void contentReady(const QString& mime, const QByteArray& data);
    // requestSetContent() 的应答
    void contentApplied(bool ok, const QString& mime, const QByteArray& data);

#ifdef Q_OS_LINUX
    // 内部：主线程 → 工作线程的读取/写入请求（不对外暴露）
    void requestCliRead();
    void requestCliWrite(const QString& mime, const QByteArray& data);
#endif

private slots:
    void onClipboardChanged();   // GUI 模式：QClipboard::dataChanged
    void flushPending();         // 去抖到期 → 去重后广播
    void pollClipboard();        // CLI 模式：轮询 X11 CLIPBOARD（只发请求，不阻塞）
#ifdef Q_OS_LINUX
    void onCliContentRead(const QString& mime, const QByteArray& data); // 工作线程读取结果
#endif

private:
    bool available_ = false;
    QString lastMime_;          // 上次已推送/已写入的内容指纹，用于去重，避免回声循环
    QByteArray lastData_;
    QString pendingMime_;       // 待广播内容（去抖缓冲）
    QByteArray pendingData_;
    QTimer* debounceTimer_ = nullptr;

    // GUI 模式（QClipboard）读写，均不阻塞
    void readSystemClipboard(QString& mime, QByteArray& data) const;
    bool writeSystemClipboard(const QString& mime, const QByteArray& data);

#ifdef Q_OS_LINUX
    bool cliMode_ = false;      // 无 QGuiApplication 时用 xclip/xsel 走 X11 CLIPBOARD
    bool cliImageCapable_ = false; // xclip 支持图片 target（xsel 不支持）
    QString clipBin_;           // xclip 或 xsel 的绝对路径
    QThread* cliThread_ = nullptr;
    ClipboardCliWorker* cliWorker_ = nullptr;
    bool cliReadInFlight_ = false;   // 上一轮读取尚未返回 → 不再重复发起
    bool cliPushPending_ = false;    // 读取进行中又有客户端接入，需要补发一次 contentReady

    // 探测 xclip/xsel 并启动工作线程；已初始化/不可用返回 false
    bool initCliBackend();
#endif
};

#endif // CLIPBOARD_SERVICE_H