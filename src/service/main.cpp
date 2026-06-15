#include "service.h"
#include <shellapi.h>
#include <stdio.h>

static void PrintHelp(const wchar_t* exeName) {
    wprintf(L"QtRemoteDesktopKeyboardSvc - SYSTEM-level keyboard injector for secure desktop\n");
    wprintf(L"\n");
    wprintf(L"Installs/runs a Windows service that injects keyboard input via SendInput\n");
    wprintf(L"from the SYSTEM account, bypassing UIPI restrictions on the secure desktop.\n");
    wprintf(L"\n");
    wprintf(L"Usage:\n");
    wprintf(L"  %-30s  Install and register the service\n", L"  --install");
    wprintf(L"  %-30s  Unregister and remove the service\n", L"  --uninstall");
    wprintf(L"  %-30s  Display this help message\n", L"  --help");
    wprintf(L"\n");
    wprintf(L"After installation, control the service with:\n");
    wprintf(L"  net start %s      Start the service\n", SERVICE_NAME);
    wprintf(L"  net stop %s       Stop the service\n", SERVICE_NAME);
    wprintf(L"  sc query %s       Check service status\n", SERVICE_NAME);
    wprintf(L"\n");
    wprintf(L"Requirements:\n");
    wprintf(L"  - Must run as Administrator for --install / --uninstall\n");
    wprintf(L"  - The main QtRemoteDesktop program connects automatically when\n");
    wprintf(L"    the screen is locked\n");
    wprintf(L"  - Only supported on Windows Vista and later\n");
    wprintf(L"\n");
    wprintf(L"Installation steps:\n");
    wprintf(L"  1. Run as Administrator: %s --install\n", exeName);
    wprintf(L"  2. net start %s\n", SERVICE_NAME);
    wprintf(L"  3. Launch QtRemoteDesktop normally\n");
    wprintf(L"\n");
    wprintf(L"Uninstallation steps:\n");
    wprintf(L"  1. net stop %s\n", SERVICE_NAME);
    wprintf(L"  2. Run as Administrator: %s --uninstall\n", exeName);
}

int main() {
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

    SERVICE_TABLE_ENTRYW table[] = {
        { const_cast<LPWSTR>(SERVICE_NAME), ServiceMain },
        { NULL, NULL }
    };
    if (!StartServiceCtrlDispatcherW(table)) {
        DWORD err = GetLastError();
        if (err == ERROR_FAILED_SERVICE_CONTROLLER_CONNECT) {
            PrintHelp(L"QtRemoteDesktopKeyboardSvc.exe");
            return 1;
        }
        wprintf(L"StartServiceCtrlDispatcherW failed: %lu\n", err);
        return 1;
    }
    return 0;
}
