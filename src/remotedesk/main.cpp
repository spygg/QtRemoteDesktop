#include "inputmanager.h"
#include "rdpserver.h"
#include "screencapturer.h"
#include "service/service.h"
#include "singleapplication.h"
// #include "startup.h"
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

int g_logLevel = QtDebugMsg;

void logToFile(QtMsgType type, const QMessageLogContext& lg, const QString& msg)
{

#ifndef QT_DEBUG
    if (type < g_logLevel) {
        return;
    }
#endif

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
    if (a.isRunning()) {
        return 0;
    }

    QCommandLineParser parser;
    parser.setApplicationDescription("Qt Remote Desktop Server");
    parser.addHelpOption();
    QCommandLineOption noSslOption("no-ssl", "Disable HTTPS/WSS (use plain HTTP/WS)");
    parser.addOption(noSslOption);
    parser.process(a);

    bool useSslOverride = !parser.isSet(noSslOption);
    QString path = QString("%1/%2").arg(a.applicationDirPath()).arg("logs");
    QDir dir(path);
    if (!dir.exists()) {
        dir.mkpath(path);
    }

    qInstallMessageHandler(logToFile);

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
