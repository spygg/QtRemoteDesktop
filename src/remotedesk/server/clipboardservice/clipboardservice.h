#ifndef CLIPBOARD_SERVICE_H
#define CLIPBOARD_SERVICE_H

#include <QObject>
#include <QString>

class QTimer;

class ClipboardService : public QObject {
    Q_OBJECT

public:
    explicit ClipboardService(QObject* parent = nullptr);
    ~ClipboardService();

    void start();

    // 客户端发来的文本 → 写入系统剪贴板。返回是否真正写入了新内容（用于触发远端粘贴）
    bool setTextFromClient(const QString& text);
    QString text() const;

    bool isAvailable() const { return available_; }

signals:
    // 剪贴板内容发生变化（系统复制或客户端写入）→ 由上层广播给所有客户端
    void textChanged(const QString& text);

private slots:
    void onClipboardChanged();
    void flushPending();

private:
    bool available_ = false;
    QString lastText_;      // 上次已推送/已写入的文本，用于去重，避免回声循环
    QString pendingText_;   // 待广播文本（去抖缓冲）
    QTimer* debounceTimer_ = nullptr;
};

#endif // CLIPBOARD_SERVICE_H
