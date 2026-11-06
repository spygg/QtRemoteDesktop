#include "rdpserver.h"

// Android 没有 Windows 的 Secure Input（凭据输入隔离进程）。rdpserver.cpp 无条件
// 调用这两个入口，这里提供空实现，保证链接与前端流程一致。
void RDPServer::startSecureInputProcess()
{
}

void RDPServer::stopSecureInputProcess()
{
}
