// server/sessionagent/session_agent_run.h
#pragma once

namespace SessionAgentRun {
// `--session-agent` 入口：在用户会话里运行采集 + 输入代理。
// 返回进程退出码（非 Wayland / 已有实例 / 采集不可用时为 0，systemd 不重启）。
int run(int argc, char* argv[]);
} // namespace SessionAgentRun
