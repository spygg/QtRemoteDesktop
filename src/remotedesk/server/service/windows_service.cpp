#include "windows_service.h"
#include "rdpserver.h"
#include "crashhandler.h"

#include <QCoreApplication>
#include <QDir>
#include <QMessageLogContext>
#include <QTimer>

#include <string>
#include <wchar.h>
#include <winsvc.h>
#include <wtsapi32.h>

void logToFile(QtMsgType type, const QMessageLogContext& lg, const QString& msg);
void applyLogLevelFromArgs(int argc, char* argv[]);

#define SERVICE_NAME L"QtRemoteDesktop"

SERVICE_STATUS_HANDLE WindowsService::s_statusHandle = NULL;
SERVICE_STATUS WindowsService::s_status = {};
HANDLE WindowsService::s_stopEvent = NULL;

void WINAPI WindowsService::serviceCtrlHandler(DWORD ctrlCode)
{
    switch (ctrlCode) {
    case SERVICE_CONTROL_STOP:
    case SERVICE_CONTROL_SHUTDOWN:
        // [P2] STOP_PENDING 必须带 dwWaitHint 并推进 dwCheckPoint：
        // 退出链路（停采集/编码线程/断开客户端）可能超过 SCM 默认宽限，
        // 不报 WaitHint 会被 SCM 直接强杀（错误 1053）。
        s_status.dwCurrentState = SERVICE_STOP_PENDING;
        s_status.dwCheckPoint = 1;
        s_status.dwWaitHint = 15000;
        SetServiceStatus(s_statusHandle, &s_status);
        SetEvent(s_stopEvent);
        break;
    default:
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

    // Find user's process (explorer.exe) in target session to get the real user token.
    // Using SYSTEM token causes DXGI Desktop Duplication to crash in the helper process.
    DWORD targetPid = 0;
    PWTS_PROCESS_INFO pProcessInfo = NULL;
    DWORD processCount = 0;
    if (WTSEnumerateProcesses(WTS_CURRENT_SERVER_HANDLE, 0, 1, &pProcessInfo, &processCount)) {
        for (DWORD i = 0; i < processCount; i++) {
            if (pProcessInfo[i].SessionId == sessionId &&
                pProcessInfo[i].pProcessName &&
                strcmp(pProcessInfo[i].pProcessName, "explorer.exe") == 0) {
                targetPid = pProcessInfo[i].ProcessId;
                break;
            }
        }
        WTSFreeMemory(pProcessInfo);
    }

    HANDLE hUserToken = NULL;
    if (targetPid != 0) {
        HANDLE hProc = OpenProcess(PROCESS_QUERY_INFORMATION, FALSE, targetPid);
        if (hProc) {
            if (!OpenProcessToken(hProc, TOKEN_DUPLICATE | TOKEN_QUERY, &hUserToken)) {
                qWarning() << "LaunchHelper: OpenProcessToken (user) failed, error:" << GetLastError();
                hUserToken = NULL;
            }
            CloseHandle(hProc);
        }
    }

    // Fallback: use service's own token (SYSTEM)
    if (!hUserToken) {
        qInfo() << "LaunchHelper: user process not found in session" << sessionId << "- using SYSTEM token";
        if (!OpenProcessToken(GetCurrentProcess(), TOKEN_DUPLICATE | TOKEN_QUERY, &hUserToken)) {
            qWarning() << "LaunchHelper: OpenProcessToken (SYSTEM) failed, error:" << GetLastError();
            return 0;
        }
    }

    HANDLE hDupToken = NULL;
    if (!DuplicateTokenEx(hUserToken, TOKEN_ALL_ACCESS, NULL, SecurityImpersonation, TokenPrimary, &hDupToken)) {
        qWarning() << "LaunchHelper: DuplicateTokenEx failed, error:" << GetLastError();
        CloseHandle(hUserToken);
        return 0;
    }
    CloseHandle(hUserToken);

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
        qInfo() << "LaunchHelper: helper process started, PID:" << pi.dwProcessId
                << "token: user (explorer.exe in session" << sessionId << ")";
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

        // 服务进程启用崩溃转储：main.cpp 的 platformMain 在 --service 分支提前
        // return，breakpad 不会初始化，崩溃时将无 .dmp 可分析。
        Breakpad::CrashHandler::instance()->Init(QCoreApplication::applicationDirPath());

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
                    // 不 stop()：helper 崩溃/断开后需要继续监视并自动重启。
                    // 原实现在此 stop()，导致 helper 崩溃后 timer 永久停止、
                    // 远程桌面无法恢复（服务只挂着不重建 helper）。
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

    // [P2] serviceMain 返回前必须显式报 SERVICE_STOPPED，否则 SCM 等到超时
    // 才认为服务已停（事件查看器记 7031/1053）。
    s_status.dwCurrentState = SERVICE_STOPPED;
    s_status.dwCheckPoint = 0;
    s_status.dwWaitHint = 0;
    SetServiceStatus(s_statusHandle, &s_status);
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
    // [B8] 宽字符 printf 格式必须用 %ls：%S 在 MSVC 宽格式里表示窄字符串，
    // 传入 wchar_t* 是 UB，服务安装路径会损坏（MinGW 下同样不可依赖）。
    swprintf(cmdLine, L"\"%ls\" --service", path);

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
        wprintf(L"Service '%ls' installed successfully.\n", SERVICE_NAME);
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

    // --service 分流绕过了 main() 的 QCommandLineParser，须从原始 argv 恢复日志级别
    // （g_logLevel 为全局，serviceMain 安装 handler 前即已生效）
    applyLogLevelFromArgs(argc, argv);

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
