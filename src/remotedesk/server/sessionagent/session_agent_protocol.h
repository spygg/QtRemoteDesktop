// server/sessionagent/session_agent_protocol.h
//
// 会话代理（session agent）与 root 服务之间的私有协议常量。
//
// 角色划分：
//   * 代理：运行在**用户会话**里（systemd user unit / 登录自启），持有
//     PipeWire ScreenCast / RemoteDesktop portal 授权与 uinput 设备；
//     它既是 unix socket 的服务端，也是共享内存帧的写入方。
//   * root 服务：保留端口绑定、配置、编码、回落等职责；作为 socket 客户端
//     读取帧、转发输入。
//
// 控制通道是 newline 分隔的 JSON（AF_UNIX SOCK_STREAM），像素走 /dev/shm
// 的两槽环形缓冲，避免把整帧塞进 socket。
#pragma once

#include <QString>

#include <unistd.h>

namespace SessionAgent {

constexpr int kProtocolVersion = 1;

// 两槽足够把"代理正在写下一帧"与"root 正在拷贝上一帧"分开；
// 再不济也不会让写入方覆盖尚未 ack 的槽。
constexpr int kSlots = 2;

// 单帧上限（8K@32bpp 约 130MB 太夸张，这里给 64MB 足够 4K/5K）。
constexpr int kMaxFrameBytes = 64 * 1024 * 1024;

// 代理 socket 所在目录：优先 XDG_RUNTIME_DIR（systemd user unit 一定设置），
// 否则按 uid 找 /run/user/<uid>；再不行退回 /tmp（只在手工调试时出现）。
inline QString socketDir()
{
    const QByteArray rt = qgetenv("XDG_RUNTIME_DIR");
    if (!rt.isEmpty())
        return QString::fromUtf8(rt) + QStringLiteral("/remotedesk");
    const uid_t uid = ::getuid();
    const QString run = QStringLiteral("/run/user/") + QString::number(uid);
    if (::access(qPrintable(run), F_OK) == 0)
        return run + QStringLiteral("/remotedesk");
    return QStringLiteral("/tmp/remotedesk-") + QString::number(uid);
}

inline QString socketPath()
{
    return socketDir() + QStringLiteral("/session-agent.sock");
}

// 共享内存文件名带 uid/pid：root 扫描时可据此核对 hello 里的 pid。
inline QString shmPath(int uid, int pid)
{
    return QStringLiteral("/dev/shm/remotedesk-agent.%1.%2")
        .arg(uid)
        .arg(pid);
}

} // namespace SessionAgent
