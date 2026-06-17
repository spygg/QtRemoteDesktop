#include "rdpserver.h"
#include "websocketserver.h"

#include <QTimer>
#include <windows.h>
#include <wtsapi32.h>
#include <string>

void RDPServer::startSecureInputProcess()
{
    if (secureInputRunning_) return;

    DWORD sessionId = WTSGetActiveConsoleSessionId();
    if (sessionId == 0xFFFFFFFF) return;

    HANDLE hProcToken = NULL;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_DUPLICATE | TOKEN_QUERY, &hProcToken))
        return;

    HANDLE hDupToken = NULL;
    if (!DuplicateTokenEx(hProcToken, TOKEN_ALL_ACCESS, NULL, SecurityImpersonation, TokenPrimary, &hDupToken)) {
        CloseHandle(hProcToken);
        return;
    }

    SetTokenInformation(hDupToken, TokenSessionId, &sessionId, sizeof(sessionId));

    wchar_t exePath[MAX_PATH];
    GetModuleFileNameW(NULL, exePath, MAX_PATH);
    std::wstring cmdLine = std::wstring(exePath) + L" --secure-input " + std::to_wstring(wsPort_);

    STARTUPINFOW si = { sizeof(si) };
    si.lpDesktop = const_cast<wchar_t*>(L"winsta0\\winlogon");
    PROCESS_INFORMATION pi = {};
    if (CreateProcessAsUserW(hDupToken, NULL, &cmdLine[0], NULL, NULL, FALSE, 0, NULL, NULL, &si, &pi)) {
        qInfo() << "Secure input process started, PID:" << pi.dwProcessId;
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
        secureInputPid_ = pi.dwProcessId;
        secureInputRunning_ = true;
        DWORD pid = pi.dwProcessId;
        QTimer::singleShot(5000, this, [this, pid]() {
            if (wsServer_ && !wsServer_->isSecureInputConnected()) {
                qWarning() << "Secure input process didn't connect in time, terminating";
                HANDLE hProc = OpenProcess(PROCESS_TERMINATE, FALSE, pid);
                if (hProc) { TerminateProcess(hProc, 1); CloseHandle(hProc); }
            }
        });
    } else {
        qWarning() << "CreateProcessAsUserW for secure input failed:" << GetLastError();
    }

    CloseHandle(hDupToken);
    CloseHandle(hProcToken);
}

void RDPServer::stopSecureInputProcess()
{
    if (!secureInputRunning_) return;
    secureInputRunning_ = false;

    if (wsServer_)
        wsServer_->closeSecureInput();

    if (secureInputPid_) {
        HANDLE hProc = OpenProcess(PROCESS_TERMINATE, FALSE, secureInputPid_);
        if (hProc) {
            TerminateProcess(hProc, 0);
            CloseHandle(hProc);
        }
        secureInputPid_ = 0;
    }
}
