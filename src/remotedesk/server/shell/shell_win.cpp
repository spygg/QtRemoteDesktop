#include "shell.h"
#include <QProcess>
#include <QWebSocket>
#include <QDebug>
#include <QTimer>
#include <windows.h>

class WinInteractiveShell : public InteractiveShell {
public:
    WinInteractiveShell(QWebSocket* socket, QObject* parent)
        : InteractiveShell(socket, parent) {}
    ~WinInteractiveShell() override { stop(); }

    void start();
    void write(const QByteArray& data) override;
    void resize(int, int) override {}
    void stop() override;
    bool hasRemoteEcho() const override { return false; }

private:
    QProcess* proc_ = nullptr;
    QString lineBuf_;
};

InteractiveShell* InteractiveShell::create(QWebSocket* socket, QObject* parent)
{
    auto* shell = new WinInteractiveShell(socket, parent);
    shell->start();
    sessions().insert(socket, shell);
    return shell;
}

void WinInteractiveShell::start()
{
    proc_ = new QProcess(this);
    proc_->setProcessChannelMode(QProcess::MergedChannels);

    connect(proc_, &QProcess::readyReadStandardOutput, this, [this]() {
        QByteArray data = proc_->readAllStandardOutput();
        if (!data.isEmpty())
            // cmd 输出按本地代码页字节接收，转成 UTF-8 后用二进制帧发送，
            // 使前端 xterm 按 UTF-8 渲染，避免多字节字符乱码
            ws_->sendBinaryMessage(QString::fromLocal8Bit(data).toUtf8());
    });
    connect(proc_, QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished),
            this, [this]() { ws_->close(); });
    // 启动失败（如 cmd.exe 缺失）异步处理，避免在主线程同步阻塞（waitForStarted 最长 5s）
    connect(proc_, &QProcess::errorOccurred, this, [this](QProcess::ProcessError err) {
        if (err == QProcess::FailedToStart) {
            qWarning() << "InteractiveShell: failed to start cmd.exe";
            ws_->close();
        }
    });

    // 异步启动：不调用 waitForStarted，避免阻塞主线程（屏幕捕获/WebSocket 全卡）
    proc_->start("cmd.exe", QStringList() << "/Q");
}

void WinInteractiveShell::write(const QByteArray& data)
{
    if (data.isEmpty() || !proc_ || proc_->state() != QProcess::Running)
        return;

    if (data.size() == 1) {
        char c = data[0];
        // 退格：从行缓冲中移除末字符，不写入管道（管道不处理退格）
        if (c == '\x7f') {
            if (!lineBuf_.isEmpty())
                lineBuf_.chop(1);
            return;
        }
        // 回车：将缓冲行写入管道
        if (c == '\n' || c == '\r') {
            proc_->write(lineBuf_.toLocal8Bit() + "\r\n");
            lineBuf_.clear();
            return;
        }
        // Ctrl+C：通过 GenerateConsoleCtrlEvent 发送中断信号
        // （管道写入 \x03 不会被 cmd.exe 解释为 Ctrl+C）
        if (c == '\x03') {
            lineBuf_.clear();
            DWORD pid = proc_->processId();
            BOOL hadConsole = GetConsoleWindow() != NULL;
            FreeConsole();
            if (AttachConsole(pid)) {
                SetConsoleCtrlHandler(NULL, TRUE);
                GenerateConsoleCtrlEvent(CTRL_C_EVENT, 0);
                Sleep(50);
                FreeConsole();
                SetConsoleCtrlHandler(NULL, FALSE);
            }
            if (hadConsole)
                AttachConsole(ATTACH_PARENT_PROCESS);
            return;
        }
        if (c == '\x04' || c == '\x1a') {
            lineBuf_.clear();
            proc_->write(data);
            return;
        }
        // 可打印字符（含扩展 ASCII 0x80-0xFF）：加入行缓冲
        if (c >= 32) {
            lineBuf_ += QChar::fromLatin1(c);
            return;
        }
        // 其余控制字符直写
        proc_->write(data);
        return;
    }

    // 转义序列（方向键等）：直写管道
    if (data[0] == '\x1b') {
        proc_->write(data);
        return;
    }

    // 多字节文本（如中文，已由服务端转为本地编码）：写入行缓冲
    QString text = QString::fromLocal8Bit(data);
    lineBuf_ += text;
}

void WinInteractiveShell::stop()
{
    if (proc_) {
        // kill 后交由 finished 信号异步清理（ws_->close 已在 finished 里处理），
        // 不用 waitForFinished 在主线程阻塞等待进程退出
        proc_->kill();
        proc_->deleteLater();
        proc_ = nullptr;
    }
    InteractiveShell::stop();
}
