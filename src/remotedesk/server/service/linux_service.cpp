#include "linux_service.h"
#include "rdpserver.h"

#include <QCoreApplication>
#include <QDir>
#include <QMessageLogContext>
#include <QTimer>

#include <cstdio>

void logToFile(QtMsgType type, const QMessageLogContext& lg, const QString& msg);

void LinuxService::printInstallInstructions()
{
    fprintf(stdout, "Linux: use systemctl to manage the service.\n");
    fprintf(stdout, "  sudo cp remotedesk.service /etc/systemd/system/\n");
    fprintf(stdout, "  sudo systemctl enable remotedesk\n");
    fprintf(stdout, "  sudo systemctl start remotedesk\n");
}

void LinuxService::printUninstallInstructions()
{
    fprintf(stdout, "Linux: use systemctl to remove the service.\n");
    fprintf(stdout, "  sudo systemctl stop remotedesk\n");
    fprintf(stdout, "  sudo systemctl disable remotedesk\n");
    fprintf(stdout, "  sudo rm /etc/systemd/system/remotedesk.service\n");
}

int LinuxService::run(int argc, char* argv[])
{
    QCoreApplication app(argc, argv);
    QDir::setCurrent(QCoreApplication::applicationDirPath());

    QString logDir = QString("%1/logs").arg(QCoreApplication::applicationDirPath());
    QDir().mkpath(logDir);
    qInstallMessageHandler(logToFile);

    qInfo() << "Linux service mode: starting RDP server";

    RDPServer server;
    if (!server.initialize(QString(), true, true)) {
        return 1;
    }
    server.start();

    QTimer displayCheckTimer;
    bool helperLaunched = false;
    QObject::connect(&displayCheckTimer, &QTimer::timeout, [&]() {
        if (helperLaunched && server.isCaptureSourceConnected()) {
            displayCheckTimer.stop();
            return;
        }
        if (helperLaunched) return;

        const char* display = getenv("DISPLAY");
        const char* wayland = getenv("WAYLAND_DISPLAY");
        if ((display && display[0]) || (wayland && wayland[0])) {
            qInfo() << "Linux service: display detected, starting capture";
            helperLaunched = true;
        }
    });
    displayCheckTimer.start(2000);

    return app.exec();
}
