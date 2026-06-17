#include "rdpserver.h"
#include "singleapplication.h"
#include "startup.h"
#include "systemsleepblocker.h"

#include <QApplication>
#include <QCommandLineParser>
#include <QDir>
#include <QMutexLocker>
#include <QTextStream>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTimer>

#ifdef _WIN32
#include <windows.h>
#include <winsvc.h>
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

    int qtArgc = 1;
    QByteArray appName = QCoreApplication::applicationFilePath().toUtf8();
    char* qtArgv[] = { appName.data(), NULL };
    QCoreApplication app(qtArgc, qtArgv);

    QString logDir = QString("%1/logs").arg(QCoreApplication::applicationDirPath());
    QDir().mkpath(logDir);
    qInstallMessageHandler(logToFile);

    SystemSleepBlocker blocker;
    blocker.start();

    RDPServer server;
    if (!server.initialize(QString(), true)) {
        g_svcStatus.dwCurrentState = SERVICE_STOPPED;
        SetServiceStatus(g_svcStatusHandle, &g_svcStatus);
        CloseHandle(g_svcStopEvent);
        g_svcStopEvent = NULL;
        return;
    }
    server.start();

    g_svcStatus.dwCurrentState = SERVICE_RUNNING;
    SetServiceStatus(g_svcStatusHandle, &g_svcStatus);

    QTimer timer;
    QObject::connect(&timer, &QTimer::timeout, [&]() {
        if (WaitForSingleObject(g_svcStopEvent, 0) == WAIT_OBJECT_0)
            app.quit();
    });
    timer.start(500);

    app.exec();

    timer.stop();
    g_svcStatus.dwCurrentState = SERVICE_STOPPED;
    SetServiceStatus(g_svcStatusHandle, &g_svcStatus);
    CloseHandle(g_svcStopEvent);
    g_svcStopEvent = NULL;
}

static BOOL InstallService()
{
    wchar_t path[MAX_PATH];
    GetModuleFileNameW(NULL, path, MAX_PATH);

    wchar_t cmdLine[MAX_PATH + 32];
    swprintf(cmdLine, L"\"%s\" --service", path);

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
        wprintf(L"Service '%s' installed successfully.\n", SERVICE_NAME);
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
            return InstallService() ? 0 : 1;
        }
        if (strcmp(argv[i], "--uninstall") == 0) {
            AllocConsole();
            freopen("CONOUT$", "w", stdout);
            freopen("CONOUT$", "w", stderr);
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
    StartUp::setup(true, QString(), QString(), QString(), QString(), true);

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
