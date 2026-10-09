#include "shell.h"
#include <QSocketNotifier>
#include <QWebSocket>
#include <QDebug>
#include <util.h>
#include <sys/ioctl.h>
#include <termios.h>
#include <unistd.h>
#include <signal.h>
#include <sys/wait.h>
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
    int masterFd_ = -1;
    pid_t childPid_ = 0;
    QSocketNotifier* notifier_ = nullptr;
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

    // ── fork 之前完成终端属性与环境表准备（子进程只用 AS-safe 调用） ──
    struct termios tios;
    if (tcgetattr(slaveFd, &tios) == 0) {
        tios.c_cc[VERASE] = '\x7f';
        tcsetattr(slaveFd, TCSANOW, &tios);
    }
    std::vector<std::string> envStrings;
    for (char** e = environ; e && *e; ++e) {
        const std::string s(*e);
        if (s.compare(0, 5, "TERM=") == 0 || s.compare(0, 5, "LANG=") == 0
            || s.compare(0, 7, "LC_ALL=") == 0)
            continue;
        envStrings.push_back(s);
    }
    envStrings.push_back("TERM=xterm-256color");
    // 强制 UTF-8，保证 PTY 输出字节与前端 xterm 的 UTF-8 解码一致，避免中文乱码
    envStrings.push_back("LANG=C.UTF-8");
    envStrings.push_back("LC_ALL=C.UTF-8");
    std::vector<char*> envp;
    envp.reserve(envStrings.size() + 1);
    for (std::string& s : envStrings)
        envp.push_back(const_cast<char*>(s.c_str()));
    envp.push_back(nullptr);
    char* const argv[] = { const_cast<char*>("/bin/bash"),
                           const_cast<char*>("--login"), nullptr };

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
        execve("/bin/bash", argv, envp.data());
        _exit(1);
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
        int n = read(fd, buf, sizeof(buf));
        if (n > 0) {
            // 用二进制帧发送原始终端字节，避免 UTF-8 多字节序列被拆成多帧导致乱码
            ws_->sendBinaryMessage(QByteArray(buf, n));
        } else {
            ws_->close();
        }
    });
}

void MacInteractiveShell::write(const QByteArray& data)
{
    if (!data.isEmpty() && masterFd_ >= 0)
        ::write(masterFd_, data.data(), data.size());
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
    if (childPid_ > 0) {
        kill(childPid_, SIGTERM);
        // 有限等待子进程退出并回收（WNOHANG 只回收已退出的，会残留僵尸）
        for (int i = 0; i < 20; ++i) {
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
