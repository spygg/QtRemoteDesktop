#include "shell.h"
#include <QWebSocket>

// Android：本进程没有可供控制台使用的 PTY 子进程环境，阶段 1 不提供网页终端。
// 返回 nullptr 即由调用方（RDPServer::onShellConnected）直接关闭该 WS 连接，
// 前端"终端"页签显示不可用，其余（画面/输入）功能不受影响。
InteractiveShell* InteractiveShell::create(QWebSocket* socket, QObject* parent)
{
    Q_UNUSED(socket);
    Q_UNUSED(parent);
    return nullptr;
}
