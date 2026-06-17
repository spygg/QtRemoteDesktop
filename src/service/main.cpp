#include "service.h"

#include <shellapi.h>
#include <stdio.h>

static void PrintHelp(const wchar_t* exeName)
{
    wprintf(L"QtRemoteDesktopKeyboardSvc - SYSTEM-level keyboard injector for secure desktop\n");
    wprintf(L"\n");
    wprintf(L"Installs/runs a Windows service that injects keyboard input via SendInput\n");
    wprintf(L"from the SYSTEM account, bypassing UIPI restrictions on the secure desktop.\n");
    wprintf(L"\n");
    wprintf(L"Usage:\n");
    wprintf(L"  %-30s  Auto-install and start the service (default)\n", L"(no args)");
    wprintf(L"  %-30s  Unregister and remove the service\n", L"  --uninstall");
    wprintf(L"  %-30s  Display this help message\n", L"  --help");
    wprintf(L"\n");
    wprintf(L"After installation, control the service with:\n");
    wprintf(L"  net start %s      Start the service\n", SERVICE_NAME);
    wprintf(L"  net stop %s       Stop the service\n", SERVICE_NAME);
    wprintf(L"  sc query %s       Check service status\n", SERVICE_NAME);
    wprintf(L"\n");
    wprintf(L"Requirements:\n");
    wprintf(L"  - Must run as Administrator (manifest requires it)\n");
    wprintf(L"  - The main QtRemoteDesktop program connects automatically when\n");
    wprintf(L"    the screen is locked\n");
    wprintf(L"  - Only supported on Windows Vista and later\n");
    wprintf(L"\n");
    wprintf(L"To remove:\n");
    wprintf(L"  %s --uninstall\n", exeName);
}

int main()
{
    int argc;
    wchar_t** argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (!argv) {
        wprintf(L"CommandLineToArgvW failed\n");
        return 1;
    }

    const wchar_t* exeName = wcsrchr(argv[0], L'\\');
    exeName = exeName ? exeName + 1 : argv[0];

    if (argc > 1) {
        if (wcscmp(argv[1], L"--help") == 0 || wcscmp(argv[1], L"/?") == 0
            || wcscmp(argv[1], L"-h") == 0) {
            PrintHelp(exeName);
            LocalFree(argv);
            return 0;
        }
        if (wcscmp(argv[1], L"--install") == 0) {
            BOOL r = InstallService();
            LocalFree(argv);
            return r ? 0 : 1;
        }
        if (wcscmp(argv[1], L"--uninstall") == 0) {
            BOOL r = UninstallService();
            LocalFree(argv);
            return r ? 0 : 1;
        }
        if (wcscmp(argv[1], L"--helper") == 0 && argc > 2) {
            DWORD sessionId = wcstoul(argv[2], NULL, 10);
            LocalFree(argv);
            return RunHelper(sessionId);
        }
        wprintf(L"Unknown option: %s\n\n", argv[1]);
        PrintHelp(exeName);
        LocalFree(argv);
        return 1;
    }

    LocalFree(argv);

    // Check if service already exists; if not, install first
    SC_HANDLE scm = OpenSCManagerW(NULL, NULL, SC_MANAGER_CREATE_SERVICE);
    if (!scm) {
        wprintf(L"OpenSCManagerW failed: %lu\n", GetLastError());
        return 1;
    }
    SC_HANDLE svc = OpenServiceW(scm, SERVICE_NAME, SERVICE_START);
    if (!svc) {
        CloseServiceHandle(scm);
        wprintf(L"Installing service...\n");
        if (!InstallService()) {
            wprintf(L"Failed to install service. Try running as Administrator.\n");
            return 1;
        }
        scm = OpenSCManagerW(NULL, NULL, SC_MANAGER_CONNECT);
        svc = OpenServiceW(scm, SERVICE_NAME, SERVICE_START);
        if (!svc) {
            wprintf(L"Failed to open service after install: %lu\n", GetLastError());
            CloseServiceHandle(scm);
            return 1;
        }
    }

    wprintf(L"Starting service...\n");
    if (StartServiceW(svc, 0, NULL)) {
        wprintf(L"Service '%s' started successfully.\n", SERVICE_NAME);
    } else {
        wprintf(L"Failed to start service: %lu\n", GetLastError());
    }
    CloseServiceHandle(svc);
    CloseServiceHandle(scm);
    return 0;
}
