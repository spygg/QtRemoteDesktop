#include "service.h"
#include "windows_service.h"   // from server/windows_service/
#include "helper_process.h"    // from server/helper_process/
#include "secure_input_process.h"  // from server/secure_input_process/

#include <windows.h>
#include <cstdio>
#include <cstring>

#include <QString>

int platformMain(int argc, char* argv[])
{
    for (int i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "--install") == 0) {
            AllocConsole();
            freopen("CONOUT$", "w", stdout);
            freopen("CONOUT$", "w", stderr);
            // 安装/启动服务都要管理员权限。以前只在 SCM 调用失败时打一行错误码，
            // 普通 cmd 里跑完看起来"没报错"，实际什么都没发生 —— 先自查，缺权限
            // 就把同样的参数交给一个提权进程跑完。
            int elevRc = 0;
            if (WindowsService::relaunchElevatedIfNeeded(argc, argv, &elevRc))
                return elevRc;
            return WindowsService::install() ? 0 : 1;
        }
        if (strcmp(argv[i], "--uninstall") == 0) {
            AllocConsole();
            freopen("CONOUT$", "w", stdout);
            freopen("CONOUT$", "w", stderr);
            int elevRc = 0;
            if (WindowsService::relaunchElevatedIfNeeded(argc, argv, &elevRc))
                return elevRc;
            return WindowsService::uninstall() ? 0 : 1;
        }
        if (strcmp(argv[i], "--service") == 0) {
            return WindowsService::run(argc, argv);
        }
    }

    for (int i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "--secure-input") == 0) {
            int wsPort = 8081;
            if (i + 1 < argc)
                wsPort = QString::fromLocal8Bit(argv[i + 1]).toInt();
            return SecureInputProcess::run(argc, argv, wsPort);
        }
        if (strcmp(argv[i], "--helper") == 0) {
            return HelperProcess::run(argc, argv);
        }
        if (strcmp(argv[i], "--lock-once") == 0) {
            // [LOCK] 服务端在 helper 不回执时的兜底：用 console 会话令牌把本程序
            // 投到用户会话，在这里调 LockWorkStation() 并以退出码回报结果。
            // 退出码就是最直接的 ack：0 = BOOL TRUE，非 0 = 失败（拿不到 rundll32
            // 那样的"投递成功但不知道有没有锁上"的模糊结论）。
            const BOOL ok = LockWorkStation();
            return ok ? 0 : 2;
        }
    }
    return -1;
}
