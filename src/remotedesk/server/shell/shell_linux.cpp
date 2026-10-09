#include "shell.h"
#include <QSocketNotifier>
#include <QWebSocket>
#include <QDebug>
#include <fcntl.h>
#include <cerrno>
#include <pty.h>
#include <utmp.h>
#include <sys/ioctl.h>
#include <termios.h>
#include <unistd.h>
#include <signal.h>
#include <sys/wait.h>
#include <string>
#include <vector>

class LinuxInteractiveShell : public InteractiveShell {
public:
    LinuxInteractiveShell(QWebSocket* socket, QObject* parent)
        : InteractiveShell(socket, parent) {}
    ~LinuxInteractiveShell() override { stop(); }

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
    auto* shell = new LinuxInteractiveShell(socket, parent);
    shell->start();
    sessions().insert(socket, shell);
    return shell;
}

void LinuxInteractiveShell::start()
{
    masterFd_ = posix_openpt(O_RDWR | O_NOCTTY);
    if (masterFd_ < 0) {
        qWarning() << "InteractiveShell: posix_openpt failed";
        ws_->close();
        return;
    }
    grantpt(masterFd_);
    unlockpt(masterFd_);

    // ── fork 之前完成所有"非 async-signal-safe"的准备 ──
    // 本进程是多线程的（Qt 事件循环 + 采集/编码线程）。fork 出来的子进程只能安全
    // 调用 async-signal-safe 函数：若 fork 瞬间恰好有其它线程持有 malloc / stdio 锁，
    // 子进程里再调 setenv / tcgetattr（都会碰 libc 锁）就可能永久死锁，表现为"点开
    // 终端后卡住不输出"。因此这里把从端打开、终端属性、环境表全部前置到父进程，
    // 子进程只做 setsid/dup2/execve（均为 AS-safe）。
    const char* slaveName = ptsname(masterFd_);
    if (!slaveName) {
        qWarning() << "InteractiveShell: ptsname failed";
        close(masterFd_); masterFd_ = -1;
        ws_->close();
        return;
    }
    const int slaveFd = ::open(slaveName, O_RDWR);
    if (slaveFd < 0) {
        qWarning() << "InteractiveShell: open slave pty failed";
        close(masterFd_); masterFd_ = -1;
        ws_->close();
        return;
    }
    // 配置终端：Backspace 发送 DEL (0x7f)，匹配浏览器按键
    struct termios tios;
    if (tcgetattr(slaveFd, &tios) == 0) {
        tios.c_cc[VERASE] = '\x7f';
        tcsetattr(slaveFd, TCSANOW, &tios);
    }

    // 环境表：继承父进程 environ 并覆盖 TERM/LANG/LC_ALL（execve 需要连续存储，
    // 由下面的 vector 持有，其内存在 fork 后被子进程继承，保持有效）
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
        // 子进程：仅使用 async-signal-safe 调用
        setsid();
        ioctl(slaveFd, TIOCSCTTY, 0);
        dup2(slaveFd, 0); dup2(slaveFd, 1); dup2(slaveFd, 2);
        if (slaveFd > 2) ::close(slaveFd);
        ::close(masterFd_);
        execve("/bin/bash", argv, envp.data());
        _exit(1);
    }
    // 父进程不再需要从端：不关会导致子进程退出后 PTY 不产生 EOF（会话泄漏）
    ::close(slaveFd);
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
        // [P2] EINTR/EAGAIN 不是 EOF：信号中断或非阻塞暂无数据时重试/返回，
        // 旧代码把 read()==-1 一律当 EOF 处理，信号一来自然断开用户 shell
        do {
            n = read(fd, buf, sizeof(buf));
        } while (n < 0 && (errno == EINTR || errno == EAGAIN));
        if (n > 0) {
            // 用二进制帧发送原始终端字节，避免 UTF-8 多字节序列被拆成多帧导致乱码
            ws_->sendBinaryMessage(QByteArray(buf, n));
        } else if (n == 0 || (n < 0 && errno != EINTR)) {
            ws_->close();
        }
    });
}

void LinuxInteractiveShell::write(const QByteArray& data)
{
    if (!data.isEmpty() && masterFd_ >= 0)
        ::write(masterFd_, data.data(), data.size());
}

void LinuxInteractiveShell::resize(int cols, int rows)
{
    if (masterFd_ >= 0) {
        // 必须整体零初始化：ws_xpixel/ws_ypixel 未初始化即传给内核，属未定义数据
        struct winsize ws = {};
        ws.ws_col = static_cast<unsigned short>(cols);
        ws.ws_row = static_cast<unsigned short>(rows);
        ioctl(masterFd_, TIOCSWINSZ, &ws);
    }
}

void LinuxInteractiveShell::stop()
{
    if (notifier_) { notifier_->setEnabled(false); }
    if (childPid_ > 0) {
        kill(childPid_, SIGTERM);
        // 有限等待子进程退出并回收。[P2] 旧实现最多在主线程阻塞 1s（20×50ms），
        // 会话多时 stop() 明显卡顿。bash 收 SIGTERM 通常 <100ms 退出；
        // 没退就 SIGKILL（回收近似瞬时）。
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
