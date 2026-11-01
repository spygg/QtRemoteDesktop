#include "rdpserver.h"

// macOS 既没有 Windows 的 Secure Input（凭据输入隔离进程），也没有 Linux 的
// login1 会话锁定，远程"锁屏/解锁"没有可对等的系统能力。rdpserver.cpp 无条件
// 调用这两个入口，所以 Apple 侧提供空实现，保证链接与前端流程一致。
void RDPServer::startSecureInputProcess()
{
}

void RDPServer::stopSecureInputProcess()
{
}
