#include "rdpserver.h"
#include "singleapplication.h"
#include "startup.h"
#include "systemsleepblocker.h"

#include <QApplication>
#include <QCommandLineParser>

int main(int argc, char* argv[])
{
#if QT_VERSION < QT_VERSION_CHECK(6, 0, 0)
    QApplication::setAttribute(Qt::AA_EnableHighDpiScaling);
#endif

    SingleApplication a(argc, argv);
    if (a.isRunning()) {
        return 0;
    }

    // 开机自启动
    StartUp::setup(true, QString(), QString(), QString(), QString(), true);

    SystemSleepBlocker blocker;
    if (!blocker.start()) {
        qCritical("Failed to prevent system sleep!");
    } else {
        qWarning("已经阻止系统休眠");
    }

    QCommandLineParser parser;
    parser.setApplicationDescription("Qt Remote Desktop Server");
    parser.addHelpOption();
    QCommandLineOption noSslOption("no-ssl", "Disable HTTPS/WSS (use plain HTTP/WS)");
    parser.addOption(noSslOption);
    parser.process(a);

    bool useSslOverride = !parser.isSet(noSslOption);

    RDPServer server;
    if (!server.initialize(QString(), useSslOverride)) {
        return 1;
    }

    server.start();

    return a.exec();
}
