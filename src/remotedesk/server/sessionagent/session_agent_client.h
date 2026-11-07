// server/sessionagent/session_agent_client.h
//
// root 侧的会话代理客户端：
//   * 发现用户会话里的 socket（$QTRD_AGENT_SOCKET → logind seat0 的 /run/user/<uid>
//     → 扫描 /run/user/*）；
//   * 完成 hello 握手、映射 /dev/shm 双槽缓冲、收到帧后 copy 成 QImage 并 ack；
//   * 把输入事件以 JSON 转发给代理；
//   * 检测代理退出/心跳超时，并把"采集来源发生变化"暴露给 LinuxService，
//     由后者 restartCapture() 在代理与本地回落之间切换。
//
// 单例由 LinuxService::run() 启动；ScreenCapturer 的 AgentCapturer 与
// RDPServer 的 InputManager 共享同一个实例。
#pragma once

#include "external_injector.h"

#include <QImage>
#include <QJsonObject>
#include <QMutex>
#include <QObject>
#include <QString>

#include <atomic>

class QLocalSocket;
class QTimer;

class SessionAgentClient : public QObject, public ExternalInjector
{
    Q_OBJECT
public:
    static SessionAgentClient& shared();

    // LinuxService::run() 在事件循环前调用；内部用 1s tick 驱动发现/心跳。
    void start();

    // 是否已经收到至少一帧（AgentCapturer 只在此时接管）。
    bool isStreaming() const { return streaming_; }

    // 代理侧 uinput 可用（输入由代理接管）；为 false 时帧仍来自代理，
    // 但输入必须降级到 root 侧注入。
    bool inputActive() const { return streaming_ && inputAvailable_; }

    // 采集来源发生变化（代理接管 / 代理掉线），由 LinuxService 消费。
    bool captureChangePending() const { return captureChangePending_; }
    void clearCaptureChangePending() { captureChangePending_ = false; }

    int width() const { return width_; }
    int height() const { return height_; }

    // 取最近一帧。updated 表示自上次调用以来是否有新帧。
    bool takeFrame(QImage& out, bool* updated = nullptr);

    void requestFrame();
    void setPaused(bool paused);

    // ExternalInjector
    bool isActive() const override { return streaming_ && inputAvailable_; }
    bool injectMouseMove(int x, int y) override;
    bool injectMouseButton(int x, int y, int button, bool isDown) override;
    bool injectWheel(int delta) override;
    bool injectKeyboard(int keycode, const QString& code, bool isDown,
                        bool ctrl, bool alt, bool shift,
                        bool isChar, bool meta) override;
    bool updateModifiers(bool ctrl, bool alt, bool shift) override;

private slots:
    void onConnected();
    void onReadyRead();
    void onDisconnected();
    void tick();

private:
    SessionAgentClient();

    void connectToAgent();
    void handleMessage(const QJsonObject& obj);
    void sendJson(const QJsonObject& obj);
    void resetLink(const QString& reason);
    void markStreaming(bool on);
    void setCaptureChangePending();
    void reloadShm(const QString& path, int slotBytes);
    void unmapShm();

    QLocalSocket* socket_ = nullptr;
    QTimer* tickTimer_ = nullptr;

    // 单次连接的超时（connect 300ms / hello 1s），用 deadline 在 tick 里判断。
    qint64 connectDeadlineMs_ = 0;
    qint64 lastAttemptMs_ = 0;
    qint64 lastMsgMs_ = 0;
    qint64 lastPingMs_ = 0;
    qint64 lastResetLogMs_ = 0;

    bool started_ = false;
    bool handshaken_ = false;
    bool streaming_ = false;
    bool inputAvailable_ = false;
    bool captureChangePending_ = false;
    bool discoveryLogged_ = false;
    int expectedUid_ = -1;

    // hello 信息
    int agentPid_ = 0;
    int agentUid_ = -1;

    // 帧缓冲
    uchar* shmBase_ = nullptr;
    int shmSlotBytes_ = 0;
    int shmFd_ = -1;
    QString shmPath_;
    int width_ = 0;
    int height_ = 0;

    QMutex frameMutex_;
    QImage latestFrame_;
    quint64 latestSeq_ = 0;
    quint64 deliveredSeq_ = 0;

    std::atomic<bool> stopRequested_{false};
};
