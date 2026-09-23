#include "clipboardservice.h"

#include <QClipboard>
#include <QCoreApplication>
#include <QGuiApplication>
#include <QProcess>
#include <QStandardPaths>
#include <QTimer>
#include <QDebug>

ClipboardService::ClipboardService(QObject* parent)
    : QObject(parent)
{
    debounceTimer_ = new QTimer(this);
    debounceTimer_->setSingleShot(true);
    debounceTimer_->setInterval(150);
    connect(debounceTimer_, &QTimer::timeout, this, &ClipboardService::flushPending);
}

ClipboardService::~ClipboardService() = default;

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
            if (bin.isEmpty())
                bin = QStandardPaths::findExecutable(QStringLiteral("xsel"));
            if (!bin.isEmpty()) {
                cliMode_ = true;
                clipBin_ = bin;
                available_ = true;
                qInfo() << "ClipboardService: using" << qPrintable(bin) << "for X11 clipboard (service mode)";
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
    QString t = clipboard->text();
    if (!t.isEmpty() && t != lastText_) {
        lastText_ = t;
        emit textChanged(t);
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
    if (bin.isEmpty())
        bin = QStandardPaths::findExecutable(QStringLiteral("xsel"));
    if (bin.isEmpty()) {
        qWarning("ClipboardService: no xclip/xsel found, clipboard disabled");
        return;
    }
    cliMode_ = true;
    clipBin_ = bin;
    available_ = true;
    qInfo() << "ClipboardService: using" << qPrintable(bin)
            << "for X11 clipboard (lazy init, DISPLAY =" << qgetenv("DISPLAY").constData() << ")";
}
#endif

QString ClipboardService::text() const
{
#ifdef Q_OS_LINUX
    if (!available_ && !qobject_cast<QGuiApplication*>(QCoreApplication::instance()))
        const_cast<ClipboardService*>(this)->ensureCliMode();
#endif
    if (!available_)
        return QString();
#ifdef Q_OS_LINUX
    if (cliMode_) {
        // 读取 X11 CLIPBOARD（远端→本地同步时使用）
        QProcess p;
        QStringList args = clipBin_.endsWith(QStringLiteral("xclip"))
            ? QStringList{ "-selection", "clipboard", "-o" }
            : QStringList{ "--clipboard", "--output" };
        p.start(clipBin_, args);
        if (p.waitForFinished(2000))
            return QString::fromUtf8(p.readAllStandardOutput()).trimmed();
        return QString();
    }
#endif
    QClipboard* clipboard = QGuiApplication::clipboard();
    return clipboard ? clipboard->text() : QString();
}

void ClipboardService::onClipboardChanged()
{
    // A single copy action can fire dataChanged several times;
    // buffer the value and debounce so we only report the final content.
    QClipboard* clipboard = QGuiApplication::clipboard();
    if (!clipboard)
        return;
    pendingText_ = clipboard->text();
    debounceTimer_->start();
}

void ClipboardService::flushPending()
{
    if (pendingText_.isEmpty() || pendingText_ == lastText_)
        return;
    lastText_ = pendingText_;
    emit textChanged(pendingText_);
}

bool ClipboardService::setTextFromClient(const QString& text)
{
#ifdef Q_OS_LINUX
    if (!available_ && !qobject_cast<QGuiApplication*>(QCoreApplication::instance()))
        ensureCliMode();
#endif
    if (!available_ || text.isEmpty())
        return false;
    // 先更新 lastText_ 再写入，setText 触发的 dataChanged 会被 flushPending 抑制，避免回声
    lastText_ = text;
    pendingText_.clear();
#ifdef Q_OS_LINUX
    if (cliMode_) {
        // xclip 从 stdin 读入并成为 CLIPBOARD selection owner（进程常驻，向 X 客户端提供数据）；
        // xsel 同理。QProcess 析构不杀子进程，owner 持续有效。
        QProcess p;
        QStringList args = clipBin_.endsWith(QStringLiteral("xclip"))
            ? QStringList{ "-selection", "clipboard", "-i" }
            : QStringList{ "--clipboard", "--input" };
        p.start(clipBin_, args);
        if (!p.waitForStarted(2000))
            return false;
        p.write(text.toUtf8());
        p.closeWriteChannel();
        // xclip/xsel 作为 owner 常驻不退出，只等待管道写入完成即返回
        p.waitForFinished(500);
        return true;
    }
#endif
    QClipboard* clipboard = QGuiApplication::clipboard();
    if (!clipboard)
        return false;
    clipboard->setText(text);
    // 即使文本与当前剪贴板相同也返回 true：客户端主动粘贴必须触发一次远端 Ctrl+V，
    // 否则"远端复制→本地同步→再粘贴回远端"的场景会被去重吞掉
    return true;
}
