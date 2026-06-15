#ifndef KEYBOARDSERVICE_H
#define KEYBOARDSERVICE_H

#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0601
#endif
#include <windows.h>

#define SERVICE_NAME L"QtRemoteDesktopKeyboardSvc"
#define PIPE_NAME L"\\\\.\\pipe\\QtRemoteDesktopKeyboardSvc"

#define MSG_TYPE_VK        0x01
#define MSG_TYPE_UNICODE   0x02

BOOL InstallService();
BOOL UninstallService();

void WINAPI ServiceMain(DWORD argc, LPWSTR* argv);
void WINAPI ServiceCtrlHandler(DWORD ctrlCode);

// Helper process (runs in user session, does actual SendInput)
int RunHelper(DWORD sessionId);

#endif
