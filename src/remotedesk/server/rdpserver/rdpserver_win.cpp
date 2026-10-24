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

    if (!SetTokenInformation(hDupToken, TokenSessionId, &sessionId, sizeof(sessionId))) {
        // 不改会话 id 就会在 Session0 启动 helper：它永远连不上 winlogon 桌面，
        // 只会被 5s 超时杀掉并反复重试。这里显式失败退出，日志能直接定位原因。
        qWarning() << "SetTokenInformation(TokenSessionId) failed:" << GetLastError();
        CloseHandle(hDupToken);
        CloseHandle(hProcToken);
        return;
    }

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
        secureInputProcHandle_ = pi.hProcess;   // 保留句柄，超时/停止时直接用它终止
        secureInputPid_ = pi.dwProcessId;
        secureInputRunning_ = true;
        const quint64 gen = ++secureInputGen_;
        QTimer::singleShot(5000, this, [this, gen]() {
            // 代次已变 = 期间 stop 过（或又拉起了新进程）→ 本定时器作废，
            // 否则会 TerminateProcess 掉新进程并把状态复位。
            if (gen != secureInputGen_)
                return;
            if (wsServer_ && !wsServer_->isSecureInputConnected()) {
                qWarning() << "Secure input process didn't connect in time, terminating";
                // 用保留的句柄终止：按 PID 重新 OpenProcess 在 PID 被回收时
                // 可能误杀无关进程。
                if (secureInputProcHandle_) {
                    TerminateProcess((HANDLE)secureInputProcHandle_, 1);
                    CloseHandle((HANDLE)secureInputProcHandle_);
                    secureInputProcHandle_ = nullptr;
                }
                // 复位标志，允许下一次锁屏事件重新拉起（否则永久失效）
                secureInputPid_ = 0;
                secureInputRunning_ = false;
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
    // 作废在途的 5s 超时定时器（它只持有 this 与代次，无法单独取消）
    ++secureInputGen_;
    secureInputRunning_ = false;

    if (wsServer_)
        wsServer_->closeSecureInput();

    if (secureInputProcHandle_) {
        TerminateProcess((HANDLE)secureInputProcHandle_, 0);
        CloseHandle((HANDLE)secureInputProcHandle_);
        secureInputProcHandle_ = nullptr;
    }
    secureInputPid_ = 0;
}
