#include "windows_service.h"
#include "rdpserver.h"
#include "crashhandler.h"

#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMessageLogContext>
#include <QSslSocket>
#include <QStringList>
#include <QTimer>

#include <string>
#include <cstring>
#include <wchar.h>
#include <shellapi.h>
#include <winsvc.h>
#include <wtsapi32.h>

void logToFile(QtMsgType type, const QMessageLogContext& lg, const QString& msg);
void applyLogLevelFromArgs(int argc, char* argv[]);
// 安装目录 ACL 固化（定义在 install() 之后，见下方说明）
static bool ensureInstallDirAcl(QString* why);
// exe 所在目录（同文件下方定义）
static QString exeDirPath();

#define SERVICE_NAME L"QtRemoteDesktop"

// helper 启动后必须在此时限内连上 /capture，否则视为"活着但没用"并杀掉重建。
// 卡死/连错端口/WS 断开都会表现为"服务端永远等不到画面"，只判进程存活会永久黑屏。
static const qint64 kHelperConnectTimeoutMs = 20000;

SERVICE_STATUS_HANDLE WindowsService::s_statusHandle = NULL;
SERVICE_STATUS WindowsService::s_status = {};
HANDLE WindowsService::s_stopEvent = NULL;
HANDLE WindowsService::s_sessionChangeEvent = NULL;

DWORD WINAPI WindowsService::serviceCtrlHandler(DWORD ctrlCode, DWORD eventType,
                                               LPVOID eventData, LPVOID context)
{
    Q_UNUSED(eventData);
    Q_UNUSED(context);
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
        //
        // [FIX-B9] 只对"会话归属可能变化"的事件重建。锁屏/解锁同样会发
        // SESSIONCHANGE（WTS_SESSION_LOCK/UNLOCK），若不过滤会在每次锁屏时
        // 白白杀掉重建 helper（黑屏 churn），而锁屏本身不需要换会话。
        switch (eventType) {
        case WTS_CONSOLE_CONNECT:
        case WTS_CONSOLE_DISCONNECT:
        case WTS_REMOTE_CONNECT:
        case WTS_REMOTE_DISCONNECT:
        case WTS_SESSION_LOGON:
        case WTS_SESSION_LOGOFF:
            if (s_sessionChangeEvent)
                SetEvent(s_sessionChangeEvent);
            break;
        default:
            break;
        }
        break;
    default:
        break;
    }
    return NO_ERROR;
}

DWORD WindowsService::launchHelperProcess(HANDLE* outProcess)
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
        if (outProcess)
            *outProcess = pi.hProcess;   // 生命周期交给调用方
        else
            CloseHandle(pi.hProcess);
        return pi.dwProcessId;
    }
    qWarning() << "LaunchHelper: CreateProcessAsUser failed, error:" << GetLastError();
    return 0;
}

void WINAPI WindowsService::serviceMain(DWORD argc, LPWSTR* argv)
{
    s_statusHandle = RegisterServiceCtrlHandlerExW(SERVICE_NAME, serviceCtrlHandler, nullptr);
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

        // 升级路径同样要保证安装目录 ACL：现场升级是「换 exe + 启服务」，不会跑
        // --install。这里在每次启动时幂等校正一次；失败只记日志、不阻断启动。
        {
            QString why;
            if (!ensureInstallDirAcl(&why))
                qWarning() << "Service: install dir ACL not applied:" << why;
        }

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
            HANDLE helperProc = NULL;     // helper 进程句柄：判活/终止都用它，避免 PID 复用误杀
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
                        if (helperProc) {
                            TerminateProcess(helperProc, 1);
                            CloseHandle(helperProc);
                            helperProc = NULL;
                        } else {
                            HANDLE hKill = OpenProcess(PROCESS_TERMINATE, FALSE, helperPid);
                            if (hKill) { TerminateProcess(hKill, 1); CloseHandle(hKill); }
                        }
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
                    // 有句柄时对句柄判活：既可靠又不会因 PID 复用把无关进程当成 helper。
                    bool alive = false;
                    if (helperProc) {
                        alive = (WaitForSingleObject(helperProc, 0) == WAIT_TIMEOUT);
                    } else {
                        HANDLE hProc = OpenProcess(SYNCHRONIZE | PROCESS_QUERY_INFORMATION,
                                                   FALSE, helperPid);
                        if (hProc) {
                            alive = (WaitForSingleObject(hProc, 0) == WAIT_TIMEOUT);
                            CloseHandle(hProc);
                        }
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
                        if (helperProc) {
                            TerminateProcess(helperProc, 1);
                        } else {
                            HANDLE hKill = OpenProcess(PROCESS_TERMINATE, FALSE, helperPid);
                            if (hKill) { TerminateProcess(hKill, 1); CloseHandle(hKill); }
                        }
                    }
                    if (helperProc) {
                        CloseHandle(helperProc);
                        helperProc = NULL;
                    }
                    helperPid = 0;
                    lastCaptureMs = 0;
                }
                DWORD pid = launchHelperProcess(&helperProc);
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
            helperPid = launchHelperProcess(&helperProc);
            helperSpawnMs = QDateTime::currentMSecsSinceEpoch();

            app.exec();
            tickTimer.stop();

            // 服务停止：必须回收 helper，否则它在用户会话里成为孤儿进程，
            // 继续抓屏/占端口，下次启动还会与新 helper 抢 /capture。
            if (helperPid != 0) {
                qInfo() << "Service stopping, terminating helper PID" << helperPid;
                if (helperProc) {
                    TerminateProcess(helperProc, 1);
                    CloseHandle(helperProc);
                    helperProc = NULL;
                } else {
                    HANDLE hKill = OpenProcess(PROCESS_TERMINATE, FALSE, helperPid);
                    if (hKill) { TerminateProcess(hKill, 1); CloseHandle(hKill); }
                }
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

// 服务真正会用的 scheme/端口，判定与 RDPServer::loadServerConfig 一致：
//   配置文件缺失 → 默认 ssl=true、端口 8080；
//   目标机没有可用的 TLS 运行库 → 服务端自己降级成明文。
// 装完打印错协议等于把用户引向一次"连得上但没有页面、且看不出原因"的失败，
// 所以这里宁可把服务端的判定重算一遍，而不是照抄 README 里的 http://。
static void resolveListenEndpoint(bool* useSsl, quint16* port)
{
    bool ssl = true;
    int httpPort = 8080;

    QFile file(exeDirPath() + QStringLiteral("/server_config.json"));
    if (file.open(QIODevice::ReadOnly)) {
        const QJsonObject root = QJsonDocument::fromJson(file.readAll()).object();
        if (root.contains(QStringLiteral("ssl")))
            ssl = root.value(QStringLiteral("ssl")).toBool();
        if (root.contains(QStringLiteral("httpPort")))
            httpPort = root.value(QStringLiteral("httpPort")).toInt();
    }
    if (ssl && !QSslSocket::supportsSsl())
        ssl = false;

    *useSsl = ssl;
    *port = (httpPort > 0 && httpPort < 65536) ? static_cast<quint16>(httpPort) : 8080;
}

static void printAccessUrls()
{
    bool useSsl = false;
    quint16 port = 8080;
    resolveListenEndpoint(&useSsl, &port);

    const std::wstring scheme = (useSsl ? QStringLiteral("https") : QStringLiteral("http")).toStdWString();
    wprintf(L"\n");
    wprintf(L"Open in a browser (default account: admin / admin):\n");
    const QStringList ips = RDPServer::getLocalIpAddr();
    for (const QString& ip : ips) {
        const std::wstring wip = ip.toStdWString();
        wprintf(L"    %ls://%ls:%u\n", scheme.c_str(), wip.c_str(), static_cast<unsigned>(port));
    }
    if (useSsl)
        wprintf(L"  Self-signed certificate: the browser warning is expected, choose \"Continue\".\n");
}

// 已注册服务的配置对不上本次安装时改写。查询/改写失败都只告警：路径本来就一致的
// 占多数，剩下的是管理员手工调过服务配置的情形，覆盖它反而不对。
static void syncExistingServiceConfig(SC_HANDLE svc, const wchar_t* cmdLine)
{
    std::wstring buf;
    buf.resize(4096);
    DWORD needed = 0;
    QUERY_SERVICE_CONFIGW* cfg = reinterpret_cast<QUERY_SERVICE_CONFIGW*>(&buf[0]);
    if (!QueryServiceConfigW(svc, cfg, static_cast<DWORD>(buf.size() * sizeof(wchar_t)), &needed)) {
        wprintf(L"  warning: QueryServiceConfigW failed: %lu (leaving the service as is)\n",
                GetLastError());
        return;
    }

    const bool samePath = cfg->lpBinaryPathName && _wcsicmp(cfg->lpBinaryPathName, cmdLine) == 0;
    const bool autoStart = cfg->dwStartType == SERVICE_AUTO_START;
    if (samePath && autoStart) {
        wprintf(L"  existing service already points at this executable; nothing to change.\n");
        return;
    }

    if (!ChangeServiceConfigW(svc, SERVICE_WIN32_OWN_PROCESS, SERVICE_AUTO_START,
                              SERVICE_ERROR_NORMAL, cmdLine, NULL, NULL, NULL, NULL, NULL, NULL)) {
        wprintf(L"  warning: ChangeServiceConfigW failed: %lu (service keeps its old configuration)\n",
                GetLastError());
        return;
    }

    if (!samePath) {
        const std::wstring wcmd = QString::fromWCharArray(cmdLine).toStdWString();
        wprintf(L"  binary path updated to: %ls\n", wcmd.c_str());
    }
    if (!autoStart)
        wprintf(L"  start type set to automatic.\n");
}

// 注册只是"下次开机自启"，装完那一刻并没有任何进程在监听；这里真正把它拉起来并等到
// RUNNING，失败时把原因（含"起来就退"这种）说清楚。
static bool startInstalledService(QString* why)
{
    SC_HANDLE scm = OpenSCManagerW(NULL, NULL, SC_MANAGER_CONNECT);
    if (!scm) {
        *why = QStringLiteral("OpenSCManagerW failed: %1").arg(GetLastError());
        return false;
    }
    SC_HANDLE svc = OpenServiceW(scm, SERVICE_NAME, SERVICE_START | SERVICE_QUERY_STATUS);
    if (!svc) {
        *why = QStringLiteral("OpenServiceW failed: %1").arg(GetLastError());
        CloseServiceHandle(scm);
        return false;
    }

    bool running = false;
    SERVICE_STATUS ss;
    ZeroMemory(&ss, sizeof(ss));
    if (QueryServiceStatus(svc, &ss) && ss.dwCurrentState == SERVICE_RUNNING) {
        running = true;
    } else if (!StartServiceW(svc, 0, NULL)) {
        const DWORD err = GetLastError();
        if (err == ERROR_SERVICE_ALREADY_RUNNING)
            running = true;
        else
            *why = QStringLiteral("StartServiceW failed: %1").arg(err);
    } else {
        // 最多等 20s：服务初始化要建采集/编码上下文，START_PENDING 期间不能判失败。
        for (int i = 0; i < 100; ++i) {
            if (!QueryServiceStatus(svc, &ss)) {
                *why = QStringLiteral("QueryServiceStatus failed: %1").arg(GetLastError());
                break;
            }
            if (ss.dwCurrentState == SERVICE_RUNNING) {
                running = true;
                break;
            }
            if (ss.dwCurrentState == SERVICE_STOPPED) {
                *why = QStringLiteral("the service stopped immediately after starting "
                                      "(wrong Qt runtime/PE bitness, or port 8080 already busy)");
                break;
            }
            Sleep(200);
        }
        if (!running && why->isEmpty())
            *why = QStringLiteral("service did not reach RUNNING within 20s (last state=%1)")
                       .arg(ss.dwCurrentState);
    }

    CloseServiceHandle(svc);
    CloseServiceHandle(scm);
    return running;
}

// 本次安装等价的手工命令。提权分支里由父进程先打一遍：子进程有它自己的控制台窗口，
// 成功时一闪而过，用户必须在自己的终端里看得见"接下来跑什么"。
static void printInstallPlan(const wchar_t* cmdLine)
{
    wprintf(L"[1/2] Install the service (auto-start at boot). Equivalent command:\n");
    wprintf(L"      sc create %ls binPath= \"%ls\" start= auto\n", SERVICE_NAME, cmdLine);
    wprintf(L"[2/2] Start it now. Equivalent command:\n");
    wprintf(L"      net start %ls\n\n", SERVICE_NAME);
}

// ---------------------------------------------------------------------------
// 安装/卸载的公共前置：权限自查 + 提权重跑
//
// 「只注册不启动」+「失败只打印一行错误码」是现场"install 没报错但页面打不开"的直接
// 来源：CreateServiceW 写的是开机自启，装完那一刻没有任何进程在监听 8080。现在把整条
// 链补全：权限不够 → 用同参数把自己提权重跑；注册完 → 立刻启动到 RUNNING；
// 启动完 → 打印真正可访问的地址。
// ---------------------------------------------------------------------------
bool WindowsService::relaunchElevatedIfNeeded(int argc, char* argv[], int* outExitCode)
{
    *outExitCode = 0;
    if (isAdmin())
        return false;

    wchar_t exePath[MAX_PATH] = { 0 };
    if (!GetModuleFileNameW(NULL, exePath, MAX_PATH)) {
        wprintf(L"GetModuleFileNameW failed: %lu\n", GetLastError());
        *outExitCode = 1;
        return true;
    }

    // 原参数原样带给提权后的自身，附加选项（--log-level 之类）不会在提权这一步丢掉。
    QString params;
    for (int i = 1; i < argc; ++i) {
        const QString a = QString::fromLocal8Bit(argv[i]);
        if (a.isEmpty())
            continue;
        params += a.contains(QLatin1Char(' ')) ? QLatin1Char('"') + a + QLatin1Char('"') : a;
        params += QLatin1Char(' ');
    }
    std::wstring wparams = params.toStdWString();
    const std::wstring wexe = QString::fromWCharArray(exePath).toStdWString();

    // 安装类操作先把计划打印在本终端，再去提权窗口里执行（否则用户看不见要跑什么）。
    for (int i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "--install") == 0) {
            wchar_t cmdLine[MAX_PATH + 32];
            swprintf(cmdLine, L"\"%ls\" --service", exePath);
            printInstallPlan(cmdLine);
            break;
        }
    }

    wprintf(L"This operation needs administrator privileges; the current shell is not elevated.\n");
    wprintf(L"Re-running the same command elevated - accept the UAC prompt:\n");
    wprintf(L"    \"%ls\" %ls\n\n", wexe.c_str(), wparams.c_str());

    SHELLEXECUTEINFOW sei;
    ZeroMemory(&sei, sizeof(sei));
    sei.cbSize = sizeof(sei);
    sei.fMask = SEE_MASK_NOCLOSEPROCESS;
    sei.lpVerb = L"runas";
    sei.lpFile = exePath;
    sei.lpParameters = wparams.empty() ? NULL : &wparams[0];
    sei.nShow = SW_SHOWNORMAL;
    if (!ShellExecuteExW(&sei) || !sei.hProcess) {
        const DWORD err = GetLastError();
        if (err == ERROR_CANCELLED)
            wprintf(L"UAC prompt declined: nothing was changed.\n");
        else
            wprintf(L"ShellExecuteExW(runas) failed: %lu\n", err);
        *outExitCode = 1;
        return true;
    }

    WaitForSingleObject(sei.hProcess, 5 * 60 * 1000);
    DWORD code = 1;
    GetExitCodeProcess(sei.hProcess, &code);
    CloseHandle(sei.hProcess);
    if (code != 0) {
        *outExitCode = 1;
        wprintf(L"Elevated run failed (exit code %lu); see the messages in that window.\n", code);
        return true;
    }
    *outExitCode = 0;

    // 提权子进程有它自己的控制台窗口，成功时一闪而过；这里在本终端复述结论，
    // 免得用户只看到黑框闪现，还得猜到底装没装好。
    for (int i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "--install") == 0) {
            wprintf(L"Service '%ls' installed and started.\n", SERVICE_NAME);
            printAccessUrls();
            break;
        }
    }
    return true;
}

bool WindowsService::install()
{
    wchar_t path[MAX_PATH];
    GetModuleFileNameW(NULL, path, MAX_PATH);

    // 安装目录 ACL 固化（部署步骤）：Win7 实测 C:\bin 只授权给安装者/Administrators/
    // SYSTEM，非管理员账户登录后 helper 以 user-token 运行，加载 Qt DLL 失败
    // （0xC0000135），桌面起不来。这里把「Users 可读可执行安装目录」写进安装流程，
    // 失败只告警、不阻断安装。详见 ensureInstallDirAcl()。
    {
        QString why;
        if (!ensureInstallDirAcl(&why))
            wprintf(L"Warning: install dir ACL not applied: %ls\n", why.toStdWString().c_str());
    }

    wchar_t cmdLine[MAX_PATH + 32];
    // [B8] 宽字符 printf 格式必须用 %ls：%S 在 MSVC 宽格式里表示窄字符串，
    // 传入 wchar_t* 是 UB，服务安装路径会损坏（MinGW 下同样不可依赖）。
    swprintf(cmdLine, L"\"%ls\" --service", path);

    // 先声明再执行：授权受限、只能把命令转给管理员代跑的环境需要能对照，
    // 出问题时这些命令也是逐条复现的依据（本程序走同一套 SCM API，不落 sc.exe）。
    printInstallPlan(cmdLine);

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
    } else {
        const DWORD err = GetLastError();
        if (err != ERROR_SERVICE_EXISTS) {
            wprintf(L"CreateServiceW failed: %lu\n", err);
            CloseServiceHandle(scm);
            return FALSE;
        }
        // 已存在：换目录重装（升级最常见的形态）时 ImagePath 还指着旧 exe，
        // 旧目录一删服务永久起不来，而用户看到的仍然是"装了但打不开"。
        wprintf(L"Service already registered; reconciling its configuration...\n");
        svc = OpenServiceW(scm, SERVICE_NAME,
                           SERVICE_START | SERVICE_QUERY_STATUS |
                           SERVICE_QUERY_CONFIG | SERVICE_CHANGE_CONFIG);
        if (!svc) {
            wprintf(L"OpenServiceW failed: %lu\n", GetLastError());
            CloseServiceHandle(scm);
            return FALSE;
        }
        syncExistingServiceConfig(svc, cmdLine);
    }
    CloseServiceHandle(svc);
    CloseServiceHandle(scm);

    QString why;
    if (!startInstalledService(&why)) {
        const std::wstring wwhy = why.toStdWString();
        wprintf(L"Service is registered but FAILED to start: %ls\n", wwhy.c_str());
        wprintf(L"Check <install dir>\\logs\\, or start it by hand: net start %ls\n", SERVICE_NAME);
        return FALSE;
    }

    wprintf(L"Service '%ls' is running.\n", SERVICE_NAME);
    printAccessUrls();
    return TRUE;
}

// ---------------------------------------------------------------------------
// 安装目录 ACL 固化（安装 / 升级路径，幂等）
//
// 背景（2026-11 Win7 实测）：C:\bin 只授权给 安装者/Administrators/SYSTEM。非管理员
// 账户登录后，服务用该用户令牌拉起的 helper 加载 C:\bin 下的 Qt DLL 失败
// （0xC0000135），桌面起不来；手工补一条 BUILTIN\Users:(OI)(CI)(RX) 才恢复。
//
// 固化规则：
//   · 用 SID（*S-1-5-32-545）而不是本地化组名，英文/中文/其它语言系统一致；
//   · 只授予安装目录（exe/DLL/资源所在的层级）：Users 读+执行、可遍历；
//     ACL 是集合，重复 grant 同一条不会叠加 —— 幂等；
//   · logs/crashdumps/bearer 属于数据目录（日志可能含敏感信息），不跟着继承放宽：
//     显式去掉继承、只保留 管理员 + SYSTEM 的完全控制；
//   · 任何一步失败只返回原因给调用方记日志，绝不阻断安装/启动；
//     XP 没有 icacls，CreateProcess 直接失败 → 同样只记日志。
// ---------------------------------------------------------------------------
static QString exeDirPath()
{
    wchar_t path[MAX_PATH] = { 0 };
    if (!GetModuleFileNameW(NULL, path, MAX_PATH))
        return QString();
    const QString full = QString::fromWCharArray(path);
    const int slash = full.lastIndexOf(QLatin1Char('\\'));
    return slash > 1 ? full.left(slash) : QString();
}

static bool runIcacls(const QString& args, int* exitCode)
{
    const QString cmd = QStringLiteral("icacls.exe ") + args;
    std::wstring wcmd = cmd.toStdWString();
    STARTUPINFOW si;
    ZeroMemory(&si, sizeof(si));
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;
    PROCESS_INFORMATION pi;
    ZeroMemory(&pi, sizeof(pi));
    if (!CreateProcessW(nullptr, const_cast<LPWSTR>(wcmd.c_str()), nullptr, nullptr, FALSE,
                        CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) {
        *exitCode = -1;
        return false;
    }
    WaitForSingleObject(pi.hProcess, 20000);
    DWORD code = 1;
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    *exitCode = static_cast<int>(code);
    return true;
}

static bool ensureInstallDirAcl(QString* why)
{
    // XP 没有 icacls（其 C:\bin 也是普通目录 ACL，无需固化）：跳过，不刷错误日志。
    const QString icaclsPath = QString::fromLocal8Bit(qgetenv("SystemRoot"))
                               + QStringLiteral("/System32/icacls.exe");
    if (!QFile::exists(icaclsPath))
        return true;

    const QString dir = exeDirPath();
    if (dir.isEmpty()) {
        *why = QStringLiteral("cannot resolve install dir");
        return false;
    }
    int rc = 0;
    if (!runIcacls(QStringLiteral("\"%1\" /grant *S-1-5-32-545:(OI)(CI)(RX)").arg(dir), &rc)) {
        *why = QStringLiteral("cannot start icacls");
        return false;
    }
    if (rc != 0) {
        *why = QStringLiteral("icacls grant on %1 failed rc=%2").arg(dir).arg(rc);
        return false;
    }
    static const char* const kSensitiveDirs[] = { "logs", "crashdumps", "bearer" };
    for (const char* sub : kSensitiveDirs) {
        const QString p = dir + QLatin1Char('\\') + QLatin1String(sub);
        if (!QDir(p).exists())
            continue;
        int rc2 = 0;
        // 去掉从安装目录继承来的 Users 读权限，只留 管理员 + SYSTEM。
        if (!runIcacls(QStringLiteral("\"%1\" /inheritance:r /grant:r "
                                      "*S-1-5-32-544:(OI)(CI)(F) *S-1-5-18:(OI)(CI)(F)")
                           .arg(p),
                       &rc2)
            || rc2 != 0) {
            *why = QStringLiteral("icacls harden %1 failed rc=%2").arg(p).arg(rc2);
            return false;
        }
    }
    return true;
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
