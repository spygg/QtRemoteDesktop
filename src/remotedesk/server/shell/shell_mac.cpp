#include "shell.h"
#include <QSocketNotifier>
#include <QWebSocket>
#include <QDebug>
#include <util.h>
#include <crt_externs.h>
#include <sys/ioctl.h>
#include <fcntl.h>
#include <termios.h>
#include <unistd.h>
#include <signal.h>
#include <sys/wait.h>
#include <cerrno>
#include <string>
#include <vector>

class MacInteractiveShell : public InteractiveShell {
public:
    MacInteractiveShell(QWebSocket* socket, QObject* parent)
        : InteractiveShell(socket, parent) {}
    ~MacInteractiveShell() override { stop(); }

    void start();
    void write(const QByteArray& data) override;
    void resize(int cols, int rows) override;
    void stop() override;

private:
    void flushWrite();

    int masterFd_ = -1;
    pid_t childPid_ = 0;
    QSocketNotifier* notifier_ = nullptr;
    QSocketNotifier* writeNotifier_ = nullptr;
    QByteArray pendingWrite_;
};

InteractiveShell* InteractiveShell::create(QWebSocket* socket, QObject* parent)
{
    auto* shell = new MacInteractiveShell(socket, parent);
    shell->start();
    sessions().insert(socket, shell);
    return shell;
}

void MacInteractiveShell::start()
{
    int slaveFd = -1;
    if (openpty(&masterFd_, &slaveFd, nullptr, nullptr, nullptr) != 0) {
        qWarning() << "InteractiveShell: openpty failed";
        ws_->close();
        return;
    }

    // master 必须非阻塞：否则远端 shell 不读 stdin（cat 大文件、vim 粘贴）时
    // 主线程的 write() 会卡死整个服务。读侧同样依赖 EAGAIN 语义。
    const int masterFlags = fcntl(masterFd_, F_GETFL, 0);
    if (masterFlags < 0
        || fcntl(masterFd_, F_SETFL, masterFlags | O_NONBLOCK) < 0) {
        qWarning() << "InteractiveShell: failed to set PTY master non-blocking";
        close(slaveFd);
        close(masterFd_);
        masterFd_ = -1;
        ws_->close();
        return;
    }

    // ── fork 之前完成终端属性与环境表准备（子进程只用 AS-safe 调用） ──
    struct termios tios;
    if (tcgetattr(slaveFd, &tios) == 0) {
        tios.c_cc[VERASE] = '\x7f';
        tcsetattr(slaveFd, TCSANOW, &tios);
    }
    std::vector<std::string> envStrings;
    // Apple 的 <unistd.h> 不声明 environ（符号本身在 libSystem 里），拿它的官方入口
    // 是 crt_externs.h 的 _NSGetEnviron()；直接写 environ 在 Clang 下是
    // "use of undeclared identifier"，Linux 那份 shell_linux.cpp 才能那么写。
    for (char** e = *_NSGetEnviron(); e && *e; ++e) {
        const std::string s(*e);
        if (s.compare(0, 5, "TERM=") == 0 || s.compare(0, 5, "LANG=") == 0
            || s.compare(0, 7, "LC_ALL=") == 0)
            continue;
        envStrings.push_back(s);
    }
    envStrings.push_back("TERM=xterm-256color");
    // 强制 UTF-8，保证 PTY 输出字节与前端 xterm 的 UTF-8 解码一致，避免中文乱码。
    // macOS 没有 glibc 那个 C.UTF-8 locale（也不给 locale-gen），写 C.UTF-8 会让
    // setlocale 失败退回 C=ASCII，正好把这里想防的乱码又请回来。
    envStrings.push_back("LANG=en_US.UTF-8");
    envStrings.push_back("LC_ALL=en_US.UTF-8");
    std::vector<char*> envp;
    envp.reserve(envStrings.size() + 1);
    for (std::string& s : envStrings)
        envp.push_back(const_cast<char*>(s.c_str()));
    envp.push_back(nullptr);
    // 交互式 shell：优先 $SHELL（绝对路径且可执行），退回 /bin/sh。
    // macOS 默认 $SHELL=/bin/zsh，硬编码 /bin/bash 会跑成旧版 bash（3.2）。
    const char* envShell = ::getenv("SHELL");
    std::string shellPath = (envShell && envShell[0] == '/'
                             && ::access(envShell, X_OK) == 0)
        ? std::string(envShell) : std::string("/bin/sh");
    // --login 只对 bash 传：dash/BusyBox sh 会把 --login 当脚本文件名。
    const bool shellIsBash = shellPath.size() >= 4
        && shellPath.compare(shellPath.size() - 4, 4, "bash") == 0;
    char* const argvLogin[] = { const_cast<char*>(shellPath.c_str()),
                                const_cast<char*>("--login"), nullptr };
    char* const argvPlain[] = { const_cast<char*>(shellPath.c_str()), nullptr };
    char* const* argv = shellIsBash ? argvLogin : argvPlain;
    // exec 失败要在 PTY 里给出原因；格式化不是 AS-safe，fork 前先拼好。
    const std::string execFailMsg = "remotedesk: failed to exec " + shellPath + "\n";

    childPid_ = fork();
    if (childPid_ == 0) {
        // 子进程内只用 async-signal-safe 调用（见 shell_linux.cpp 的详细说明：
        // 多线程进程 fork 后调用 setenv/tcgetattr 可能因 libc 锁而死锁）。
        // 终端属性与环境表已在 fork 前于父进程准备好。
        setsid();
        ioctl(slaveFd, TIOCSCTTY, 0);
        dup2(slaveFd, 0); dup2(slaveFd, 1); dup2(slaveFd, 2);
        if (slaveFd > 2) close(slaveFd);
        close(masterFd_);
        execve(shellPath.c_str(), argv, envp.data());
        // stderr 已指向 PTY 从端：exec 失败时前端终端能看到原因，而不是空白
        (void)::write(2, execFailMsg.c_str(), execFailMsg.size());
        _exit(127);
    }
    close(slaveFd);

    if (childPid_ < 0) {
        close(masterFd_); masterFd_ = -1;
        qWarning() << "InteractiveShell: fork failed";
        ws_->close();
        return;
    }

    notifier_ = new QSocketNotifier(masterFd_, QSocketNotifier::Read, this);
    connect(notifier_, &QSocketNotifier::activated, this, [this](int fd) {
        char buf[16384];
        int n;
        do {
            n = read(fd, buf, sizeof(buf));
        } while (n < 0 && errno == EINTR);
        if (n > 0) {
            // 用二进制帧发送原始终端字节，避免 UTF-8 多字节序列被拆成多帧导致乱码
            ws_->sendBinaryMessage(QByteArray(buf, n));
        } else if (n == 0 || (errno != EAGAIN && errno != EWOULDBLOCK)) {
            // EOF 或真错误才关闭；EAGAIN 只是这一轮没数据，交给下一次可读通知。
            ws_->close();
        }
    });

    writeNotifier_ = new QSocketNotifier(masterFd_, QSocketNotifier::Write, this);
    writeNotifier_->setEnabled(false);
    connect(writeNotifier_, &QSocketNotifier::activated,
            this, [this]() { flushWrite(); });
}

void MacInteractiveShell::write(const QByteArray& data)
{
    if (data.isEmpty() || masterFd_ < 0)
        return;
    // 单次粘贴上限，避免远端不读时 pendingWrite_ 无界增长。
    static const int kMaxPendingWrite = 8 * 1024 * 1024;
    if (data.size() > kMaxPendingWrite) {
        ws_->close();
        return;
    }
    pendingWrite_.append(data);
    if (pendingWrite_.size() > kMaxPendingWrite) {
        ws_->close();
        return;
    }
    flushWrite();
}

void MacInteractiveShell::flushWrite()
{
    if (masterFd_ < 0) {
        if (writeNotifier_)
            writeNotifier_->setEnabled(false);
        pendingWrite_.clear();
        return;
    }
    // 非阻塞 master：能写多少写多少，写不动的部分留在 pendingWrite_，
    // 等 QSocketNotifier 的下次可写通知。
    while (!pendingWrite_.isEmpty()) {
        const ssize_t n = ::write(masterFd_, pendingWrite_.constData(),
                                  static_cast<size_t>(pendingWrite_.size()));
        if (n > 0) {
            pendingWrite_.remove(0, static_cast<int>(n));
            continue;
        }
        if (n < 0 && errno == EINTR)
            continue;
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            if (writeNotifier_)
                writeNotifier_->setEnabled(true);
            return;
        }
        // 对端已关闭等硬错误：丢弃剩余数据，由读侧 EOF 关闭会话。
        pendingWrite_.clear();
        break;
    }
    if (writeNotifier_)
        writeNotifier_->setEnabled(!pendingWrite_.isEmpty());
}

void MacInteractiveShell::resize(int cols, int rows)
{
    if (masterFd_ >= 0) {
        struct winsize ws = {};
        ws.ws_col = static_cast<unsigned short>(cols);
        ws.ws_row = static_cast<unsigned short>(rows);
        ioctl(masterFd_, TIOCSWINSZ, &ws);
    }
}

void MacInteractiveShell::stop()
{
    if (notifier_) { notifier_->setEnabled(false); }
    if (writeNotifier_) { writeNotifier_->setEnabled(false); }
    pendingWrite_.clear();
    if (childPid_ > 0) {
        kill(childPid_, SIGTERM);
        // 与 Linux 实现一致：只等两次 50ms，没退就 SIGKILL，避免会话关闭时
        // 在主线程里 1s 轮询。
        for (int i = 0; i < 2; ++i) {
            if (waitpid(childPid_, nullptr, WNOHANG) == childPid_) {
                childPid_ = 0;
                break;
            }
            usleep(50 * 1000);
        }
        if (childPid_ != 0) {
            kill(childPid_, SIGKILL);
            waitpid(childPid_, nullptr, 0);
            childPid_ = 0;
        }
    }
    if (masterFd_ >= 0) { close(masterFd_); masterFd_ = -1; }
    InteractiveShell::stop();
}
