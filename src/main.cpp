// main.cpp
#include "server/rdpserver.h"
#include <QApplication>

int main(int argc, char* argv[])
{
    // 设置高 DPI 支持（必须在 QApplication 创建之前）
    QApplication::setAttribute(Qt::AA_EnableHighDpiScaling);

    QApplication app(argc, argv);

    RDPServer server;
    if (!server.initialize(8084)) {
        return 1;
    }

    server.start();

    return app.exec();
}
