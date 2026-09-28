#ifndef CLIPBOARD_SERVICE_H
#define CLIPBOARD_SERVICE_H

#include <QObject>
#include <QString>
#include <QByteArray>

class QTimer;
class QProcess;

// 共享剪贴板服务：支持文本（text/plain）与图片（image/png）两种内容类型。
// - GUI 模式（QGuiApplication）：QClipboard 原生读写，dataChanged 信号驱动远端→客户端同步。
// - CLI 模式（QCoreApplication + xclip）：xclip 读写 X11 CLIPBOARD，图片需 xclip
//   （xsel 不支持 image target）；CLI 模式无变化信号，用轮询定时器驱动同步。
class ClipboardService : public QObject {
    Q_OBJECT

public:
    explicit ClipboardService(QObject* parent = nullptr);
    ~ClipboardService();

    void start();

    // 客户端发来的内容 → 写入系统剪贴板。
    // mime: "text/plain" 或 "image/png"；data: 文本为 UTF-8 字节，图片为 PNG 原始字节。
    // 返回是否真正写入了新内容（用于触发远端粘贴）。
    bool setContentFromClient(const QString& mime, const QByteArray& data);
    // 读取当前系统剪贴板内容（用于客户端接入时的初始推送）
    void content(QString& mime, QByteArray& data) const;

    bool isAvailable() const { return available_; }

signals:
    // 剪贴板内容发生变化（系统复制或客户端写入）→ 由上层广播给所有客户端
    void contentChanged(const QString& mime, const QByteArray& data);

private slots:
    void onClipboardChanged();   // GUI 模式：QClipboard::dataChanged
    void flushPending();         // 去抖到期 → 去重后广播
    void pollClipboard();        // CLI 模式：轮询 X11 CLIPBOARD

private:
    bool available_ = false;
    QString lastMime_;          // 上次已推送/已写入的内容指纹，用于去重，避免回声循环
    QByteArray lastData_;
    QString pendingMime_;       // 待广播内容（去抖缓冲）
    QByteArray pendingData_;
    QTimer* debounceTimer_ = nullptr;

#ifdef Q_OS_LINUX
    bool cliMode_ = false;      // 无 QGuiApplication 时用 xclip/xsel 走 X11 CLIPBOARD
    bool cliImageCapable_ = false; // xclip 支持图片 target（xsel 不支持）
    QString clipBin_;           // xclip 或 xsel 的绝对路径
    // 常驻的 xclip/xsel 进程：必须存活才能作为 CLIPBOARD selection owner 向其他
    // X 客户端提供数据（局部 QProcess 析构会 kill 子进程 → owner 消失 → 粘贴为空）
    QProcess* cliOwner_ = nullptr;
    void ensureCliMode();       // 惰性启用 CLI 后端（DISPLAY 出现后自动初始化）
    void readCliContent(QString& mime, QByteArray& data) const;
#endif
};

#endif // CLIPBOARD_SERVICE_H
