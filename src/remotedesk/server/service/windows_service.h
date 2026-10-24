#pragma once

#include <windows.h>

class WindowsService
{
public:
    static bool install();
    static bool uninstall();
    static int run(int argc, char* argv[]);

private:
    static bool isAdmin();
    static DWORD launchHelperProcess();

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
