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
#define MSG_TYPE_MOUSE_MOVE   0x03  // data: [4-byte x][4-byte y]
#define MSG_TYPE_MOUSE_BUTTON 0x04  // data: [4-byte x][4-byte y][4-byte button][1-byte isDown]
#define MSG_TYPE_MOUSE_WHEEL  0x05  // data: [4-byte delta]

BOOL InstallService();
BOOL UninstallService();

void WINAPI ServiceMain(DWORD argc, LPWSTR* argv);
void WINAPI ServiceCtrlHandler(DWORD ctrlCode);

// Helper process (runs in user session, does actual SendInput)
int RunHelper(DWORD sessionId);

#endif
