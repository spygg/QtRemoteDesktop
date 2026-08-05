#include "clipboardservice.h"

#include <QClipboard>
#include <QCoreApplication>
#include <QGuiApplication>

ClipboardService::ClipboardService(QObject* parent)
    : QObject(parent)
{
}

ClipboardService::~ClipboardService() = default;

void ClipboardService::start()
{
    // 无 QGuiApplication（如 Windows 服务仅创建 QCoreApplication）时，
    // QGuiApplication::clipboard() 返回的指针无效，访问会崩溃。
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

    // 推送当前剪贴板内容作为初始状态
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
    if (!available_)
        return;
    QClipboard* clipboard = QGuiApplication::clipboard();
    if (!clipboard)
        return;
    QString t = clipboard->text();
    if (t.isEmpty() || t == lastText_)
        return;
    lastText_ = t;
    emit textChanged(t);
}

void ClipboardService::setTextFromClient(const QString& text)
{
    if (!available_ || text.isEmpty() || text == lastText_)
        return;
    QClipboard* clipboard = QGuiApplication::clipboard();
    if (!clipboard)
        return;
    // 先记录，避免 setText 触发的 dataChanged 又被广播回去
    lastText_ = text;
    clipboard->setText(text);
    emit textChanged(text);
}
