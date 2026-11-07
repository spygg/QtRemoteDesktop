#include "service.h"
#include "linux_service.h"   // from server/linux_service/
#include "session_agent_run.h"

#include <cstdio>
#include <cstring>

int platformMain(int argc, char* argv[])
{
    for (int i = 1; i < argc; ++i) {
        // 用户会话代理：由 systemd user unit 拉起，不经过 root 服务的进程树。
        if (strcmp(argv[i], "--session-agent") == 0) {
            return SessionAgentRun::run(argc, argv);
        }
        if (strcmp(argv[i], "--install") == 0) {
            return LinuxService::install(argc, argv);
        }
        if (strcmp(argv[i], "--uninstall") == 0) {
            return LinuxService::uninstall(argc, argv);
        }
        if (strcmp(argv[i], "--service") == 0) {
            return LinuxService::run(argc, argv);
        }
    }
    return -1;
}
