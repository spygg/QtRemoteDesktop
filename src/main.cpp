// main.cpp
#include "server/rdpserver.h"
#include <QApplication>
#include <QCommandLineParser>

int main(int argc, char* argv[])
{
    QApplication::setAttribute(Qt::AA_EnableHighDpiScaling);

    QApplication app(argc, argv);

    QCommandLineParser parser;
    parser.setApplicationDescription("Qt Remote Desktop Server");
    parser.addHelpOption();
    QCommandLineOption noSslOption("no-ssl", "Disable HTTPS/WSS (use plain HTTP/WS)");
    parser.addOption(noSslOption);
    parser.process(app);

    bool useSsl = !parser.isSet(noSslOption);

    RDPServer server(useSsl);
    if (!server.initialize(8084)) {
        return 1;
    }

    server.start();

    return app.exec();
}
