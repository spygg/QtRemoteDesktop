#include "clipboardservice.h"

#include <QClipboard>
#include <QCoreApplication>
#include <QGuiApplication>
#include <QTimer>

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

QString ClipboardService::text() const
{
    if (!available_)
        return QString();
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
    if (!available_ || text.isEmpty())
        return false;
    // 先更新 lastText_ 再写入，setText 触发的 dataChanged 会被 flushPending 抑制，避免回声
    lastText_ = text;
    pendingText_.clear();
    QClipboard* clipboard = QGuiApplication::clipboard();
    if (!clipboard)
        return false;
    clipboard->setText(text);
    // 即使文本与当前剪贴板相同也返回 true：客户端主动粘贴必须触发一次远端 Ctrl+V，
    // 否则"远端复制→本地同步→再粘贴回远端"的场景会被去重吞掉
    return true;
}
