#include "clipboardservice.h"

#include <QClipboard>
#include <QCoreApplication>
#include <QGuiApplication>
#include <QProcess>
#include <QStandardPaths>
#include <QThread>
#include <QTimer>
#include <QDebug>
#include <QBuffer>
#include <QImage>
// QMimeData 是跨平台类：readSystemClipboard()/onClipboardChanged() 调用
// clipboard->mimeData() 的成员（hasImage/imageData）需要完整定义，
// qclipboard.h 只前置声明。必须无条件 include，否则 Windows/macOS 编译报 incomplete type。
#include <QMimeData>

namespace {
constexpr const char* kMimeText  = "text/plain";
constexpr const char* kMimeImage = "image/png";
constexpr int kCliPollMs = 1200; // CLI 模式轮询周期（X11 无剪贴板变化信号）
}

#ifdef Q_OS_LINUX
// ---------------------------------------------------------------------------
// ClipboardCliWorker：跑在 cliThread_ 里的阻塞后端
// ---------------------------------------------------------------------------

ClipboardCliWorker::ClipboardCliWorker(const QString& bin, bool imageCapable)
    : bin_(bin)
    , imageCapable_(imageCapable)
    , xclip_(bin.endsWith(QStringLiteral("xclip")))
{
}

ClipboardCliWorker::~ClipboardCliWorker()
{
    if (owner_) {
        owner_->disconnect(this);
        if (owner_->state() != QProcess::NotRunning) {
            owner_->kill();
            owner_->waitForFinished(1000);
        }
        // owner_ 是 this 的子对象，随 this 析构；
        // 但所有者是 selection owner，必须先 kill 再让 QProcess 析构，
        // 否则析构时进程仍在运行（Qt 会打印 "QProcess: Destroyed while process is still running"）。
        owner_ = nullptr;
    }
}

void ClipboardCliWorker::readImpl(QString& mime, QByteArray& data) const
{
    if (!xclip_) {
        // xsel 无 TARGETS 探测，--output 直接返回剪贴板文本
        QProcess p;
        p.start(bin_, QStringList{ "--clipboard", "--output" });
        if (!p.waitForFinished(1500))
            return;
        mime = QString::fromLatin1(kMimeText);
        data = p.readAllStandardOutput();
        return;
    }
    // 1) 探测 TARGETS：仅用于判断当前是图片还是文本
    QProcess p;
    p.start(bin_, QStringList{ "-selection", "clipboard", "-o", "-t", "TARGETS" });
    if (!p.waitForFinished(1500))
        return;
    const QByteArray targets = p.readAllStandardOutput();
    if (targets.contains("image/png") && imageCapable_) {
        // 2) 图片：按 image/png 读原始字节
        QProcess ip;
        ip.start(bin_, QStringList{ "-selection", "clipboard", "-o", "-t", "image/png" });
        if (ip.waitForFinished(2500)) {
            QByteArray png = ip.readAllStandardOutput();
            if (!png.isEmpty()) {
                mime = QString::fromLatin1(kMimeImage);
                data = png;
            }
        }
        return;
    }
    // 3) 文本：必须再读一次实际内容。-t TARGETS 的输出只是目标列表
    //    （如 "TARGETS\nUTF8_STRING\n"），不是剪贴板文本，直接回传会污染前端。
    QProcess tp;
    tp.start(bin_, QStringList{ "-selection", "clipboard", "-o" });
    if (!tp.waitForFinished(1500))
        return;
    mime = QString::fromLatin1(kMimeText);
    data = tp.readAllStandardOutput();
}

void ClipboardCliWorker::read()
{
    QString mime;
    QByteArray data;
    readImpl(mime, data);
    emit contentRead(mime, data);
}

void ClipboardCliWorker::write(const QString& mime, const QByteArray& data)
{
    const bool isImage = (mime == QLatin1String(kMimeImage));
    if (owner_) {
        owner_->disconnect(this);
        if (owner_->state() != QProcess::NotRunning) {
            owner_->kill();
            owner_->waitForFinished(1000);
        }
        owner_->deleteLater();
        owner_ = nullptr;
    }
    QProcess* owner = new QProcess(this);
    QStringList args;
    if (xclip_) {
        args << "-selection" << "clipboard" << "-i";
        if (isImage)
            args << "-t" << "image/png";
    } else {
        args << "--clipboard" << "--input";
    }
    owner->start(bin_, args);
    if (!owner->waitForStarted(2000)) {
        qWarning("ClipboardService: failed to start clipboard owner process");
        owner->deleteLater();
        emit contentWritten(false, mime, data);
        return;
    }
    owner->write(data);
    owner_ = owner;
    // xclip 读完 stdin 后可能 fork 到后台并让前台进程退出，或失去 selection
    // （其他应用复制）后自行退出；这里统一清理成员指针。
    // 注意：必须在等待之前连接，否则等待期间已发出的 finished 会丢失，指针悬空。
    connect(owner, static_cast<void(QProcess::*)(int, QProcess::ExitStatus)>(&QProcess::finished),
        this, [this, owner](int, QProcess::ExitStatus) {
            if (owner_ == owner)
                owner_ = nullptr;
            owner->deleteLater();
        });
    owner->closeWriteChannel();
    // [P2] 大数据必须等全部写入管道：管道缓冲仅 64KB，旧代码一次
    // waitForBytesWritten(1000) 只能冲刷一小部分，closeWriteChannel 后剩余
    // 字节被截断 → 远端粘贴内容缺斤短两，却恒报成功。
    const int kFlushTimeoutMs = 30000;
    int waited = 0;
    while (owner->bytesToWrite() > 0 && waited < kFlushTimeoutMs) {
        const bool progressed = owner->waitForBytesWritten(500);
        waited += 500;
        if (!progressed && owner->error() == QProcess::WriteError)
            break; // 管道写错误，继续等也不会成功
    }
    const bool flushed = (owner->bytesToWrite() == 0);
    // 确保数据已写入管道：xclip 若在读全 stdin 后 fork，数据未送达时会以空内容
    // 成为 selection owner。xsel 不 fork、常驻前台，waitForFinished 超时返回。
    owner->waitForFinished(200);
    if (!flushed)
        qWarning("ClipboardService: clipboard write truncated (%lld bytes pending)",
                 (long long)owner->bytesToWrite());
    emit contentWritten(flushed, mime, data);
}
#endif // Q_OS_LINUX

// ---------------------------------------------------------------------------
// ClipboardService
// ---------------------------------------------------------------------------

ClipboardService::ClipboardService(QObject* parent)
    : QObject(parent)
{
    debounceTimer_ = new QTimer(this);
    debounceTimer_->setSingleShot(true);
    debounceTimer_->setInterval(150);
    connect(debounceTimer_, &QTimer::timeout, this, &ClipboardService::flushPending);
}

ClipboardService::~ClipboardService()
{
#ifdef Q_OS_LINUX
    // 工作线程内的阻塞调用最长 2.5s，先停线程再让 QObject 析构，
    // 避免析构 QProcess/QTimer 时它们仍属于一个运行中的线程。
    if (cliThread_) {
        cliThread_->quit();
        if (!cliThread_->wait(5000))
            qWarning("ClipboardService: clipboard worker thread did not stop in time");
        cliThread_ = nullptr;   // QThread 是 this 的子对象，随 this 析构
        cliWorker_ = nullptr;   // 已由 finished → deleteLater 在工作线程内回收
    }
#endif
}

void ClipboardService::start()
{
    // GUI 模式：QClipboard 只能在其所属（GUI）线程访问，且读写都不阻塞，保持主线程直连。
    if (qobject_cast<QGuiApplication*>(QCoreApplication::instance())) {
        QClipboard* clipboard = QGuiApplication::clipboard();
        if (!clipboard) {
            qWarning("ClipboardService: no QGuiApplication clipboard available");
            return;
        }
        available_ = true;
        connect(clipboard, &QClipboard::dataChanged,
            this, &ClipboardService::onClipboardChanged);

        // Push current clipboard content as initial state
        QString mime;
        QByteArray data;
        readSystemClipboard(mime, data);
        if (!data.isEmpty()) {
            lastMime_ = mime;
            lastData_ = data;
            emit contentChanged(mime, data);
        }
        return;
    }
#ifdef Q_OS_LINUX
    // 服务模式（QCoreApplication）：退化为 xclip/xsel 写 X11 CLIPBOARD。
    // 服务进程的 DISPLAY/XAUTHORITY 由 detectUserX11Env 设置，GUI 会话可正常读取。
    // 阻塞式 QProcess 调用全部交给工作线程，主线程只做请求/应答。
    if (initCliBackend())
        return;
    qWarning("ClipboardService: no QGuiApplication and no usable xclip/xsel, clipboard disabled");
#else
    qWarning("ClipboardService: no QGuiApplication, clipboard disabled");
#endif
}

#ifdef Q_OS_LINUX
bool ClipboardService::initCliBackend()
{
    if (available_ || cliMode_)
        return available_;
    if (qobject_cast<QGuiApplication*>(QCoreApplication::instance()))
        return false; // GUI 模式走 QClipboard，不需要 CLI 后端
    // [compat] qEnvironmentVariableIsEmpty 是 Qt 5.10 API，Qt 5.9 用 qgetenv
    if (qgetenv("DISPLAY").isEmpty())
        return false; // 服务启动早期 DISPLAY 可能尚未设置，留给后续惰性初始化

    QString bin = QStandardPaths::findExecutable(QStringLiteral("xclip"));
    bool imageCapable = !bin.isEmpty();
    if (bin.isEmpty()) {
        bin = QStandardPaths::findExecutable(QStringLiteral("xsel"));
        // xsel 不支持 image target，仅文本
    }
    if (bin.isEmpty()) {
        qWarning("ClipboardService: no xclip/xsel found, clipboard disabled");
        return false;
    }

    cliThread_ = new QThread(this);
    cliWorker_ = new ClipboardCliWorker(bin, imageCapable);
    cliWorker_->moveToThread(cliThread_);
    connect(this, &ClipboardService::requestCliRead,
        cliWorker_, &ClipboardCliWorker::read);
    connect(this, &ClipboardService::requestCliWrite,
        cliWorker_, &ClipboardCliWorker::write);
    connect(cliWorker_, &ClipboardCliWorker::contentRead,
        this, &ClipboardService::onCliContentRead);
    connect(cliWorker_, &ClipboardCliWorker::contentWritten,
        this, &ClipboardService::contentApplied);
    connect(cliThread_, &QThread::finished, cliWorker_, &QObject::deleteLater);
    cliThread_->start();

    clipBin_ = bin;
    cliImageCapable_ = imageCapable;
    cliMode_ = true;
    available_ = true;
    qInfo() << "ClipboardService: using" << qPrintable(bin)
            << "for X11 clipboard (service mode, image:"
            << (cliImageCapable_ ? "yes" : "no") << ")";

    // CLI 模式没有剪贴板变化信号，只能轮询。定时器留在主线程：
    // 它只发一个跨线程请求，不做任何阻塞等待。
    QTimer* poll = new QTimer(this);
    connect(poll, &QTimer::timeout, this, &ClipboardService::pollClipboard);
    cliPollTimer_ = poll;
    if (suspended_)
        poll->stop(); // helper 通道活跃时不启动轮询
    else
        poll->start(kCliPollMs);
    return true;
}
#endif

// [N1] helper 通道活跃时挂起本服务的轮询与广播（由 RDPServer 调用）。
// 仅对 CLI（xclip 轮询）模式有意义；GUI 模式无轮询，标记即可。
void ClipboardService::setSuspended(bool s)
{
    if (suspended_ == s)
        return;
    suspended_ = s;
#ifdef Q_OS_LINUX
    if (cliMode_ && cliPollTimer_) {
        if (s) {
            cliPollTimer_->stop();
            qInfo() << "ClipboardService: suspended (helper channel active)";
        } else {
            cliReadInFlight_ = false;
            cliPollTimer_->start(kCliPollMs);
            qInfo() << "ClipboardService: resumed (helper channel gone)";
        }
    }
#endif
}

void ClipboardService::readSystemClipboard(QString& mime, QByteArray& data) const
{
    mime.clear();
    data.clear();
    QClipboard* clipboard = QGuiApplication::clipboard();
    if (!clipboard)
        return;
    const QMimeData* md = clipboard->mimeData();
    if (md && md->hasImage()) {
        QImage img = qvariant_cast<QImage>(md->imageData());
        if (!img.isNull()) {
            QBuffer buf;
            buf.open(QIODevice::WriteOnly);
            if (img.save(&buf, "PNG")) {
                mime = QString::fromLatin1(kMimeImage);
                data = buf.data();
                return;
            }
        }
    }
    mime = QString::fromLatin1(kMimeText);
    data = clipboard->text().toUtf8();
}

bool ClipboardService::writeSystemClipboard(const QString& mime, const QByteArray& data)
{
    QClipboard* clipboard = QGuiApplication::clipboard();
    if (!clipboard)
        return false;
    if (mime == QLatin1String(kMimeImage)) {
        QImage img;
        if (!img.loadFromData(data, "PNG") || img.isNull())
            return false;
        clipboard->setImage(img);
        return true;
    }
    clipboard->setText(QString::fromUtf8(data));
    return true;
}

void ClipboardService::onClipboardChanged()
{
    // A single copy action can fire dataChanged several times;
    // buffer the value and debounce so we only report the final content.
    QClipboard* clipboard = QGuiApplication::clipboard();
    if (!clipboard)
        return;
    const QMimeData* md = clipboard->mimeData();
    if (md && md->hasImage()) {
        QImage img = qvariant_cast<QImage>(md->imageData());
        if (!img.isNull()) {
            QBuffer buf;
            buf.open(QIODevice::WriteOnly);
            if (img.save(&buf, "PNG")) {
                pendingMime_ = QString::fromLatin1(kMimeImage);
                pendingData_ = buf.data();
                debounceTimer_->start();
                return;
            }
        }
    }
    pendingMime_ = QString::fromLatin1(kMimeText);
    pendingData_ = clipboard->text().toUtf8();
    debounceTimer_->start();
}

void ClipboardService::flushPending()
{
    if (pendingData_.isEmpty() || (pendingMime_ == lastMime_ && pendingData_ == lastData_))
        return;
    lastMime_ = pendingMime_;
    lastData_ = pendingData_;
    emit contentChanged(pendingMime_, pendingData_);
}

// 实现必须全平台存在：pollClipboard() 声明在 private slots 区（无条件），
// moc 会为所有平台生成 qt_static_metacall 调用代码；若实现被 #ifdef Q_OS_LINUX
// 包住，Windows/macOS 链接期报 undefined reference。函数体内部条件编译即可——
// 非 Linux 平台为空函数且永不触发（轮询定时器仅在 Linux CLI 模式启动）。
void ClipboardService::pollClipboard()
{
#ifdef Q_OS_LINUX
    if (!cliMode_ || cliReadInFlight_)
        return;
    // 只发起请求：阻塞读取在工作线程里，主线程立刻返回
    cliReadInFlight_ = true;
    emit requestCliRead();
#endif
}

#ifdef Q_OS_LINUX
void ClipboardService::onCliContentRead(const QString& mime, const QByteArray& data)
{
    cliReadInFlight_ = false;
    // 有客户端在读取期间接入 → 补发一次应答，别让它等下一次轮询
    if (cliPushPending_) {
        cliPushPending_ = false;
        emit contentReady(mime, data);
    }
    if (data.isEmpty() || (mime == lastMime_ && data == lastData_))
        return;
    lastMime_ = mime;
    lastData_ = data;
    emit contentChanged(mime, data);
}
#endif

void ClipboardService::requestContent()
{
#ifdef Q_OS_LINUX
    if (!available_ && !qobject_cast<QGuiApplication*>(QCoreApplication::instance()))
        initCliBackend();
#endif
    if (!available_) {
        // 必须应答：调用方（新客户端接入）在等这个信号发初始剪贴板
        emit contentReady(QString(), QByteArray());
        return;
    }
#ifdef Q_OS_LINUX
    if (cliMode_) {
        cliPushPending_ = true;
        if (cliReadInFlight_)
            return; // 结果会由 onCliContentRead 补发
        cliReadInFlight_ = true;
        emit requestCliRead();
        return;
    }
#endif
    QString mime;
    QByteArray data;
    readSystemClipboard(mime, data);
    emit contentReady(mime, data);
}

void ClipboardService::requestSetContent(const QString& mime, const QByteArray& data)
{
#ifdef Q_OS_LINUX
    if (!available_ && !qobject_cast<QGuiApplication*>(QCoreApplication::instance()))
        initCliBackend();
#endif
    if (!available_ || data.isEmpty()) {
        emit contentApplied(false, mime, data);
        return;
    }
    const bool isImage = (mime == QLatin1String(kMimeImage));
    if (isImage) {
#ifdef Q_OS_LINUX
        if (cliMode_ && !cliImageCapable_) {
            qWarning("ClipboardService: image clipboard requires xclip (xsel unsupported)");
            emit contentApplied(false, mime, data);
            return;
        }
#else
        // 非 Linux 平台暂不支持图片剪贴板写入（Windows helper 走文本通道）
        emit contentApplied(false, mime, data);
        return;
#endif
    }
    // 先更新指纹再写入，写入触发的 dataChanged/轮询会被去重抑制，避免回声
    lastMime_ = mime;
    lastData_ = data;
    pendingData_.clear();
#ifdef Q_OS_LINUX
    if (cliMode_) {
        // 阻塞写入在工作线程执行，完成后经 contentWritten → contentApplied 应答
        emit requestCliWrite(mime, data);
        return;
    }
#endif
    emit contentApplied(writeSystemClipboard(mime, data), mime, data);
}