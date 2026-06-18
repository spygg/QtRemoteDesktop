#include "windows_service.h"
#include "rdpserver.h"

#include <QCoreApplication>
#include <QDir>
#include <QMessageLogContext>
#include <QTimer>

#include <string>
#include <winsvc.h>
#include <wtsapi32.h>

void logToFile(QtMsgType type, const QMessageLogContext& lg, const QString& msg);

#define SERVICE_NAME L"QtRemoteDesktop"

SERVICE_STATUS_HANDLE WindowsService::s_statusHandle = NULL;
SERVICE_STATUS WindowsService::s_status = {};
HANDLE WindowsService::s_stopEvent = NULL;

void WINAPI WindowsService::serviceCtrlHandler(DWORD ctrlCode)
{
    switch (ctrlCode) {
    case SERVICE_CONTROL_STOP:
    case SERVICE_CONTROL_SHUTDOWN:
        s_status.dwCurrentState = SERVICE_STOP_PENDING;
        SetServiceStatus(s_statusHandle, &s_status);
        SetEvent(s_stopEvent);
        break;
    }
}

DWORD WindowsService::launchHelperProcess()
{
    DWORD sessionId = WTSGetActiveConsoleSessionId();
    if (sessionId == 0xFFFFFFFF) {
        qInfo() << "LaunchHelper: no active console session";
        return 0;
    }

    HANDLE hProcToken = NULL;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_DUPLICATE | TOKEN_QUERY, &hProcToken)) {
        qWarning() << "LaunchHelper: OpenProcessToken failed, error:" << GetLastError();
        return 0;
    }

    HANDLE hDupToken = NULL;
    if (!DuplicateTokenEx(hProcToken, TOKEN_ALL_ACCESS, NULL, SecurityImpersonation, TokenPrimary, &hDupToken)) {
        qWarning() << "LaunchHelper: DuplicateTokenEx failed, error:" << GetLastError();
        CloseHandle(hProcToken);
        return 0;
    }
    CloseHandle(hProcToken);

    if (!SetTokenInformation(hDupToken, TokenSessionId, &sessionId, sizeof(sessionId))) {
        qWarning() << "LaunchHelper: SetTokenInformation failed, error:" << GetLastError();
        CloseHandle(hDupToken);
        return 0;
    }

    wchar_t exePath[MAX_PATH];
    GetModuleFileNameW(NULL, exePath, MAX_PATH);
    std::wstring cmdLine = std::wstring(L"\"") + exePath + L"\" --helper";

    STARTUPINFOW si = { sizeof(si) };
    si.lpDesktop = const_cast<wchar_t*>(L"winsta0\\default");
    PROCESS_INFORMATION pi;
    typedef BOOL (WINAPI *CPAUserW_t)(HANDLE, LPCWSTR, LPWSTR,
        LPSECURITY_ATTRIBUTES, LPSECURITY_ATTRIBUTES, BOOL, DWORD,
        LPVOID, LPCWSTR, LPSTARTUPINFOW, LPPROCESS_INFORMATION);
    CPAUserW_t pCreateProcessAsUserW = (CPAUserW_t)GetProcAddress(
        GetModuleHandleA("advapi32"), "CreateProcessAsUserW");

    bool ok = pCreateProcessAsUserW && pCreateProcessAsUserW(hDupToken, NULL, &cmdLine[0], NULL, NULL, FALSE, 0, NULL, NULL, &si, &pi);

    CloseHandle(hDupToken);

    if (ok) {
        qInfo() << "LaunchHelper: helper process started, PID:" << pi.dwProcessId;
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
        return pi.dwProcessId;
    }
    qWarning() << "LaunchHelper: CreateProcessAsUser failed, error:" << GetLastError();
    return 0;
}

void WINAPI WindowsService::serviceMain(DWORD argc, LPWSTR* argv)
{
    s_statusHandle = RegisterServiceCtrlHandlerW(SERVICE_NAME, serviceCtrlHandler);
    if (!s_statusHandle)
        return;

    s_status.dwServiceType = SERVICE_WIN32_OWN_PROCESS;
    s_status.dwCurrentState = SERVICE_START_PENDING;
    s_status.dwControlsAccepted = SERVICE_ACCEPT_STOP | SERVICE_ACCEPT_SHUTDOWN;
    SetServiceStatus(s_statusHandle, &s_status);

    s_stopEvent = CreateEventW(NULL, TRUE, FALSE, NULL);
    if (!s_stopEvent) {
        s_status.dwCurrentState = SERVICE_STOPPED;
        SetServiceStatus(s_statusHandle, &s_status);
        return;
    }

#if QT_VERSION < QT_VERSION_CHECK(6, 0, 0)
    QCoreApplication::setAttribute(Qt::AA_EnableHighDpiScaling);
#endif

    {
        int qtArgc = 1;
        char* qtArgv[] = { const_cast<char*>("QtRemoteDesktop"), NULL };
        QCoreApplication app(qtArgc, qtArgv);

        s_status.dwCurrentState = SERVICE_RUNNING;
        SetServiceStatus(s_statusHandle, &s_status);

        QString logDir = QString("%1/logs").arg(QCoreApplication::applicationDirPath());
        QDir().mkpath(logDir);
        qInstallMessageHandler(logToFile);

        RDPServer server;

        if (!server.initialize(QString(), true, true)) {
            s_status.dwCurrentState = SERVICE_STOPPED;
            SetServiceStatus(s_statusHandle, &s_status);
        } else {
            server.start();

            QTimer tickTimer;
            QObject::connect(&tickTimer, &QTimer::timeout, [&]() {
                if (WaitForSingleObject(s_stopEvent, 0) == WAIT_OBJECT_0)
                    app.quit();
            });
            tickTimer.start(1000);

            QTimer helperTimer;
            DWORD helperPid = 0;
            QObject::connect(&helperTimer, &QTimer::timeout, [&]() {
                if (server.isCaptureSourceConnected()) {
                    helperTimer.stop();
                    return;
                }
                if (helperPid != 0) {
                    HANDLE hProc = OpenProcess(PROCESS_QUERY_INFORMATION, FALSE, helperPid);
                    if (hProc) {
                        DWORD exitCode;
                        if (GetExitCodeProcess(hProc, &exitCode) && exitCode == STILL_ACTIVE) {
                            CloseHandle(hProc);
                            return;
                        }
                        CloseHandle(hProc);
                    }
                    helperPid = 0;
                }
                DWORD pid = launchHelperProcess();
                if (pid != 0) {
                    helperPid = pid;
                    helperTimer.setInterval(5000);
                } else {
                    // 无用户会话时慢速轮询，有会话但启动失败时快速重试
                    bool hasSession = (WTSGetActiveConsoleSessionId() != 0xFFFFFFFF);
                    helperTimer.setInterval(hasSession ? 1000 : 5000);
                }
            });
            helperTimer.start(5000);
            helperPid = launchHelperProcess();

            app.exec();
            tickTimer.stop();
        }
    }

    CloseHandle(s_stopEvent);
    s_stopEvent = NULL;
}

bool WindowsService::isAdmin()
{
    BOOL admin = FALSE;
    PSID adminGroup = NULL;
    SID_IDENTIFIER_AUTHORITY ntAuth = SECURITY_NT_AUTHORITY;
    if (AllocateAndInitializeSid(&ntAuth, 2, SECURITY_BUILTIN_DOMAIN_RID, DOMAIN_ALIAS_RID_ADMINS, 0, 0, 0, 0, 0, 0, &adminGroup)) {
        CheckTokenMembership(NULL, adminGroup, &admin);
        FreeSid(adminGroup);
    }
    return !!admin;
}

bool WindowsService::install()
{
    wchar_t path[MAX_PATH];
    GetModuleFileNameW(NULL, path, MAX_PATH);

    wchar_t cmdLine[MAX_PATH + 32];
    swprintf(cmdLine, L"\"%S\" --service", path);

    SC_HANDLE scm = OpenSCManagerW(NULL, NULL, SC_MANAGER_CREATE_SERVICE);
    if (!scm) {
        wprintf(L"OpenSCManagerW failed: %lu\n", GetLastError());
        return FALSE;
    }

    SC_HANDLE svc = CreateServiceW(
        scm, SERVICE_NAME, L"Qt Remote Desktop Server",
        SERVICE_ALL_ACCESS,
        SERVICE_WIN32_OWN_PROCESS,
        SERVICE_AUTO_START,
        SERVICE_ERROR_NORMAL,
        cmdLine, NULL, NULL, NULL, NULL, NULL);

    if (svc) {
        wprintf(L"Service '%S' installed successfully.\n", SERVICE_NAME);
        CloseServiceHandle(svc);
        CloseServiceHandle(scm);
        return TRUE;
    }

    wprintf(L"CreateServiceW failed: %lu\n", GetLastError());
    CloseServiceHandle(scm);
    return FALSE;
}

bool WindowsService::uninstall()
{
    SC_HANDLE scm = OpenSCManagerW(NULL, NULL, SC_MANAGER_CONNECT);
    if (!scm) {
        wprintf(L"OpenSCManagerW failed: %lu\n", GetLastError());
        return FALSE;
    }

    SC_HANDLE svc = OpenServiceW(scm, SERVICE_NAME,
        SERVICE_QUERY_STATUS | SERVICE_STOP | DELETE);
    if (!svc) {
        wprintf(L"Service not found: %lu\n", GetLastError());
        CloseServiceHandle(scm);
        return FALSE;
    }

    SERVICE_STATUS ss;
    ControlService(svc, SERVICE_CONTROL_STOP, &ss);
    DeleteService(svc);

    CloseServiceHandle(svc);
    CloseServiceHandle(scm);
    wprintf(L"Service '%S' removed.\n", SERVICE_NAME);
    return TRUE;
}

int WindowsService::run(int argc, char* argv[])
{
    (void*)argc;
    (void*)argv;

    wchar_t serviceName[] = SERVICE_NAME;
    SERVICE_TABLE_ENTRYW table[] = {
        { serviceName, serviceMain },
        { NULL, NULL }
    };
    if (!StartServiceCtrlDispatcherW(table)) {
        fprintf(stderr, "StartServiceCtrlDispatcherW failed: %lu\n", GetLastError());
    }
    return 0;
}
