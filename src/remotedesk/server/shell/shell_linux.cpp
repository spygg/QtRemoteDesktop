#include "shell.h"
#include <QSocketNotifier>
#include <QWebSocket>
#include <QDebug>
#include <fcntl.h>
#include <pty.h>
#include <utmp.h>
#include <sys/ioctl.h>
#include <termios.h>
#include <unistd.h>
#include <signal.h>
#include <sys/wait.h>

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

    childPid_ = fork();
    if (childPid_ == 0) {
        setsid();
        const char* slaveName = ptsname(masterFd_);
        int slaveFd = open(slaveName, O_RDWR);
        ioctl(slaveFd, TIOCSCTTY, 0);
        dup2(slaveFd, 0); dup2(slaveFd, 1); dup2(slaveFd, 2);
        // 配置终端：Backspace 发送 DEL (0x7f)，匹配浏览器按键
        struct termios tios;
        tcgetattr(slaveFd, &tios);
        tios.c_cc[VERASE] = '\x7f';
        tcsetattr(slaveFd, TCSANOW, &tios);
        if (slaveFd > 2) close(slaveFd);
        close(masterFd_);
        setenv("TERM", "xterm-256color", 1);
        execl("/bin/bash", "/bin/bash", "--login", nullptr);
        _exit(1);
    }
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
            ws_->sendTextMessage(QByteArray(buf, n));
        } else {
            ws_->close();
        }
    });
}

void LinuxInteractiveShell::write(const QByteArray& data)
{
    if (masterFd_ >= 0)
        ::write(masterFd_, data.data(), data.size());
}

void LinuxInteractiveShell::resize(int cols, int rows)
{
    if (masterFd_ >= 0) {
        struct winsize ws;
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
        waitpid(childPid_, nullptr, WNOHANG);
        childPid_ = 0;
    }
    if (masterFd_ >= 0) { close(masterFd_); masterFd_ = -1; }
    InteractiveShell::stop();
}
