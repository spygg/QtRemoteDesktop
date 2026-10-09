#include "windows_service.h"
#include "rdpserver.h"
#include "crashhandler.h"

#include <QCoreApplication>
#include <QDateTime>
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

// helper 启动后必须在此时限内连上 /capture，否则视为"活着但没用"并杀掉重建。
// 卡死/连错端口/WS 断开都会表现为"服务端永远等不到画面"，只判进程存活会永久黑屏。
static const qint64 kHelperConnectTimeoutMs = 20000;

SERVICE_STATUS_HANDLE WindowsService::s_statusHandle = NULL;
SERVICE_STATUS WindowsService::s_status = {};
HANDLE WindowsService::s_stopEvent = NULL;
HANDLE WindowsService::s_sessionChangeEvent = NULL;

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
    case SERVICE_CONTROL_SESSIONCHANGE:
        // 用户切换/注销/新登录：当前 helper 属于旧会话，必须重建（否则新会话
        // 一直是黑屏，而旧 helper 还"已连接"）。这里只置事件，实际重建交给
        // helper 定时器（服务控制线程里不能做重活）。
        if (s_sessionChangeEvent)
            SetEvent(s_sessionChangeEvent);
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
    PROCESS_INFORMATION pi = {};
    typedef BOOL (WINAPI *CPAUserW_t)(HANDLE, LPCWSTR, LPWSTR,
        LPSECURITY_ATTRIBUTES, LPSECURITY_ATTRIBUTES, BOOL, DWORD,
        LPVOID, LPCWSTR, LPSTARTUPINFOW, LPPROCESS_INFORMATION);
    CPAUserW_t pCreateProcessAsUserW = (CPAUserW_t)GetProcAddress(
        GetModuleHandleA("advapi32"), "CreateProcessAsUserW");

    // 为桌面用户构造环境块：lpEnvironment=NULL 会让 helper 继承**服务进程(SYSTEM)**
    // 的 environ，其 TEMP/APPDATA/USERPROFILE 全部指向 systemprofile，helper 写临时
    // 文件/读用户配置都会跑到系统账户目录下。userenv.dll 动态加载，避免新增链接依赖。
    typedef BOOL (WINAPI *CreateEnvironmentBlock_t)(LPVOID*, HANDLE, BOOL);
    typedef BOOL (WINAPI *DestroyEnvironmentBlock_t)(LPVOID);
    LPVOID envBlock = NULL;
    HMODULE hUserEnv = LoadLibraryA("userenv.dll");
    CreateEnvironmentBlock_t pCreateEnv = NULL;
    DestroyEnvironmentBlock_t pDestroyEnv = NULL;
    if (hUserEnv) {
        pCreateEnv = (CreateEnvironmentBlock_t)GetProcAddress(hUserEnv, "CreateEnvironmentBlock");
        pDestroyEnv = (DestroyEnvironmentBlock_t)GetProcAddress(hUserEnv, "DestroyEnvironmentBlock");
        if (pCreateEnv && !pCreateEnv(&envBlock, hDupToken, FALSE))
            envBlock = NULL;   // 失败就退回继承环境，不阻断启动
    }

    bool ok = pCreateProcessAsUserW && pCreateProcessAsUserW(
        hDupToken, NULL, &cmdLine[0], NULL, NULL, FALSE,
        envBlock ? CREATE_UNICODE_ENVIRONMENT : 0,
        envBlock, NULL, &si, &pi);

    if (envBlock && pDestroyEnv)
        pDestroyEnv(envBlock);
    if (hUserEnv)
        FreeLibrary(hUserEnv);

    CloseHandle(hDupToken);

    if (ok) {
        qInfo() << "LaunchHelper: helper process started, PID:" << pi.dwProcessId
                << "token: user (explorer.exe in session" << sessionId << ")"
                << "env:" << (envBlock ? "user" : "inherited(SYSTEM)");
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
    s_status.dwControlsAccepted = SERVICE_ACCEPT_STOP | SERVICE_ACCEPT_SHUTDOWN
                                  | SERVICE_ACCEPT_SESSIONCHANGE;
    SetServiceStatus(s_statusHandle, &s_status);

    s_stopEvent = CreateEventW(NULL, TRUE, FALSE, NULL);
    s_sessionChangeEvent = CreateEventW(NULL, TRUE, FALSE, NULL);
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
            qint64 helperSpawnMs = 0;     // helper 最近一次启动时刻
            qint64 lastCaptureMs = 0;     // 最近一次观察到 /capture 已连接的时刻
            QObject::connect(&helperTimer, &QTimer::timeout, [&]() {
                const qint64 now = QDateTime::currentMSecsSinceEpoch();

                // 会话切换/注销：旧 helper 属于旧会话，先收掉，让下面立刻为新会话重建
                if (s_sessionChangeEvent
                    && WaitForSingleObject(s_sessionChangeEvent, 0) == WAIT_OBJECT_0) {
                    ResetEvent(s_sessionChangeEvent);
                    if (helperPid != 0) {
                        qInfo() << "Session change detected, terminating helper PID" << helperPid;
                        HANDLE hKill = OpenProcess(PROCESS_TERMINATE, FALSE, helperPid);
                        if (hKill) { TerminateProcess(hKill, 1); CloseHandle(hKill); }
                        helperPid = 0;
                    }
                    helperTimer.setInterval(1000);
                    return;
                }

                if (server.isCaptureSourceConnected()) {
                    // 不 stop()：helper 崩溃/断开后需要继续监视并自动重启。
                    lastCaptureMs = now;
                    return;
                }

                if (helperPid != 0) {
                    // 用 WaitForSingleObject 判活，比 GetExitCodeProcess==STILL_ACTIVE
                    // 可靠（PID 复用/句柄失效都不会误判为"还在跑"）。
                    bool alive = false;
                    HANDLE hProc = OpenProcess(SYNCHRONIZE | PROCESS_QUERY_INFORMATION,
                                               FALSE, helperPid);
                    if (hProc) {
                        alive = (WaitForSingleObject(hProc, 0) == WAIT_TIMEOUT);
                        CloseHandle(hProc);
                    }
                    if (alive) {
                        // 关键：进程活着 != 健康。helper 卡死 / WS 断开 / 连错端口时
                        // 服务端永远等不到 /capture，只判"进程存活"会永久黑屏。
                        // 以"最近一次成功连接 /capture 的时刻"为健康判据，超时就杀掉重建。
                        const qint64 lastOk = (lastCaptureMs > 0) ? lastCaptureMs : helperSpawnMs;
                        if (now - lastOk < kHelperConnectTimeoutMs)
                            return;   // 宽限期内，等它连上
                        qWarning() << "Helper PID" << helperPid << "alive but /capture not"
                                   << "connected for" << (now - lastOk) << "ms - terminating";
                        HANDLE hKill = OpenProcess(PROCESS_TERMINATE, FALSE, helperPid);
                        if (hKill) { TerminateProcess(hKill, 1); CloseHandle(hKill); }
                    }
                    helperPid = 0;
                    lastCaptureMs = 0;
                }
                DWORD pid = launchHelperProcess();
                if (pid != 0) {
                    helperPid = pid;
                    helperSpawnMs = now;
                    helperTimer.setInterval(5000);
                } else {
                    // 无用户会话时慢速轮询，有会话但启动失败时快速重试
                    bool hasSession = (WTSGetActiveConsoleSessionId() != 0xFFFFFFFF);
                    helperTimer.setInterval(hasSession ? 1000 : 5000);
                }
            });
            helperTimer.start(5000);
            helperPid = launchHelperProcess();
            helperSpawnMs = QDateTime::currentMSecsSinceEpoch();

            app.exec();
            tickTimer.stop();

            // 服务停止：必须回收 helper，否则它在用户会话里成为孤儿进程，
            // 继续抓屏/占端口，下次启动还会与新 helper 抢 /capture。
            if (helperPid != 0) {
                qInfo() << "Service stopping, terminating helper PID" << helperPid;
                HANDLE hKill = OpenProcess(PROCESS_TERMINATE, FALSE, helperPid);
                if (hKill) { TerminateProcess(hKill, 1); CloseHandle(hKill); }
            }
        }
    }

    CloseHandle(s_stopEvent);
    s_stopEvent = NULL;
    if (s_sessionChangeEvent) {
        CloseHandle(s_sessionChangeEvent);
        s_sessionChangeEvent = NULL;
    }

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
    // [B8] 宽字符 printf 格式必须用 %ls：%S 在 MSVC 宽格式里表示窄字符串，
    // 传入 wchar_t* 是 UB（MinGW 下依赖实现）。
    wprintf(L"Service '%ls' removed.\n", SERVICE_NAME);
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
