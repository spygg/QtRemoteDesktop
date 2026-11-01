#include "service.h"

#include <cstdio>
#include <cstring>

// macOS 不提供可自装的后台服务模式。
// 抓屏（屏幕录制）、注入按键（辅助功能 / 输入监控）都是 TCC 授权，授权对象是
// "发起请求的那个可执行文件 + 它的调用者"，且必须存在于某个已登录用户的窗口
// 会话里；LaunchDaemon 跑在 SYSTEM 上下文，既拿不到 WindowServer 连接，也无法
// 由程序自己完成授权。所以这里不模仿 Linux 的 --install/--service，直接失败退出，
// 让调用方（脚本/用户）看见明确原因，而不是无声地以后台模式启动再抓不到屏。
int platformMain(int argc, char* argv[])
{
    for (int i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "--install") == 0 || strcmp(argv[i], "--uninstall") == 0
            || strcmp(argv[i], "--service") == 0) {
            fprintf(stderr,
                    "[service] macOS does not support service mode (%s).\n"
                    "[service] Screen capture and input injection need per-user TCC\n"
                    "[service] permissions (Screen Recording / Accessibility), which\n"
                    "[service] only exist inside a logged-in GUI session. Launch the\n"
                    "[service] app normally instead.\n",
                    argv[i]);
            return 1;
        }
    }
    return -1; // 非服务参数：落回普通桌面模式
}
