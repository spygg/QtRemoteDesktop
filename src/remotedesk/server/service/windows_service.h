#pragma once

#include <windows.h>

class WindowsService
{
public:
    static bool install();
    static bool uninstall();
    static int run(int argc, char* argv[]);

    // 非管理员时把完全相同的参数交给一个提权进程执行并等它结束。
    // 返回 true 表示调用方应直接以 *outExitCode 作为退出码返回（安装/卸载已在子进程里做完）；
    // 返回 false 表示当前已是管理员，调用方继续在本进程执行。
    // 背景：--install 只需要一次管理员权限，但现场几乎都是从普通 cmd 敲的 ——
    // OpenSCManagerW 静默失败只打印一行错误码，用户看到的是"没报错但页面打不开"。
    static bool relaunchElevatedIfNeeded(int argc, char* argv[], int* outExitCode);

private:
    static bool isAdmin();
    // outProcess 非空时返回已打开的进程句柄（调用方负责 CloseHandle）。
    // 必须保留句柄而不是之后按 PID 重新 OpenProcess：helper 退出后 PID 可能被
    // 系统复用，按 PID 杀进程会误杀同会话里恰好复用了该 PID 的其它进程。
    static DWORD launchHelperProcess(HANDLE* outProcess = nullptr);

    // [FIX-B9] 必须用 Ex 版本：SERVICE_CONTROL_SESSIONCHANGE 只投递给
    // RegisterServiceCtrlHandlerEx 注册的处理器，旧版单参处理器永远收不到，
    // 导致快速用户切换后新会话永久黑屏。
    static DWORD WINAPI serviceCtrlHandler(DWORD ctrlCode, DWORD eventType,
                                           LPVOID eventData, LPVOID context);
    static void WINAPI serviceMain(DWORD argc, LPWSTR* argv);

    static SERVICE_STATUS_HANDLE s_statusHandle;
    static SERVICE_STATUS s_status;
    static HANDLE s_stopEvent;
    static HANDLE s_sessionChangeEvent;   // SERVICE_CONTROL_SESSIONCHANGE → helper 重建
};
