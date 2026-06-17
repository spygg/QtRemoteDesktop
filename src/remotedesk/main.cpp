#include "rdpserver.h"
#include "singleapplication.h"
#include "startup.h"
#include "systemsleepblocker.h"

#include <QApplication>
#include <QCommandLineParser>
#include <QDir>
#include <QMutexLocker>
#include <QTextStream>

#ifdef _WIN32
#include <windows.h>
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

int main(int argc, char* argv[])
{
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
    if (parser.isSet(consoleOption)) {
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
