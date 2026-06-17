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
    static bool launchHelperProcess();

    static void WINAPI serviceCtrlHandler(DWORD ctrlCode);
    static void WINAPI serviceMain(DWORD argc, LPWSTR* argv);

    static SERVICE_STATUS_HANDLE s_statusHandle;
    static SERVICE_STATUS s_status;
    static HANDLE s_stopEvent;
};
