#include "rdpserver.h"
#include "inputmanager.h"
#include "screencapturer.h"
#include "singleapplication.h"
#include "startup.h"
#include "systemsleepblocker.h"

#include <QCommandLineParser>
#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QGuiApplication>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMutexLocker>
#include <QSysInfo>
#include <QTextStream>
#include <QTimer>
#include <QWebSocket>

#ifdef _WIN32
#include <windows.h>
#include <winsvc.h>
#include <wtsapi32.h>
#endif

int g_logLevel = QtDebugMsg;

void logToFile(QtMsgType type, const QMessageLogContext& lg, const QString& msg)
{

#ifndef QT_DEBUG
    if (type < g_logLevel) {
        return;
    }
#endif

    // 加锁
    static QMutex mutex;
    QMutexLocker lock(&mutex);

    QString content;
    switch (type) {
    case QtDebugMsg:
        content = QString("%1").arg(msg);
        break;
    case QtInfoMsg:
        content = QString("%1").arg(msg);
        break;
    case QtWarningMsg:
        content = QString("%1").arg(msg);
        break;
    case QtCriticalMsg:
        content = QString("%1 [%2  %3]").arg(msg).arg(lg.file).arg(lg.line);
        break;
    case QtFatalMsg:
        content = QString("%1 [%2  %3]").arg(msg).arg(lg.file).arg(lg.line);
        break;
    default:
        content = QString("%1 [%2  %3]").arg(msg).arg(lg.file).arg(lg.line);
        break;
    }

    // 输出到控制台
    fprintf(stderr, "%s\n", content.toUtf8().constData());
    fflush(stderr);

    QDateTime dt = QDateTime::currentDateTime();

    // 防止QApplication 析构后的日志
    QString logFile = QString("%1/logs/%2.txt").arg(QDir::currentPath()).arg(dt.toString("yyyyMMdd"));

    QFile log(logFile);
    if (log.open(QIODevice::WriteOnly | QIODevice::Append | QFile::Text)) {
        QTextStream logStream(&log);
#if QT_VERSION < QT_VERSION_CHECK(6, 0, 0)
        logStream.setCodec("utf-8");
#endif

        QString tm = QDateTime::currentDateTime().toString("[yyyy/MM/dd hh:mm:ss.zzz] ");
        logStream << tm << content << "\n";
    }
}

#ifdef _WIN32
#define SERVICE_NAME L"QtRemoteDesktop"
static SERVICE_STATUS_HANDLE g_svcStatusHandle = NULL;
static SERVICE_STATUS g_svcStatus = { 0 };
static HANDLE g_svcStopEvent = NULL;

static void WINAPI ServiceCtrlHandler(DWORD ctrlCode)
{
    switch (ctrlCode) {
    case SERVICE_CONTROL_STOP:
    case SERVICE_CONTROL_SHUTDOWN:
        g_svcStatus.dwCurrentState = SERVICE_STOP_PENDING;
        SetServiceStatus(g_svcStatusHandle, &g_svcStatus);
        SetEvent(g_svcStopEvent);
        break;
    }
}

static bool LaunchHelperProcess()
{
    DWORD sessionId = WTSGetActiveConsoleSessionId();
    if (sessionId == 0xFFFFFFFF) {
        qInfo() << "LaunchHelper: no active console session";
        return false;
    }

    // 自身 SYSTEM token 复制，与 startSecureInputProcess 采用相同方式
    HANDLE hProcToken = NULL;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_DUPLICATE | TOKEN_QUERY, &hProcToken)) {
        qWarning() << "LaunchHelper: OpenProcessToken failed, error:" << GetLastError();
        return false;
    }

    HANDLE hDupToken = NULL;
    if (!DuplicateTokenEx(hProcToken, TOKEN_ALL_ACCESS, NULL, SecurityImpersonation, TokenPrimary, &hDupToken)) {
        qWarning() << "LaunchHelper: DuplicateTokenEx failed, error:" << GetLastError();
        CloseHandle(hProcToken);
        return false;
    }
    CloseHandle(hProcToken);

    // 设置目标会话 ID，使进程在用户会话中启动
    if (!SetTokenInformation(hDupToken, TokenSessionId, &sessionId, sizeof(sessionId))) {
        qWarning() << "LaunchHelper: SetTokenInformation failed, error:" << GetLastError();
        CloseHandle(hDupToken);
        return false;
    }

    wchar_t exePath[MAX_PATH];
    GetModuleFileNameW(NULL, exePath, MAX_PATH);
    std::wstring cmdLine = std::wstring(L"\"") + exePath + L"\" --helper";

    STARTUPINFOW si = { sizeof(si) };
    si.lpDesktop = const_cast<wchar_t*>(L"winsta0\\default");
    PROCESS_INFORMATION pi;
    bool ok = CreateProcessAsUserW(hDupToken, NULL, &cmdLine[0], NULL, NULL, FALSE, 0, NULL, NULL, &si, &pi);

    CloseHandle(hDupToken);

    if (ok) {
        qInfo() << "LaunchHelper: helper process started, PID:" << pi.dwProcessId;
        CloseHandle(pi.hProcess);
        CloseHandle(pi.hThread);
        return true;
    }
    qWarning() << "LaunchHelper: CreateProcessAsUser failed, error:" << GetLastError();
    return false;
}

static void WINAPI ServiceMain(DWORD argc, LPWSTR* argv)
{
    g_svcStatusHandle = RegisterServiceCtrlHandlerW(SERVICE_NAME, ServiceCtrlHandler);
    if (!g_svcStatusHandle)
        return;

    g_svcStatus.dwServiceType = SERVICE_WIN32_OWN_PROCESS;
    g_svcStatus.dwCurrentState = SERVICE_START_PENDING;
    g_svcStatus.dwControlsAccepted = SERVICE_ACCEPT_STOP | SERVICE_ACCEPT_SHUTDOWN;
    SetServiceStatus(g_svcStatusHandle, &g_svcStatus);

    g_svcStopEvent = CreateEventW(NULL, TRUE, FALSE, NULL);
    if (!g_svcStopEvent) {
        g_svcStatus.dwCurrentState = SERVICE_STOPPED;
        SetServiceStatus(g_svcStatusHandle, &g_svcStatus);
        return;
    }

#if QT_VERSION < QT_VERSION_CHECK(6, 0, 0)
    QCoreApplication::setAttribute(Qt::AA_EnableHighDpiScaling);
#endif

    {
        int qtArgc = 1;
        char* qtArgv[] = { const_cast<char*>("QtRemoteDesktop"), NULL };
        QCoreApplication app(qtArgc, qtArgv);

        g_svcStatus.dwCurrentState = SERVICE_RUNNING;
        SetServiceStatus(g_svcStatusHandle, &g_svcStatus);

        QString logDir = QString("%1/logs").arg(QCoreApplication::applicationDirPath());
        QDir().mkpath(logDir);
        qInstallMessageHandler(logToFile);

        RDPServer server;

        if (!server.initialize(QString(), true, true)) {
            g_svcStatus.dwCurrentState = SERVICE_STOPPED;
            SetServiceStatus(g_svcStatusHandle, &g_svcStatus);
        } else {
            server.start();

            // 定时检查停止事件
            QTimer tickTimer;
            QObject::connect(&tickTimer, &QTimer::timeout, [&]() {
                if (WaitForSingleObject(g_svcStopEvent, 0) == WAIT_OBJECT_0)
                    app.quit();
            });
            tickTimer.start(1000);

            // 重试拉起 helper，直到成功连上 /capture
            QTimer helperTimer;
            QObject::connect(&helperTimer, &QTimer::timeout, [&]() {
                if (server.isCaptureSourceConnected()) {
                    helperTimer.stop();
                } else {
                    LaunchHelperProcess();
                }
            });
            helperTimer.start(5000);
            LaunchHelperProcess();

            app.exec();
            tickTimer.stop();
        }
    }

    CloseHandle(g_svcStopEvent);
    g_svcStopEvent = NULL;
}

static BOOL InstallService()
{
    wchar_t path[MAX_PATH];
    GetModuleFileNameW(NULL, path, MAX_PATH);

    wchar_t cmdLine[MAX_PATH + 32];
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

static BOOL UninstallService()
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
    wprintf(L"Service '%s' removed.\n", SERVICE_NAME);
    return TRUE;
}
#endif

int main(int argc, char* argv[])
{
#ifdef _WIN32
    for (int i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "--install") == 0) {
            AllocConsole();
            freopen("CONOUT$", "w", stdout);
            freopen("CONOUT$", "w", stderr);
            BOOL admin = FALSE;
            PSID adminGroup = NULL;
            SID_IDENTIFIER_AUTHORITY ntAuth = SECURITY_NT_AUTHORITY;
            if (AllocateAndInitializeSid(&ntAuth, 2, SECURITY_BUILTIN_DOMAIN_RID, DOMAIN_ALIAS_RID_ADMINS, 0,0,0,0,0,0, &adminGroup)) {
                CheckTokenMembership(NULL, adminGroup, &admin);
                FreeSid(adminGroup);
            }
            if (!admin) {
                printf("错误: --install 需要管理员权限，请以管理员身份运行。\n");
                return 1;
            }
            return InstallService() ? 0 : 1;
        }
        if (strcmp(argv[i], "--uninstall") == 0) {
            AllocConsole();
            freopen("CONOUT$", "w", stdout);
            freopen("CONOUT$", "w", stderr);
            BOOL admin = FALSE;
            PSID adminGroup = NULL;
            SID_IDENTIFIER_AUTHORITY ntAuth = SECURITY_NT_AUTHORITY;
            if (AllocateAndInitializeSid(&ntAuth, 2, SECURITY_BUILTIN_DOMAIN_RID, DOMAIN_ALIAS_RID_ADMINS, 0,0,0,0,0,0, &adminGroup)) {
                CheckTokenMembership(NULL, adminGroup, &admin);
                FreeSid(adminGroup);
            }
            if (!admin) {
                printf("错误: --uninstall 需要管理员权限，请以管理员身份运行。\n");
                return 1;
            }
            return UninstallService() ? 0 : 1;
        }
        if (strcmp(argv[i], "--service") == 0) {
            SERVICE_TABLE_ENTRYW table[] = {
                { SERVICE_NAME, ServiceMain },
                { NULL, NULL }
            };
            if (!StartServiceCtrlDispatcherW(table)) {
                fprintf(stderr, "StartServiceCtrlDispatcherW failed: %lu\n", GetLastError());
            }
            return 0;
        }
    }
#endif

#ifdef _WIN32
    for (int i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "--secure-input") == 0) {
            QCoreApplication app(argc, argv);

            QDir::setCurrent(QCoreApplication::applicationDirPath());

            QString logDir = QString("%1/logs").arg(QCoreApplication::applicationDirPath());
            QDir().mkpath(logDir);
            qInstallMessageHandler(logToFile);

            int wsPort = 8081;
            if (i + 1 < argc) {
                wsPort = QString::fromLocal8Bit(argv[i + 1]).toInt();
                // skip the port arg if it's the next arg (from startSecureInputProcess)
            }

            QWebSocket ws;
            QObject::connect(&ws, &QWebSocket::connected, &app, [&]() {
                qInfo() << "SecureInput: connected to service";
            });
            QObject::connect(&ws, &QWebSocket::disconnected, &app, [&]() {
                qWarning() << "SecureInput: disconnected, exiting";
                QCoreApplication::quit();
            });
            QObject::connect(&ws, &QWebSocket::textMessageReceived, &app, [&](const QString& msg) {
                QJsonDocument doc = QJsonDocument::fromJson(msg.toUtf8());
                if (!doc.isObject()) return;
                QJsonObject obj = doc.object();
                QString type = obj["type"].toString();

                // isChar: 直接 Unicode 字符注入（用于锁屏密码输入）
                if (obj["isChar"].toBool() && obj["keycode"].toInt() > 0) {
                    wchar_t ch = static_cast<wchar_t>(obj["keycode"].toInt());
                    bool isDown = (type == "keydown");
                    INPUT in = {};
                    in.type = INPUT_KEYBOARD;
                    in.ki.dwFlags = KEYEVENTF_UNICODE;
                    in.ki.wScan = ch;
                    if (!isDown) in.ki.dwFlags |= KEYEVENTF_KEYUP;
                    SendInput(1, &in, sizeof(INPUT));
                    return;
                }

                static bool ctrlHeld = false, altHeld = false, shiftHeld = false;

#if defined(Q_OS_WIN)
                if (type == "keydown" || type == "keyup") {
                    int keycode = obj["keycode"].toInt();
                    bool isDown = (type == "keydown");
                    bool ctrl = obj["ctrl"].toBool();
                    bool alt = obj["alt"].toBool();
                    bool shift = obj["shift"].toBool();

                    auto sendVk = [](int vk, bool down) {
                        INPUT in = {};
                        in.type = INPUT_KEYBOARD;
                        in.ki.wVk = vk;
                        if (!down) in.ki.dwFlags = KEYEVENTF_KEYUP;
                        SendInput(1, &in, sizeof(INPUT));
                    };

                    if (ctrl != ctrlHeld) { sendVk(VK_CONTROL, ctrl); ctrlHeld = ctrl; }
                    if (alt != altHeld) { sendVk(VK_MENU, alt); altHeld = alt; }
                    if (shift != shiftHeld) { sendVk(VK_SHIFT, shift); shiftHeld = shift; }

                    sendVk(keycode, isDown);
                } else if (type == "mousemove") {
                    INPUT in = {};
                    in.type = INPUT_MOUSE;
                    in.mi.dx = obj["x"].toInt() * 65535 / GetSystemMetrics(SM_CXSCREEN);
                    in.mi.dy = obj["y"].toInt() * 65535 / GetSystemMetrics(SM_CYSCREEN);
                    in.mi.dwFlags = MOUSEEVENTF_MOVE | MOUSEEVENTF_ABSOLUTE;
                    SendInput(1, &in, sizeof(INPUT));
                } else if (type == "mousedown" || type == "mouseup") {
                    bool isDown = (type == "mousedown");
                    int x = obj["x"].toInt(), y = obj["y"].toInt();
                    int btn = obj["button"].toInt();
                    INPUT mv = {};
                    mv.type = INPUT_MOUSE;
                    mv.mi.dx = x * 65535 / GetSystemMetrics(SM_CXSCREEN);
                    mv.mi.dy = y * 65535 / GetSystemMetrics(SM_CYSCREEN);
                    mv.mi.dwFlags = MOUSEEVENTF_MOVE | MOUSEEVENTF_ABSOLUTE;
                    SendInput(1, &mv, sizeof(INPUT));
                    DWORD flags = 0;
                    if (btn == 0) flags = isDown ? MOUSEEVENTF_LEFTDOWN : MOUSEEVENTF_LEFTUP;
                    else if (btn == 1) flags = isDown ? MOUSEEVENTF_MIDDLEDOWN : MOUSEEVENTF_MIDDLEUP;
                    else if (btn == 2) flags = isDown ? MOUSEEVENTF_RIGHTDOWN : MOUSEEVENTF_RIGHTUP;
                    if (flags) { INPUT in = {}; in.type = INPUT_MOUSE; in.mi.dwFlags = flags; SendInput(1, &in, sizeof(INPUT)); }
                } else if (type == "wheel") {
                    INPUT in = {};
                    in.type = INPUT_MOUSE;
                    in.mi.mouseData = obj["delta"].toInt() * WHEEL_DELTA;
                    in.mi.dwFlags = MOUSEEVENTF_WHEEL;
                    SendInput(1, &in, sizeof(INPUT));
                }
#endif
            });
            ws.open(QUrl(QString("ws://127.0.0.1:%1/secure-input").arg(wsPort)));
            return app.exec();
        }
        if (strcmp(argv[i], "--helper") == 0) {
#if QT_VERSION < QT_VERSION_CHECK(6, 0, 0)
            QApplication::setAttribute(Qt::AA_EnableHighDpiScaling);
#endif
            QGuiApplication app(argc, argv);

            QDir::setCurrent(QGuiApplication::applicationDirPath());

            QString logDir = QString("%1/logs").arg(QGuiApplication::applicationDirPath());
            QDir().mkpath(logDir);
            qInstallMessageHandler(logToFile);

            // Read wsPort from server config
            int wsPort = 8081;
            QFile cfg(QGuiApplication::applicationDirPath() + "/server_config.json");
            if (cfg.open(QIODevice::ReadOnly)) {
                QJsonDocument doc = QJsonDocument::fromJson(cfg.readAll());
                cfg.close();
                if (doc.isObject()) {
                    QJsonObject root = doc.object();
                    int httpPort = root.value("httpPort").toInt(8080);
                    wsPort = httpPort + 1;
                }
            }

            QWebSocket ws;
            QObject::connect(&ws, &QWebSocket::connected, &app, [&]() {
                qInfo() << "Helper: connected to service WS successfully";
            });
            QObject::connect(&ws, QOverload<QAbstractSocket::SocketError>::of(&QWebSocket::error),
                &app, [&](QAbstractSocket::SocketError err) {
                    qWarning() << "Helper: WS error" << err << ws.errorString();
                });
            QObject::connect(&ws, &QWebSocket::disconnected, &app, [&]() {
                qWarning() << "Helper WS disconnected (helper may have crashed), retrying in 3s...";
                QTimer::singleShot(3000, [&]() {
                    ws.open(QUrl(QString("ws://127.0.0.1:%1/capture").arg(wsPort)));
                });
            });
            ws.open(QUrl(QString("ws://127.0.0.1:%1/capture").arg(wsPort)));

            ScreenCapturer capturer(nullptr);
            JpegCompressor compressor(nullptr);
            QObject::connect(&capturer, &ScreenCapturer::frameCaptured,
                &app, [&](const QImage& frame) { compressor.enqueue(frame); });
            QObject::connect(&compressor, &JpegCompressor::jpegCompressed,
                &ws, [&](const QByteArray& jpegData) {
                    if (ws.state() != QAbstractSocket::ConnectedState)
                        return;
                    QByteArray packet;
                    QDataStream stream(&packet, QIODevice::WriteOnly);
                    stream.setByteOrder(QDataStream::BigEndian);
                    stream << quint8(0x03);
                    stream << quint32(jpegData.size());
                    packet.append(jpegData);
                    ws.sendBinaryMessage(packet);
                }, Qt::QueuedConnection);
            // 桌面轮询检测锁屏 (Win7 GDI 不触发 screenLocked, 也兼容 Win8+)
            bool locked = false;
            bool isWin7 = false;
#ifdef _WIN32
            { struct { ULONG s; ULONG maj; ULONG min; ULONG bld; ULONG pid; WCHAR csd[128]; } osv = { sizeof(osv) };
              typedef LONG (WINAPI *R)(PVOID);
              HMODULE hNt = GetModuleHandleW(L"ntdll.dll");
              if (hNt) { R r = (R)GetProcAddress(hNt, "RtlGetVersion");
                if (r && r(&osv) == 0 && osv.maj == 6 && osv.min == 1) isWin7 = true; } }
#endif
            qInfo() << "Helper: starting desktop polling, isWin7 =" << isWin7;

            InputManager inputMgr;
            QObject::connect(&ws, &QWebSocket::textMessageReceived, &app,
                [&](const QString& msg) {
                    QJsonParseError err;
                    QJsonDocument doc = QJsonDocument::fromJson(msg.toUtf8(), &err);
                    if (err.error != QJsonParseError::NoError || !doc.isObject())
                        return;
                    QJsonObject obj = doc.object();
                    QString type = obj["type"].toString();

                    // 锁屏时 helper 的 SendInput 无法到达安全桌面，交给 secure-input 进程
                    if (locked) return;

                    if (type == "mousemove") {
                        inputMgr.injectMouseMove(obj["x"].toInt(), obj["y"].toInt());
                    } else if (type == "mousedown" || type == "mouseup") {
                        inputMgr.injectMouseButton(obj["x"].toInt(), obj["y"].toInt(),
                            obj["button"].toInt(), type == "mousedown");
                    } else if (type == "keydown" || type == "keyup") {
                        inputMgr.injectKeyboard(obj["keycode"].toInt(), obj["code"].toString(),
                            type == "keydown", obj["ctrl"].toBool(),
                            obj["alt"].toBool(), obj["shift"].toBool(),
                            isWin7 && locked);
                    } else if (type == "wheel") {
                        inputMgr.injectWheel(obj["delta"].toInt());
                    }
                });

            QObject::connect(&capturer, &ScreenCapturer::screenLocked, &app,
                [&](bool val) {
                    locked = val;
                    qInfo() << "Helper: screenLocked signal =" << locked;
                    QJsonObject msg;
                    msg["type"] = "screen_locked";
                    msg["locked"] = locked;
                    msg["hint"] = locked ? QString::fromUtf8("锁屏界面可直接输入密码") : QString();
                    ws.sendTextMessage(QString::fromUtf8(QJsonDocument(msg).toJson(QJsonDocument::Compact)));
                });

            QTimer* desktopCheckTimer = new QTimer(&app);
            QObject::connect(desktopCheckTimer, &QTimer::timeout, &app, [&]() {
                HDESK hDesk = OpenInputDesktop(0, FALSE, GENERIC_READ);
                if (!hDesk) {
                    if (!locked) {
                        locked = true;
                        qInfo() << "Helper: desktop locked (OpenInputDesktop failed)";
                        QJsonObject msg;
                        msg["type"] = "screen_locked";
                        msg["locked"] = true;
                        msg["hint"] = QString::fromUtf8("锁屏界面可直接输入密码");
                        ws.sendTextMessage(QString::fromUtf8(QJsonDocument(msg).toJson(QJsonDocument::Compact)));
                    }
                    return;
                }
                wchar_t name[256] = {};
                DWORD len = 0;
                bool isLocked = false;
                if (GetUserObjectInformationW(hDesk, UOI_NAME, name, sizeof(name), &len)) {
                    QString deskName = QString::fromWCharArray(name);
                    isLocked = (deskName.toLower() != QStringLiteral("default"));
                }
                CloseDesktop(hDesk);
                if (isLocked != locked) {
                    locked = isLocked;
                    qInfo() << "Helper: screen locked =" << locked;
                    QJsonObject msg;
                    msg["type"] = "screen_locked";
                    msg["locked"] = locked;
                    msg["hint"] = locked ? QString::fromUtf8("锁屏界面可直接输入密码") : QString();
                    ws.sendTextMessage(QString::fromUtf8(QJsonDocument(msg).toJson(QJsonDocument::Compact)));
                }
            });
            desktopCheckTimer->start(2000);

            compressor.start();
            if (!capturer.start(30)) {
                qCritical("Helper: failed to start screen capturer");
                return 1;
            }

            return app.exec();
        }
    }
#endif

#if QT_VERSION < QT_VERSION_CHECK(6, 0, 0)
    QApplication::setAttribute(Qt::AA_EnableHighDpiScaling);
#endif

    SingleApplication a(argc, argv);
    if (a.isRunning()) {
        return 0;
    }

    QCommandLineParser parser;
    parser.setApplicationDescription("Qt Remote Desktop Server");
    parser.addHelpOption();
    QCommandLineOption noSslOption("no-ssl", "Disable HTTPS/WSS (use plain HTTP/WS)");
    parser.addOption(noSslOption);
    QCommandLineOption consoleOption("console", "Show console window");
    parser.addOption(consoleOption);
    parser.process(a);

#ifdef _WIN32
    bool showConsole = parser.isSet(consoleOption);
    if (!showConsole) {
        QString configPath = a.applicationDirPath() + "/server_config.json";
        QFile configFile(configPath);
        if (configFile.exists() && configFile.open(QIODevice::ReadOnly)) {
            QJsonDocument doc = QJsonDocument::fromJson(configFile.readAll());
            configFile.close();
            if (doc.isObject()) {
                QJsonObject root = doc.object();
                if (root.contains("console")) {
                    showConsole = root["console"].toBool();
                }
            }
        }
    }
    if (showConsole) {
        AllocConsole();
        freopen("CONOUT$", "w", stdout);
        freopen("CONOUT$", "w", stderr);
        freopen("CONIN$", "r", stdin);
    }
#endif

    bool useSslOverride = !parser.isSet(noSslOption);
    // 创建日志文件夹
    QString path = QString("%1/%2").arg(a.applicationDirPath()).arg("logs");
    QDir dir(path);
    if (!dir.exists()) {
        dir.mkpath(path);
    }

    // 重定向日志
    qInstallMessageHandler(logToFile);
    // 开机自启动
    // StartUp::setup(true, QString(), QString(), QString(), QString(), true);

    SystemSleepBlocker blocker;
    if (!blocker.start()) {
        qCritical("Failed to prevent system sleep!");
    } else {
        qWarning("PREVENT system sleep...");
    }

    RDPServer server;
    if (!server.initialize(QString(), useSslOverride)) {
        return 1;
    }

    server.start();

    return a.exec();
}
