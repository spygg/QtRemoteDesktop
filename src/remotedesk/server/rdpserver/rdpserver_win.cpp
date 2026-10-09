#include "rdpserver.h"
#include "websocketserver.h"

#include <QTimer>
#include <windows.h>
#include <wtsapi32.h>
#include <string>

void RDPServer::startSecureInputProcess()
{
    if (secureInputRunning_) {
        // 标志位一旦置 true 就永久阻止重启，而进程可能已崩溃或从未连上（下面
        // 5s 超时会终止它）。所以这里先做一次健康判定：进程活着**且**已连上
        // /secure-input 才算健康；否则复位标志走重新启动流程。不修的话，锁屏
        // 输入（登录密码）会一直失效到服务重启为止。
        bool alive = false;
        HANDLE hProc = OpenProcess(SYNCHRONIZE | PROCESS_QUERY_INFORMATION, FALSE, secureInputPid_);
        if (hProc) {
            alive = (WaitForSingleObject(hProc, 0) == WAIT_TIMEOUT);
            CloseHandle(hProc);
        }
        if (alive && wsServer_ && wsServer_->isSecureInputConnected())
            return;
        qWarning() << "Secure input process unhealthy (alive =" << alive
                   << ") - restarting";
        stopSecureInputProcess();
    }

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
    std::wstring cmdLine = std::wstring(exePath) + L" --secure-input " + std::to_wstring(wsPort_)
        + (sslConfiguration_ ? L" --ssl" : L"");

    STARTUPINFOW si = { sizeof(si) };
    si.lpDesktop = const_cast<wchar_t*>(L"winsta0\\winlogon");
    PROCESS_INFORMATION pi = {};

    // Load dynamically: MinGW 5.3.0 import lib may map to kernel32.dll
    // (not available before Win7); the function lives in advapi32.dll on XP+.
    typedef BOOL (WINAPI *CPAUserW_t)(HANDLE, LPCWSTR, LPWSTR,
        LPSECURITY_ATTRIBUTES, LPSECURITY_ATTRIBUTES, BOOL, DWORD,
        LPVOID, LPCWSTR, LPSTARTUPINFOW, LPPROCESS_INFORMATION);
    CPAUserW_t pCreateProcessAsUserW = (CPAUserW_t)GetProcAddress(
        GetModuleHandleA("advapi32"), "CreateProcessAsUserW");

    if (!pCreateProcessAsUserW) {
        qWarning() << "CreateProcessAsUserW not available";
        CloseHandle(hDupToken);
        CloseHandle(hProcToken);
        return;
    }

    if (pCreateProcessAsUserW(hDupToken, NULL, &cmdLine[0], NULL, NULL, FALSE, 0, NULL, NULL, &si, &pi)) {
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
                // 复位标志，允许下一次锁屏事件重新拉起（否则永久失效）
                if (secureInputPid_ == pid) {
                    secureInputPid_ = 0;
                    secureInputRunning_ = false;
                }
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
