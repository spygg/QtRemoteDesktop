#include "clipboardservice.h"

#include <QClipboard>
#include <QCoreApplication>
#include <QGuiApplication>
#include <QProcess>
#include <QStandardPaths>
#include <QTimer>
#include <QDebug>
#include <QBuffer>
#include <QImage>

#ifdef Q_OS_LINUX
#include <QMimeData>
#endif

namespace {
constexpr const char* kMimeText  = "text/plain";
constexpr const char* kMimeImage = "image/png";
constexpr int kCliPollMs = 1200; // CLI 模式轮询周期（X11 无剪贴板变化信号）
}

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
    if (cliOwner_) {
        cliOwner_->disconnect(this);
        if (cliOwner_->state() != QProcess::NotRunning) {
            cliOwner_->kill();
            cliOwner_->waitForFinished(1000);
        }
        cliOwner_ = nullptr;
    }
#endif
}

void ClipboardService::start()
{
    // No QGuiApplication (e.g. Windows service runs as QCoreApplication only):
    // QGuiApplication::clipboard() would return an invalid pointer. Bail out.
    if (!qobject_cast<QGuiApplication*>(QCoreApplication::instance())) {
#ifdef Q_OS_LINUX
        // Linux 服务模式（QCoreApplication）：退化为 xclip/xsel 写 X11 CLIPBOARD。
        // 服务进程的 DISPLAY/XAUTHORITY 由 detectUserX11Env 设置，GUI 会话可正常读取。
        if (!qEnvironmentVariableIsEmpty("DISPLAY")) {
            QString bin = QStandardPaths::findExecutable(QStringLiteral("xclip"));
            if (!bin.isEmpty()) {
                cliMode_ = true;
                cliImageCapable_ = true;
            } else {
                bin = QStandardPaths::findExecutable(QStringLiteral("xsel"));
                cliMode_ = !bin.isEmpty();
                // xsel 不支持 image target，仅文本
            }
            if (cliMode_) {
                clipBin_ = bin;
                available_ = true;
                qInfo() << "ClipboardService: using" << qPrintable(bin)
                        << "for X11 clipboard (service mode, image:"
                        << (cliImageCapable_ ? "yes" : "no") << ")";
                QTimer* poll = new QTimer(this);
                connect(poll, &QTimer::timeout, this, &ClipboardService::pollClipboard);
                poll->start(kCliPollMs);
                return;
            }
            qWarning("ClipboardService: no xclip/xsel found, clipboard disabled");
        }
#endif
        qWarning("ClipboardService: no QGuiApplication, clipboard disabled");
        return;
    }
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
    content(mime, data);
    if (!data.isEmpty()) {
        lastMime_ = mime;
        lastData_ = data;
        emit contentChanged(mime, data);
    }
}


#ifdef Q_OS_LINUX
void ClipboardService::ensureCliMode()
{
    // 服务模式（QCoreApplication）启动时 DISPLAY 尚未被 detectUserX11Env 设置，
    // start() 无法启用 CLI 后端；此处惰性初始化：首次客户端粘贴时 DISPLAY 已就绪
    // （capture 正常运行），探测 xclip/xsel 并启用 X11 CLIPBOARD 写入。
    if (available_ || cliMode_)
        return;
    if (qobject_cast<QGuiApplication*>(QCoreApplication::instance()))
        return; // GUI 模式走 QClipboard，不需要 CLI 后端
    if (qEnvironmentVariableIsEmpty("DISPLAY"))
        return;
    QString bin = QStandardPaths::findExecutable(QStringLiteral("xclip"));
    if (!bin.isEmpty()) {
        cliImageCapable_ = true;
    } else {
        bin = QStandardPaths::findExecutable(QStringLiteral("xsel"));
    }
    if (bin.isEmpty()) {
        qWarning("ClipboardService: no xclip/xsel found, clipboard disabled");
        return;
    }
    cliMode_ = true;
    clipBin_ = bin;
    available_ = true;
    qInfo() << "ClipboardService: using" << qPrintable(bin)
            << "for X11 clipboard (lazy init, DISPLAY ="
            << qgetenv("DISPLAY").constData() << ")";
    // 惰性启用后同样要起轮询：CLI 模式没有剪贴板变化信号，
    // 否则此时才探测到 DISPLAY 的场景（service 模式启动时 DISPLAY 为空）
    // 无法把远端剪贴板变化上报给浏览器。
    QTimer* poll = new QTimer(this);
    connect(poll, &QTimer::timeout, this, &ClipboardService::pollClipboard);
    poll->start(kCliPollMs);
}

void ClipboardService::readCliContent(QString& mime, QByteArray& data) const
{
    const bool isXclip = clipBin_.endsWith(QStringLiteral("xclip"));
    if (!isXclip) {
        // xsel 无 TARGETS 探测，--output 直接返回剪贴板文本
        QProcess p;
        p.start(clipBin_, QStringList{ "--clipboard", "--output" });
        if (!p.waitForFinished(1500))
            return;
        mime = QString::fromLatin1(kMimeText);
        data = p.readAllStandardOutput();
        return;
    }
    // 1) 探测 TARGETS：仅用于判断当前是图片还是文本
    QProcess p;
    p.start(clipBin_, QStringList{ "-selection", "clipboard", "-o", "-t", "TARGETS" });
    if (!p.waitForFinished(1500))
        return;
    const QByteArray targets = p.readAllStandardOutput();
    if (targets.contains("image/png") && cliImageCapable_) {
        // 2) 图片：按 image/png 读原始字节
        QProcess ip;
        ip.start(clipBin_, QStringList{ "-selection", "clipboard", "-o", "-t", "image/png" });
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
    tp.start(clipBin_, QStringList{ "-selection", "clipboard", "-o" });
    if (!tp.waitForFinished(1500))
        return;
    mime = QString::fromLatin1(kMimeText);
    data = tp.readAllStandardOutput();
}
#endif

void ClipboardService::content(QString& mime, QByteArray& data) const
{
#ifdef Q_OS_LINUX
    if (!available_ && !qobject_cast<QGuiApplication*>(QCoreApplication::instance()))
        const_cast<ClipboardService*>(this)->ensureCliMode();
#endif
    mime.clear();
    data.clear();
    if (!available_)
        return;
#ifdef Q_OS_LINUX
    if (cliMode_) {
        readCliContent(mime, data);
        return;
    }
#endif
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

#ifdef Q_OS_LINUX
void ClipboardService::pollClipboard()
{
    // CLI 模式没有剪贴板变化信号，轮询读当前内容并去重广播
    QString mime;
    QByteArray data;
    readCliContent(mime, data);
    if (data.isEmpty() || (mime == lastMime_ && data == lastData_))
        return;
    lastMime_ = mime;
    lastData_ = data;
    emit contentChanged(mime, data);
}
#endif

bool ClipboardService::setContentFromClient(const QString& mime, const QByteArray& data)
{
#ifdef Q_OS_LINUX
    if (!available_ && !qobject_cast<QGuiApplication*>(QCoreApplication::instance()))
        ensureCliMode();
#endif
    if (!available_ || data.isEmpty())
        return false;
    const bool isImage = (mime == QLatin1String(kMimeImage));
    if (isImage) {
#ifdef Q_OS_LINUX
        if (cliMode_ && !cliImageCapable_) {
            qWarning("ClipboardService: image clipboard requires xclip (xsel unsupported)");
            return false;
        }
#endif
#ifndef Q_OS_LINUX
        // 非 Linux 平台暂不支持图片剪贴板写入（Windows helper 走文本通道）
        return false;
#endif
    }
    // 先更新指纹再写入，写入触发的 dataChanged/轮询会被去重抑制，避免回声
    lastMime_ = mime;
    lastData_ = data;
    pendingData_.clear();
#ifdef Q_OS_LINUX
    if (cliMode_) {
        // xclip/xsel 必须常驻成为 CLIPBOARD selection owner 才能向其他 X 客户端提供数据。
        // 旧实现用局部 QProcess：函数返回即析构，Qt 会 kill 子进程，owner 随即消失，
        // 远端应用读取剪贴板时拿到空内容（表现为"粘贴无反应"）。改为持有一个常驻进程。
        if (cliOwner_) {
            cliOwner_->disconnect(this);
            if (cliOwner_->state() != QProcess::NotRunning) {
                cliOwner_->kill();
                cliOwner_->waitForFinished(1000);
            }
            cliOwner_->deleteLater();
            cliOwner_ = nullptr;
        }
        QProcess* owner = new QProcess(this);
        QStringList args;
        if (clipBin_.endsWith(QStringLiteral("xclip"))) {
            args << "-selection" << "clipboard" << "-i";
            if (isImage)
                args << "-t" << "image/png";
        } else {
            args << "--clipboard" << "--input";
        }
        owner->start(clipBin_, args);
        if (!owner->waitForStarted(2000)) {
            qWarning("ClipboardService: failed to start clipboard owner process");
            owner->deleteLater();
            return false;
        }
        owner->write(data);
        owner->closeWriteChannel();
        cliOwner_ = owner;
        // xclip 读完 stdin 后可能 fork 到后台并让前台进程退出，或失去 selection
        // （其他应用复制）后自行退出；这里统一清理成员指针。
        // 注意：必须在等待之前连接，否则等待期间已发出的 finished 会丢失，指针悬空。
        connect(owner, static_cast<void(QProcess::*)(int, QProcess::ExitStatus)>(&QProcess::finished),
            this, [this, owner](int, QProcess::ExitStatus) {
                if (cliOwner_ == owner)
                    cliOwner_ = nullptr;
                owner->deleteLater();
            });
        // 确保数据已写入管道：xclip 若在读全 stdin 后 fork，数据未送达时会以空内容
        // 成为 selection owner。xsel 不 fork、常驻前台，waitForFinished 超时返回。
        owner->waitForBytesWritten(1000);
        owner->waitForFinished(200);
        return true;
    }
#endif
    QClipboard* clipboard = QGuiApplication::clipboard();
    if (!clipboard)
        return false;
    if (isImage) {
        QImage img;
        if (!img.loadFromData(data, "PNG") || img.isNull())
            return false;
        clipboard->setImage(img);
    } else {
        clipboard->setText(QString::fromUtf8(data));
    }
    // 即使内容与当前剪贴板相同也返回 true：客户端主动粘贴必须触发一次远端 Ctrl+V，
    // 否则"远端复制→本地同步→再粘贴回远端"的场景会被去重吞掉
    return true;
}
