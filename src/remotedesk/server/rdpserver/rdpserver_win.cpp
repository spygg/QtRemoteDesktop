#include "rdpserver.h"
#include "websocketserver.h"

#include <QDateTime>
#include <QTimer>
#include <windows.h>
#include <wtsapi32.h>
#include <string>

namespace {

// 服务进程默认待在 SCM 给它的窗口站里，那里的 OpenInputDesktop 直接失败
// （实测 XP/Win7 都返回错误 1），于是永远只能猜 "Winlogon" —— XP 的登录 UI 不在
// 那个桌面上，注入的密码就石沉大海。先切到 WinSta0 再问一次输入桌面，实测
// （deskprobe 在同一进程上下文里）能拿到真实名字，如 XP 的 "Screen-saver"/"Welcome"。
// 失败返回空串。
QString queryInputDesktopName()
{
    static HWINSTA station = NULL;
    if (!station) {
        station = OpenWindowStationW(L"winsta0", FALSE,
            WINSTA_ENUMDESKTOPS | WINSTA_READATTRIBUTES | WINSTA_WRITEATTRIBUTES
                | DELETE | READ_CONTROL);
        if (!station) {
            qWarning() << "OpenWindowStation(winsta0) failed:" << GetLastError();
            return QString();
        }
        if (!SetProcessWindowStation(station)) {
            qWarning() << "SetProcessWindowStation(winsta0) failed:" << GetLastError();
            CloseWindowStation(station);
            station = NULL;
            return QString();
        }
    }
    HDESK hInput = OpenInputDesktop(0, FALSE, GENERIC_READ);
    if (!hInput)
        return QString();
    wchar_t name[128] = {};
    DWORD len = 0;
    QString out;
    if (GetUserObjectInformationW(hInput, UOI_NAME, name, sizeof(name), &len) && name[0])
        out = QString::fromWCharArray(name);
    CloseDesktop(hInput);
    return out;
}

} // namespace

void RDPServer::startSecureInputProcess()
{
    if (secureInputRunning_) {
        // 标志位一旦置 true 就永久阻止重启，而进程可能已崩溃或从未连上（下面
        // 5s 超时会终止它）。所以这里先做一次健康判定：进程活着**且**已连上
        // /secure-input 才算健康；否则复位标志走重新启动流程。不修的话，锁屏
        // 输入（登录密码）会一直失效到服务重启为止。
        //
        // 还要求"进程所投的桌面 == 当前输入桌面"：登录 UI 会在锁屏期间换桌面
        // （XP 屏保退出后落到登录桌面），旧桌面上活得好好的进程只会把键鼠打进
        // 没人看的地方，必须重投。
        //
        // 但解锁序列进行中不能按桌面重投：解锁本身就要发 SAS，SAS 把输入桌面从
        // Screen-saver 切到 Winlogon，helper 随后（实测 0.9s 内）补报一次
        // screen_locked=true，于是这里判定"桌面不符"、把正在写密码的进程杀掉 ——
        // 第一次尝试全废，只能等服务端 8s 后的第二轮，XP 解锁从 ~2s 拖到 ~9s。
        // 进程自己每轮都重新附着当前输入桌面（attachInputDesktop），所以序列不受伤。
        //
        // [A18] 因此桌面名**不能**用来判"要重启"：secureInputDesktop_ 只是启动
        // 那一刻的快照，之后从不更新，而屏保/欢迎屏/登录桌面之间的切换会让它与
        // 当前输入桌面长期错位。旧逻辑每错位一次就杀掉活得好好的进程重启（XP
        // 七天日志 25 次误杀），表现为"点一次解锁没反应，要点第二次"。
        // 现在只把桌面名当"跟随记录"：进程下一轮会自己 attach 过去。
        const QString wantDesktop = queryInputDesktopName();
        bool alive = false;
        HANDLE hProc = OpenProcess(SYNCHRONIZE | PROCESS_QUERY_INFORMATION, FALSE, secureInputPid_);
        if (hProc) {
            alive = (WaitForSingleObject(hProc, 0) == WAIT_TIMEOUT);
            CloseHandle(hProc);
        }
        const bool connected = (wsServer_ && wsServer_->isSecureInputConnected());
        if (alive && connected) {
            if (!wantDesktop.isEmpty()
                    && secureInputDesktop_.compare(wantDesktop, Qt::CaseInsensitive) != 0) {
                qInfo() << "Secure input followed input desktop:"
                        << secureInputDesktop_ << "->" << wantDesktop;
                secureInputDesktop_ = wantDesktop;
            }
            return;
        }
        qWarning() << "Secure input process unhealthy (alive =" << alive
                   << "connected =" << connected
                   << "desktop =" << secureInputDesktop_ << "want =" << wantDesktop
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
    // 投放到**登录 UI 真正所在的输入桌面**，而不是写死的 winsta0\winlogon。
    // SendInput 只能到达调用线程所附着的桌面，投错桌面 = 键鼠打进一个没人看的桌面。
    // 实测 XP 主机：登录界面在 Welcome/Security（该机器上两者甚至不存在），锁屏时
    // OpenInputDesktop 给的是 "Screen-saver"/"Welcome"，而 "Winlogon" 桌面虽然能打开
    // 却不是输入桌面 —— 旧代码在这里能成功创建进程、能连上 WS、注入却永远石沉大海，
    // 表现就是"前端一键解锁点了没反应"。Vista+ 锁屏时输入桌面正是 Winlogon，
    // 所以按输入桌面取名在两个世代上都自动正确。
    //
    // 名字来源按可信度排序：自己切到 WinSta0 问到的输入桌面 > helper 上报的锁屏桌面
    // （helper 在用户会话里，但桌面切换瞬间它也可能拿不到）> winsta0\Winlogon 兜底。
    // [DESKNAME-BUF] 缓冲区必须留足余量。旧代码是
    //   wchar_t desktop[] = L"winsta0\\Winlogon";   // 只有 18 个 wchar
    // 于是 "winsta0\" + 桌面名 超过 17 个字符就被静默截断：XP 锁屏时输入桌面叫
    // "Screen-saver"（12 字符），拼出来 20 字符 → 截成 "winsta0\Screen-sav"。
    // 那个桌面并不存在，可 CreateProcessAsUserW **照样成功**（内核会新建/映射），
    // 于是安全输入进程被投到一个没人看的桌面上，解锁按钮点了完全没反应，而日志
    // 打印的还是截断前的全名，根本看不出问题。Win7 的 "Default"（15 字符）刚好
    // 放得下，所以这个 bug 只在特定桌面名上暴露。
    // 桌面名是内核对象名，长度上限远小于此；64 wchar 足够且避免截断。
    wchar_t desktop[64] = L"winsta0\\Winlogon";
    QString desktopName = queryInputDesktopName();
    if (desktopName.isEmpty())
        desktopName = lockedDesktop_;
    if (desktopName.isEmpty())
        desktopName = QStringLiteral("Winlogon");
    {
        // "winsta0\" 占 8 个 wchar，后面拼桌面名。
        const QString full = QStringLiteral("winsta0\\") + desktopName;
        const std::wstring w = full.toStdWString();
        const size_t n = qMin<size_t>(w.size(), sizeof(desktop) / sizeof(wchar_t) - 1);
        w.copy(desktop, n);
        desktop[n] = L'\0';
        if (n < w.size())
            qWarning() << "Secure input desktop name truncated:" << full
                       << "->" << QString::fromWCharArray(desktop);
    }
    secureInputDesktop_ = desktopName;
    si.lpDesktop = desktop;
    // 打印**实际生效**的 lpDesktop（而不是 desktopName），截断/取舍一眼可见。
    qInfo() << "Secure input process will attach to desktop"
            << QString::fromWCharArray(desktop);
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
    secureInputDesktop_.clear();

    if (wsServer_)
        wsServer_->closeSecureInput();

    if (secureInputProcHandle_) {
        TerminateProcess((HANDLE)secureInputProcHandle_, 0);
        CloseHandle((HANDLE)secureInputProcHandle_);
        secureInputProcHandle_ = nullptr;
    }
    secureInputPid_ = 0;
}
