#include "inputmanager.h"
#include "rdpserver.h"
#include "screencapturer.h"
#include "service/service.h"
#include "singleapplication.h"
// #include "startup.h"
#include "crashhandler.h"
#include "systemsleepblocker.h"

#include <QCommandLineParser>
#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMutexLocker>
#include <QTextStream>
#include <QTimer>

#include <cstring>

int g_logLevel = QtDebugMsg;

// 设置日志级别；无法识别的级别返回 false
bool setLogLevel(const QString& lvRaw)
{
    const QString lv = lvRaw.toLower();
    if (lv == "debug")
        g_logLevel = QtDebugMsg;
    else if (lv == "info")
        g_logLevel = QtInfoMsg;
    else if (lv == "warning" || lv == "warn")
        g_logLevel = QtWarningMsg;
    else if (lv == "critical" || lv == "crit")
        g_logLevel = QtCriticalMsg;
    else
        return false;
    return true;
}

// 从原始 argv 解析 --log-level：供 service/helper 等绕过 main() 解析器的
// 启动路径使用（--service 时 platformMain 提前进入 LinuxService::run /
// WindowsService::run，不会执行 main() 里的 QCommandLineParser）。
void applyLogLevelFromArgs(int argc, char* argv[])
{
    for (int i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "--log-level") == 0 && i + 1 < argc)
            setLogLevel(QString::fromLocal8Bit(argv[++i]));
    }
}

void logToFile(QtMsgType type, const QMessageLogContext& lg, const QString& msg)
{
    // 日志分级过滤：低于 --log-level 指定级别的消息直接丢弃。
    // 默认 QtDebugMsg 全量记录；release 部署建议 --log-level info。
    if (type < g_logLevel)
        return;

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

    fprintf(stderr, "%s\n", content.toUtf8().constData());
    fflush(stderr);

    // 持久打开日志文件，避免每条消息都 open/close（高频日志下会显著占用主线程 CPU）
    static QFile s_log;
    static QString s_logDate;
    QDateTime dt = QDateTime::currentDateTime();
    QString date = dt.toString("yyyyMMdd");
    QString logFile = QString("%1/logs/%2.txt").arg(QCoreApplication::applicationDirPath() /*QDir::currentPath()*/).arg(date);
    if (!s_log.isOpen() || date != s_logDate) {
        if (s_log.isOpen())
            s_log.close();
        s_log.setFileName(logFile);
        if (!s_log.open(QIODevice::WriteOnly | QIODevice::Append | QFile::Text))
            return;
        s_logDate = date;
    }

    QTextStream logStream(&s_log);
#if QT_VERSION < QT_VERSION_CHECK(6, 0, 0)
    logStream.setCodec("utf-8");
#endif

    QString tm = dt.toString("[yyyy/MM/dd hh:mm:ss.zzz] ");
    logStream << tm << content << "\n";
    logStream.flush();
}

int main(int argc, char* argv[])
{

    int ret = platformMain(argc, argv);
    if (ret >= 0)
        return ret;

#if QT_VERSION < QT_VERSION_CHECK(6, 0, 0)
    QApplication::setAttribute(Qt::AA_EnableHighDpiScaling);
#endif

#if defined(Q_OS_LINUX) && !defined(Q_OS_ANDROID)
    // 无 X 环境时使用 offscreen 平台插件，让 QApplication 正常启动
    if (qEnvironmentVariableIsEmpty("DISPLAY"))
        qputenv("QT_QPA_PLATFORM", "offscreen");
#endif

    SingleApplication a(argc, argv);

    // 命令行解析放在单实例判断之前：已有实例运行时 --help/--version 仍可用
    QCommandLineParser parser;
    parser.setApplicationDescription("Qt Remote Desktop Server — 跨平台远程桌面服务端"
                                     "（HTTP/WS + 可选 SSL，JPEG/H.264 推流，WebRTC/WebCodecs）");
    parser.addHelpOption();
    parser.addVersionOption();

    QCommandLineOption noSslOption("no-ssl", "禁用 HTTPS/WSS，使用明文 HTTP/WS");
    parser.addOption(noSslOption);

    QCommandLineOption logLevelOption(
        "log-level",
        "日志分级过滤: debug|info|warning|critical（默认 debug，"
        "release 部署建议 info）",
        "level",
        "debug");
    parser.addOption(logLevelOption);

    // 以下选项由 service 层（platformMain）在进入 main 前处理并提前退出，
    // 此处注册仅为让 --help 完整展示全部能力；重复传入不会产生副作用。
    parser.addOption(QCommandLineOption("service", "以系统服务方式运行（由 service 层处理）"));
    parser.addOption(QCommandLineOption("install", "安装系统服务（由 service 层处理）"));
    parser.addOption(QCommandLineOption("uninstall", "卸载系统服务（由 service 层处理）"));
    parser.addOption(QCommandLineOption("helper", "以 helper 进程运行，连接服务端 WS 端口 <wsPort>（Windows）", "wsPort"));
    parser.addOption(QCommandLineOption("secure-input", "以安全输入 helper 运行，监听 <wsPort>（Windows）", "wsPort"));

    parser.process(a);

    if (parser.isSet(logLevelOption)) {
        if (!setLogLevel(parser.value(logLevelOption))) {
            fprintf(stderr, "Invalid --log-level '%s' (expected: debug|info|warning|critical)\n",
                    parser.value(logLevelOption).toUtf8().constData());
            return 1;
        }
    }

    if (a.isRunning()) {
        return 0;
    }

    bool useSslOverride = !parser.isSet(noSslOption);
    QString path = QString("%1/%2").arg(a.applicationDirPath()).arg("logs");
    QDir dir(path);
    if (!dir.exists()) {
        dir.mkpath(path);
    }

    qInstallMessageHandler(logToFile);
    Breakpad::CrashHandler::instance()->Init(QCoreApplication::applicationDirPath());

    SystemSleepBlocker blocker;
    if (!blocker.start()) {
        qCritical("Failed to prevent system sleep!");
    } else {
        qInfo("System sleep prevention active");
    }

    RDPServer server;
    if (!server.initialize(QString(), useSslOverride)) {
        return 1;
    }

    server.start();

    return a.exec();
}
