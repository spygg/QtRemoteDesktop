#include "rdpserver.h"
#include "systeminfo.h"
#include "websocketserver.h"

#include <QDateTime>
#include <QThread>
#include <QTimer>
#include <windows.h>
#include <tlhelp32.h>
#include <wtsapi32.h>
#include <lm.h>
#include <string>

namespace {

// 服务进程默认待在 SCM 给它的窗口站里，那里的 OpenInputDesktop 直接失败
// （实测 XP/Win7 都返回错误 1），于是永远只能猜 "Winlogon" —— XP 的登录 UI 不在
// 那个桌面上，注入的密码就石沉大海。先切到 WinSta0 再问一次输入桌面，实测
// （deskprobe 在同一进程上下文里）能拿到真实名字，如 XP 的 "Screen-saver"/"Welcome"。
// 失败返回空串。
//
// [SESSION0] 这个查询**只对 XP 有意义**：XP 没有会话隔离，服务与用户同处 Session 0，
// 打开的就是用户那个 WinSta0。Vista+ 的服务被隔离在 Session 0，这里拿到的是 Session 0
// 自己的 winsta0（其输入桌面恒为 "Default"），与屏幕上真正显示的锁屏 UI 无关。
// Vista+ 请走下面的 secureInputDesktopName()。
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

// 决定 secure-input 子进程该投到哪个桌面（lpDesktop 决定该进程主线程的输入队列
// 绑定在哪张桌面，SendInput 只对这张桌面生效）。
//
// Vista+：不能向服务进程问输入桌面。服务在 Session 0，它的 winsta0 是 Session 0 自己
//   的窗口站，OpenInputDesktop 只会返回 "Default"，而屏幕上真正的锁屏 UI 在用户会话
//   （WTSGetActiveConsoleSessionId()）里。用 "Default" 当 lpDesktop 的后果实测：
//   子进程起得来、WS 连得上、日志一切正常，但键鼠全打进一张不可见的桌面 ——
//   锁屏后经 WS 连发 3 个字符，登录框 0 个圆点。
//   用户会话里的 helper 每轮都 OpenInputDesktop，它会随 screen_locked 上报当前输入
//   桌面名（lockedDesktop）。两种锁屏的输入桌面不同：
//     · 普通锁屏 → LogonUI 挂在 winsta0\Winlogon（helper 通常打不开这张桌面，
//       lockedDesktop 为空）→ 投 Winlogon；
//     · 屏保型锁屏 → 输入桌面就叫 Screen-saver（helper 能打开）→ 必须投
//       Screen-saver，否则 WS 发来的鼠标/唤醒键全落在 Winlogon 上，屏保永远唤不醒
//       （实测：投 Winlogon 时点「解锁」只得到 not_logon_ui）。
//   本函数只在 screenLocked_ 为真时被调用（rdpserver.cpp startSecureInputProcess
//   的调用点），所以 lockedDesktop 是"锁屏当下的真实桌面"。
// XP：没有会话隔离，原来的动态查询拿到的是真实输入桌面
//   （Screen-saver / Welcome / Winlogon），保留不动。
QString secureInputDesktopName(const QString& lockedDesktop)
{
    if (isVistaPlusOs()) {
        const QString reported = lockedDesktop.trimmed();
        if (reported.compare(QStringLiteral("Screen-saver"), Qt::CaseInsensitive) == 0)
            return reported;
        return QStringLiteral("Winlogon");
    }
    QString desktopName = queryInputDesktopName();
    if (desktopName.isEmpty())
        desktopName = lockedDesktop;
    if (desktopName.isEmpty())
        desktopName = QStringLiteral("Winlogon");
    return desktopName;
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
        // 取值同样走 secureInputDesktopName()：Vista+ 恒为 Winlogon，若这里仍用
        // Session 0 的输入桌面（恒 "Default"），日志会反复打印
        // "followed: Winlogon -> Default" 的假跟随。
        const QString wantDesktop = secureInputDesktopName(lockedDesktop_);
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
    // 投放到**登录 UI 真正所在的桌面**。SendInput 只能到达调用线程所附着的桌面，
    // 投错桌面 = 键鼠打进一个没人看的桌面（进程照样能起来、能连上 WS、日志无异常）。
    //
    // 取值全交给 secureInputDesktopName()：
    //   · Vista+ 固定 winsta0\Winlogon（服务在 Session 0，问不到用户会话的输入桌面）；
    //   · XP 按真实输入桌面取名 —— 实测 XP 主机锁屏时 OpenInputDesktop 给的是
    //     "Screen-saver"/"Welcome"，而 "Winlogon" 桌面虽然能打开却不是输入桌面，
    //     旧代码写死 Winlogon 时注入永远石沉大海，表现就是"前端一键解锁点了没反应"。
    //
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
    QString desktopName = secureInputDesktopName(lockedDesktop_);
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

// ---------------------------------------------------------------------------
// [LOCK] 锁屏兜底（只在 helper 没回执的 timeout 路径用，最多一次）
//
// 为什么需要：Windows 服务在 Session 0，直接 rundll32 user32.dll,LockWorkStation
// 只作用在 Session 0 那个不可见的会话上 —— 屏幕不会锁，接口却返回成功，
// 前端于是显示"已锁定"（用户反馈的"锁定空操作"）。真正能锁屏的只有两种进程：
//   · 用户会话里的 helper（正常路径，有 ack / screen_locked 双重确认）；
//   · 用 console 会话的用户令牌 CreateProcessAsUser 拉起来的一次性进程。
// 这里实现后者，令牌来源与 helper 的启动逻辑一致（explorer 属主）；console
// 会话还在登录界面（没有 explorer）时退回服务自身令牌 + 改 TokenSessionId，
// 与 startSecureInputProcess() 完全相同 —— 那条路径已被验证能在会话里生效。
// 投递的是**本程序的 --lock-once 模式**（不是 rundll32）：它在用户会话里调
// LockWorkStation()，并把 BOOL 作为退出码返回，服务端据此得到这次兜底的 ack。
// rundll32 的退出码不反映 LockWorkStation 的结果，只能"投递成功"，无法判断
// 到底锁上没有 —— 那正是旧实现谎报"已锁定"的根源。
// ---------------------------------------------------------------------------
static const int kLockOnceTimeoutMs = 2000;
// [LOCK] 兜底阶段的硬预算（B3）：等待被移到工作线程后，主线程只负责收回调；
// 万一令牌操作/线程调度被卡住，3s 到点也必须给客户端一个结论，不能沉默。
static const int kLockFallbackBudgetMs = 3000;

// 同步实现：取 console 会话的登录用户令牌 → CreateProcessAsUser --lock-once →
// 等它退出并读退出码。**只允许工作线程调用**：函数里不碰任何 Qt 对象、不碰
// RDPServer 成员，结果通过参数回传（B3 要求服务主线程不被 WaitForSingleObject 阻塞）。
static bool rdpserverLockConsoleSessionOnce(int waitMs, QString* detail)
{
    const DWORD sessionId = WTSGetActiveConsoleSessionId();
    if (sessionId == 0xFFFFFFFF) {
        *detail = QStringLiteral("没有活动 console 会话");
        return false;
    }

    HANDLE hUserToken = NULL;
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap != INVALID_HANDLE_VALUE) {
        PROCESSENTRY32W pe;
        ZeroMemory(&pe, sizeof(pe));
        pe.dwSize = sizeof(pe);
        if (Process32FirstW(snap, &pe)) {
            do {
                if (QString::fromWCharArray(pe.szExeFile)
                        .compare(QStringLiteral("explorer.exe"), Qt::CaseInsensitive) != 0)
                    continue;
                DWORD sid = 0;
                if (!ProcessIdToSessionId(pe.th32ProcessID, &sid) || sid != sessionId)
                    continue;
                HANDLE hProc = OpenProcess(PROCESS_QUERY_INFORMATION, FALSE, pe.th32ProcessID);
                if (!hProc)
                    continue;
                if (!OpenProcessToken(hProc, TOKEN_DUPLICATE | TOKEN_QUERY, &hUserToken))
                    hUserToken = NULL;
                CloseHandle(hProc);
                if (hUserToken)
                    break;
            } while (Process32NextW(snap, &pe));
        }
        CloseHandle(snap);
    }

    bool usedSystemToken = false;
    HANDLE hBase = hUserToken;
    if (!hBase) {
        usedSystemToken = true;
        if (!OpenProcessToken(GetCurrentProcess(),
                              TOKEN_DUPLICATE | TOKEN_QUERY | TOKEN_ASSIGN_PRIMARY, &hBase)) {
            *detail = QStringLiteral("OpenProcessToken 失败，错误码 %1").arg(GetLastError());
            return false;
        }
    }

    HANDLE hDup = NULL;
    bool spawned = false;
    if (DuplicateTokenEx(hBase, TOKEN_ALL_ACCESS, NULL, SecurityImpersonation,
                         TokenPrimary, &hDup)) {
        DWORD sid = sessionId;
        if (usedSystemToken) {
            if (!SetTokenInformation(hDup, TokenSessionId, &sid, sizeof(sid))) {
                *detail = QStringLiteral("SetTokenInformation(TokenSessionId) 失败，错误码 %1")
                              .arg(GetLastError());
                CloseHandle(hDup);
                CloseHandle(hBase);
                return false;
            }
        }
        STARTUPINFOW si;
        ZeroMemory(&si, sizeof(si));
        si.cb = sizeof(si);
        si.lpDesktop = const_cast<LPWSTR>(L"winsta0\\default");
        PROCESS_INFORMATION pi;
        ZeroMemory(&pi, sizeof(pi));
        // 用本程序的 --lock-once：执行 LockWorkStation() 并以退出码回报 BOOL。
        wchar_t exePath[MAX_PATH] = {};
        GetModuleFileNameW(NULL, exePath, MAX_PATH);
        std::wstring cmdLine = std::wstring(L"\"") + exePath + L"\" --lock-once";

        typedef BOOL (WINAPI *CPAUserW_t)(HANDLE, LPCWSTR, LPWSTR,
            LPSECURITY_ATTRIBUTES, LPSECURITY_ATTRIBUTES, BOOL, DWORD,
            LPVOID, LPCWSTR, LPSTARTUPINFOW, LPPROCESS_INFORMATION);
        CPAUserW_t pCreateProcessAsUserW = (CPAUserW_t)GetProcAddress(
            GetModuleHandleA("advapi32"), "CreateProcessAsUserW");
        if (!pCreateProcessAsUserW) {
            *detail = QStringLiteral("CreateProcessAsUserW 不可用");
        } else if (pCreateProcessAsUserW(hDup, NULL, &cmdLine[0], NULL, NULL, FALSE,
                                         CREATE_NO_WINDOW, NULL, NULL, &si, &pi)) {
            // 等一次性进程退出并读退出码（最多 waitMs）。超时视为失败：进程都还没
            // 跑完就把"已锁"报给前端，正是本轮要根除的谎报。
            DWORD code = STILL_ACTIVE;
            if (WaitForSingleObject(pi.hProcess, waitMs) == WAIT_OBJECT_0)
                GetExitCodeProcess(pi.hProcess, &code);
            spawned = (code == 0);
            if (!spawned) {
                *detail = (code == STILL_ACTIVE)
                    ? QStringLiteral("一次性锁定进程 %1ms 内未退出").arg(waitMs)
                    : QStringLiteral("一次性锁定进程退出码 %1（LockWorkStation 未成功）").arg(code);
            }
            CloseHandle(pi.hThread);
            CloseHandle(pi.hProcess);
        } else {
            *detail = QStringLiteral("CreateProcessAsUser 失败，错误码 %1").arg(GetLastError());
        }
        CloseHandle(hDup);
    } else {
        *detail = QStringLiteral("DuplicateTokenEx 失败，错误码 %1").arg(GetLastError());
    }
    CloseHandle(hBase);

    if (spawned) {
        *detail = QStringLiteral("console 会话 %1 的%2令牌下 LockWorkStation 返回成功（退出码 0）")
                      .arg(static_cast<qulonglong>(sessionId))
                      .arg(usedSystemToken ? QStringLiteral("SYSTEM")
                                           : QStringLiteral("登录用户"));
    }
    return spawned;
}

// [LOCK][B3] 兜底等待的工作线程：run() 里只跑纯 Win32（列进程找 explorer 令牌 →
// CreateProcessAsUser --lock-once → WaitForSingleObject），结果存成员，由
// QThread::finished（基类自带信号，子类不需要 Q_OBJECT/moc）投回主线程读取。
class LockOnceThread : public QThread
{
public:
    LockOnceThread(int seq, QString clientId, int waitMs)
        : seq_(seq), clientId_(clientId), waitMs_(waitMs) {}

    void run() override
    {
        QString detail;
        ok_ = rdpserverLockConsoleSessionOnce(waitMs_, &detail);
        detail_ = detail;
    }

    int seq() const { return seq_; }
    const QString& clientId() const { return clientId_; }
    bool lockedOk() const { return ok_; }
    const QString& detailText() const { return detail_; }

private:
    int seq_;
    QString clientId_;
    int waitMs_;
    bool ok_ = false;
    QString detail_;
};

void RDPServer::startLockFallback(int seq, const QString& clientId, bool helperTimedOut)
{
    // 硬预算：等待虽然不在主线程，但客户端仍必须在 3s 量级内拿到结论。
    // 线程回来得极慢（或令牌操作被卡住）时，由这里兜底给出 timeout/lock_failed。
    QTimer::singleShot(kLockFallbackBudgetMs, this,
        [this, seq, clientId, helperTimedOut]() {
            if (!lockPending_ || seq != lockPendingSeq_
                    || lockPendingClientId_ != clientId)
                return;
            qWarning() << "Lock: fallback budget" << kLockFallbackBudgetMs << "ms exhausted";
            finishWindowsLockFallback(seq, clientId, false,
                QStringLiteral("兜底锁定 %1ms 内未返回").arg(kLockFallbackBudgetMs),
                helperTimedOut);
        });

    LockOnceThread* th = new LockOnceThread(seq, clientId, kLockOnceTimeoutMs);
    connect(th, &QThread::finished, this, [this, th, helperTimedOut]() {
        // 只在 finished 之后读线程里的结果，读完再交给主线程收尾。
        const int s = th->seq();
        const QString cid = th->clientId();
        const bool ok = th->lockedOk();
        const QString detail = th->detailText();
        th->deleteLater();
        finishWindowsLockFallback(s, cid, ok, detail, helperTimedOut);
    });
    th->start();
}

void RDPServer::finishWindowsLockFallback(int seq, const QString& clientId, bool ok,
                                          const QString& detail, bool helperTimedOut)
{
    // 期间已收到 helper ack / screen_locked(true) / 新一轮请求 → 本轮作废。
    if (!lockPending_ || seq != lockPendingSeq_ || lockPendingClientId_ != clientId)
        return;
    lockPending_ = false;
    lockPendingClientId_.clear();

    if (ok) {
        // [B1] 兜底成功是一次**真实**的锁定：必须立刻把服务端状态同步成已锁，
        // 否则客户端在这段时间点「解锁」会命中 screenLocked_==false 的 not_locked
        // 分支，回一句"屏幕未锁定" —— 屏幕明明已经锁上了。
        applyFallbackScreenLocked();
        qInfo() << "Lock:" << detail << "(fallback ack = locked)";
    } else {
        qWarning() << "Lock: fallback failed -" << detail;
    }

    const QString okMsg = helperTimedOut
        ? QStringLiteral("helper 未回执，已通过 console 会话令牌锁定")
        : QStringLiteral("已通过 console 会话令牌锁定");
    const QString failMsg = helperTimedOut
        ? QStringLiteral("锁屏未被确认：helper 3 秒未回执，console 会话兜底也失败（%1）").arg(detail)
        : QStringLiteral("LockWorkStation 未成功（%1）").arg(detail);
    wsServer_->sendJson(clientId, QJsonObject {
        { "type", "lock_result" }, { "ok", ok },
        { "reason", ok ? QStringLiteral("locked")
                       : (helperTimedOut ? QStringLiteral("timeout")
                                         : QStringLiteral("lock_failed")) },
        { "message", ok ? okMsg : failMsg } });
}

void RDPServer::applyFallbackScreenLocked()
{
    screenLocked_ = true;
    // 兜底路径没有 helper 的现场信息（它没连上或没回执）：桌面名只能留空，
    // secure-input 会回退 winsta0\Winlogon（Session 0 的服务端能给的唯一答案）。
    lockedDesktop_.clear();
    lockedKind_.clear();
    lockedHint_ = QStringLiteral("Windows 锁屏在安全桌面上，点「解锁」由服务端注入密码");
    startSecureInputProcess();

    QJsonObject out;
    out["type"] = "screen_locked";
    out["locked"] = true;
    // platformTag() 是 rdpserver.cpp 里的 static，Windows 侧常量即可（该函数只在 _WIN32 编译）。
    out["platform"] = QStringLiteral("windows");
    out["isXP"] = !isVistaPlusOs();
    out["hint"] = lockedHint_;
    wsServer_->broadcastJson(out);
    qInfo() << "Lock: fallback success -> screenLocked_=true, secure-input started,"
               " screen_locked broadcasted";
}

// ===========================================================================
// 登录界面远程登录（login_screen）
//
// 背景：注销后 / 开机停在登录界面时，控制台会话里**没有任何用户会话**。
// helper（以及任何用 WTSQueryUserToken 拿用户令牌的路径）必然失败，
// 所以 login_screen 完全不复用 helper：服务用自己的 SYSTEM 令牌 +
// TokenSessionId=ActiveConsoleSessionId 把 secure-input 投到 winsta0\Winlogon。
// 这一点已用探针实测（Win7 注销后 UIA 能拿到磁贴/焦点元素）。
//
// 成功判据不看"已注入"，只看控制台会话里是否真的出现了请求的用户名
// （WTSQuerySessionInformation），避免再出现"15s 假成功"。
// ===========================================================================
namespace {

QString winStripDomain(const QString& s)
{
    QString t = s.trimmed();
    const int pos = t.lastIndexOf(QLatin1Char('\\'));
    if (pos >= 0)
        t = t.mid(pos + 1);
    return t.trimmed();
}

// 控制台会话当前登录用户名；没有用户（停在登录界面）时返回空串。
QString winConsoleSessionUser(DWORD sessionId)
{
    if (sessionId == 0xFFFFFFFF)
        return QString();
    LPWSTR buf = nullptr;
    DWORD bytes = 0;
    if (!WTSQuerySessionInformationW(WTS_CURRENT_SERVER_HANDLE, sessionId,
                                     WTSUserName, &buf, &bytes) || !buf)
        return QString();
    QString user = QString::fromWCharArray(buf).trimmed();
    WTSFreeMemory(buf);
    return user;
}

bool winProcessExistsInSession(const QString& exeName, DWORD sessionId)
{
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE)
        return false;
    bool found = false;
    PROCESSENTRY32W pe;
    ZeroMemory(&pe, sizeof(pe));
    pe.dwSize = sizeof(pe);
    if (Process32FirstW(snap, &pe)) {
        do {
            if (QString::fromWCharArray(pe.szExeFile)
                    .compare(exeName, Qt::CaseInsensitive) != 0)
                continue;
            DWORD sid = 0;
            if (ProcessIdToSessionId(pe.th32ProcessID, &sid) && sid == sessionId) {
                found = true;
                break;
            }
        } while (Process32NextW(snap, &pe));
    }
    CloseHandle(snap);
    return found;
}

QString winRegString(HKEY root, const wchar_t* subKey, const wchar_t* valueName)
{
    HKEY key = nullptr;
    if (RegOpenKeyExW(root, subKey, 0, KEY_READ, &key) != ERROR_SUCCESS)
        return QString();
    wchar_t buf[512] = {};
    DWORD type = 0;
    DWORD size = static_cast<DWORD>(sizeof(buf) - sizeof(wchar_t));
    QString result;
    if (RegQueryValueExW(key, valueName, nullptr, &type,
                         reinterpret_cast<LPBYTE>(buf), &size) == ERROR_SUCCESS
            && (type == REG_SZ || type == REG_EXPAND_SZ)) {
        result = QString::fromWCharArray(buf).trimmed();
    }
    RegCloseKey(key);
    return result;
}

// 最后控制台用户名：Vista+ 的 LogonUI 键；XP 没有该键，退回 Winlogon\DefaultUserName。
QString winLastLoggedOnUser()
{
    QString user = winRegString(HKEY_LOCAL_MACHINE,
        L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Authentication\\LogonUI",
        L"LastLoggedOnUser");
    if (user.isEmpty()) {
        user = winRegString(HKEY_LOCAL_MACHINE,
            L"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion\\Winlogon",
            L"DefaultUserName");
    }
    return winStripDomain(user);
}

// 本机账户是否存在（不触网、不试密码）。登录前先查，避免在登录界面盲打。
bool winLocalUserExists(const QString& user)
{
    const std::wstring wuser = user.toStdWString();
    USER_INFO_1* info = nullptr;
    DWORD err = NetUserGetInfo(nullptr, wuser.c_str(), 1,
                               reinterpret_cast<LPBYTE*>(&info));
    if (info)
        NetApiBufferFree(info);
    return err == NERR_Success;
}

}  // namespace

QString RDPServer::effectiveLoginUsername() const
{
    if (!loginUsername_.trimmed().isEmpty())
        return winStripDomain(loginUsername_);
    QString last = winLastLoggedOnUser();
    if (last.isEmpty()) {
        const DWORD sid = WTSGetActiveConsoleSessionId();
        last = winConsoleSessionUser(sid);
    }
    return last;
}

void RDPServer::startLoginScreenWatcher()
{
    auto* timer = new QTimer(this);
    timer->setInterval(1500);
    connect(timer, &QTimer::timeout, this, [this]() { updateLoginScreenState(); });
    timer->start();
    // 服务启动/重连后尽快判一次，前端不必等 1.5s。
    QTimer::singleShot(1000, this, [this]() { updateLoginScreenState(); });
    qInfo() << "LoginScreen: watcher started (1500ms)";
}

void RDPServer::updateLoginScreenState()
{
    if (!serviceMode_)
        return;

    const DWORD sessionId = WTSGetActiveConsoleSessionId();
    bool active = false;
    if (sessionId != 0xFFFFFFFF) {
        // 判据只看 WTS：控制台会话没有用户名 + LogonUI 在这个会话里 ⇒ 停在登录界面。
        // 不能用 isCaptureSourceConnected() 判断"有没有用户桌面"：登录界面上服务
        // 仍然会拉 helper，而 helper 拿不到用户令牌（没有用户会话）就退化成 SYSTEM
        // 令牌连上 /capture（Win7 实测），这时捕获源存在但**桌面不存在**，
        // 用它当判据会让登录模式刚置位就被翻回 false。
        // 有用户名时（锁屏/已登录）绝不进入登录模式，避免和锁屏解锁抢通道。
        const QString user = winConsoleSessionUser(sessionId);
        active = user.isEmpty() && winProcessExistsInSession(QStringLiteral("LogonUI.exe"),
                                                             sessionId);
    }

    if (active == loginScreenActive_)
        return;

    loginScreenActive_ = active;
    if (active) {
        // 登录界面也要有 secure-input：它由 SYSTEM 令牌投到 winlogon 桌面，
        // 键盘/鼠标转发和密码注入都走它（与锁屏共用同一个进程/通道）。
        if (!wsServer_->isSecureInputConnected())
            startSecureInputProcess();
    }

    QJsonObject out;
    out["type"] = "login_screen_state";
    out["active"] = active;
    out["username"] = effectiveLoginUsername();
    out["platform"] = QStringLiteral("windows");
    out["isXP"] = !isVistaPlusOs();
    wsServer_->broadcastJson(out);
    qInfo() << "LoginScreen: state ->" << (active ? "login screen (no user session)"
                                                  : "user session / desktop")
            << "consoleSession =" << sessionId
            << "username =" << effectiveLoginUsername();
}

void RDPServer::handleWindowsLoginScreen(const QString& clientId, const QString& username,
                                         const QString& password)
{
    auto fail = [this, &clientId](const QString& reason, const QString& message) {
        wsServer_->sendJson(clientId, QJsonObject {
            { "type", "login_result" }, { "ok", false },
            { "reason", reason }, { "message", message } });
        qWarning() << "LoginScreen:" << reason << "-" << message;
    };

    const QString user = username.trimmed().isEmpty()
        ? effectiveLoginUsername()
        : winStripDomain(username);
    const QString pwd = password.isEmpty() ? unlockPassword_ : password;

    if (user.isEmpty()) {
        fail(QStringLiteral("no_credentials"),
             QStringLiteral("未配置登录账户：在 server_config.json 设置 loginUsername，"
                            "或让机器至少登录过一次（最后控制台用户）"));
        return;
    }
    if (pwd.isEmpty()) {
        fail(QStringLiteral("no_credentials"),
             QStringLiteral("未配置登录密码：复用 server_config.json 的 unlockPassword"));
        return;
    }

    const DWORD sessionId = WTSGetActiveConsoleSessionId();
    if (sessionId == 0xFFFFFFFF) {
        fail(QStringLiteral("no_console_session"), QStringLiteral("没有活动的控制台会话"));
        return;
    }

    // 已经有用户会话：这不是登录界面。登录请求应改用"解锁"或直接操作桌面。
    // 判据同样只看 WTS 用户名：登录界面上也可能有 SYSTEM 令牌的捕获源连着。
    const QString currentUser = winConsoleSessionUser(sessionId);
    if (!currentUser.isEmpty()) {
        fail(QStringLiteral("not_logon_ui"),
             QStringLiteral("当前不是登录界面（控制台会话已登录用户 %1）").arg(currentUser));
        return;
    }
    if (!winProcessExistsInSession(QStringLiteral("LogonUI.exe"), sessionId)) {
        fail(QStringLiteral("not_logon_ui"),
             QStringLiteral("控制台会话里没有 LogonUI，当前不是登录界面"));
        return;
    }
    if (!winLocalUserExists(user)) {
        fail(QStringLiteral("user_not_found"),
             QStringLiteral("登录界面上没有账户 %1（本机不存在该用户）").arg(user));
        return;
    }

    bool justStarted = false;
    if (!wsServer_->isSecureInputConnected()) {
        startSecureInputProcess();
        justStarted = true;
    }
    if (!justStarted && !wsServer_->isSecureInputConnected()) {
        fail(QStringLiteral("unconfirmed"),
             QStringLiteral("安全输入通道未就绪，请稍后重试"));
        return;
    }

    // 新一轮登录：seq 作废上一轮所有延时回调，同时清空终判状态。
    const int seq = ++loginSeq_;
    loginPending_ = true;
    loginFinalSent_ = false;
    loginTyped_ = false;
    loginPendingClientId_ = clientId;
    loginPendingUser_ = user;
    loginScreenActive_ = true;

    if (justStarted) {
        // 进程刚拉起，WS 连接还要 1s 左右；与解锁路径一致延后 1.5s 再下发。
        QTimer::singleShot(1500, this, [this, seq, user, pwd]() {
            if (seq != loginSeq_)
                return;
            wsServer_->sendToSecureInput(QJsonObject {
                { "type", "login" }, { "username", user }, { "password", pwd },
                { "seq", seq } });
        });
    } else {
        wsServer_->sendToSecureInput(QJsonObject {
            { "type", "login" }, { "username", user }, { "password", pwd },
            { "seq", seq } });
    }

    // 先给客户端一条"进行中"，避免界面无反馈。
    wsServer_->sendJson(clientId, QJsonObject {
        { "type", "login_result" }, { "ok", true }, { "pending", true },
        { "reason", "injecting" },
        { "message", QStringLiteral("正在登录 %1…").arg(user) } });

    // 兜底：22s 内没有任何最终结论（secure-input 卡死/通道断）→ unconfirmed，
    // 绝不默认成功。
    QTimer::singleShot(22000, this, [this, seq]() {
        if (seq != loginSeq_ || !loginPending_ || loginFinalSent_)
            return;
        loginFinalSent_ = true;
        loginPending_ = false;
        const QString cid = loginPendingClientId_;
        wsServer_->sendJson(cid, QJsonObject {
            { "type", "login_result" }, { "ok", false },
            { "reason", "unconfirmed" },
            { "message", QStringLiteral("22 秒内未收到登录结论（安全输入通道可能中断），"
                                        "请查看画面后重试") } });
        qWarning() << "LoginScreen: no final result within 22s -> unconfirmed, seq =" << seq;
    });
}

void RDPServer::handleLoginProgress(const QJsonObject& msg)
{
    const int seq = msg["seq"].toInt();
    if (!loginPending_ || seq != loginSeq_ || loginFinalSent_)
        return;

    const QString stage = msg["stage"].toString();
    const QString reason = msg["reason"].toString();
    const QString text = msg["message"].toString();
    const QString desktop = msg["desktop"].toString();

    if (stage == QLatin1String("verified")) {
        qInfo() << "LoginScreen: target verified (seq =" << seq << ")";
        return;
    }
    if (stage == QLatin1String("typed")) {
        loginTyped_ = true;
        qInfo() << "LoginScreen: password typed (typed =" << msg["typed"].toInt()
                << ", dropped =" << msg["dropped"].toInt()
                << ", cleared =" << msg["cleared"].toInt() << ", seq =" << seq
                << ") -> confirming via WTS session user";
        wsServer_->sendJson(loginPendingClientId_, QJsonObject {
            { "type", "login_result" }, { "ok", true }, { "pending", true },
            { "reason", "confirming" },
            { "message", QStringLiteral("密码已键入，正在确认登录结果…") } });
        tickLoginConfirmation(seq, 0);
        return;
    }

    // blocked / uncertain：没有键入任何字符（或无法确认目标）。
    QString outReason = reason;
    if (stage == QLatin1String("uncertain")
            || (outReason != QLatin1String("user_not_found")
                && outReason != QLatin1String("not_logon_ui")
                && outReason != QLatin1String("no_credentials")
                && outReason != QLatin1String("stuck_dialog"))) {
        outReason = QStringLiteral("unconfirmed");
    }
    loginFinalSent_ = true;
    loginPending_ = false;
    qWarning() << "LoginScreen: secure-input did not type -" << stage << reason
               << "desktop =" << desktop << text << "seq =" << seq;
    wsServer_->sendJson(loginPendingClientId_, QJsonObject {
        { "type", "login_result" }, { "ok", false },
        { "reason", outReason }, { "message", text } });
}

void RDPServer::tickLoginConfirmation(int seq, int attempt)
{
    if (seq != loginSeq_ || !loginPending_ || loginFinalSent_)
        return;

    const DWORD sessionId = WTSGetActiveConsoleSessionId();
    const QString currentUser = winConsoleSessionUser(sessionId);

    auto finish = [this, seq](bool ok, const QString& reason, const QString& message) {
        if (seq != loginSeq_ || loginFinalSent_)
            return;
        loginFinalSent_ = true;
        loginPending_ = false;
        if (ok) {
            // 登录成功：登录模式立即退出（输入不再转发到 secure-input）；
            // screenLocked_ 仍等 helper 上报 false，保持既有锁屏链路零回归。
            loginScreenActive_ = false;
            QJsonObject st;
            st["type"] = "login_screen_state";
            st["active"] = false;
            st["username"] = loginPendingUser_;
            st["platform"] = QStringLiteral("windows");
            st["isXP"] = !isVistaPlusOs();
            wsServer_->broadcastJson(st);
        }
        wsServer_->sendJson(loginPendingClientId_, QJsonObject {
            { "type", "login_result" }, { "ok", ok },
            { "reason", reason }, { "message", message } });
        qInfo() << "LoginScreen: final ->" << (ok ? "confirmed" : reason)
                << "user =" << loginPendingUser_ << "seq =" << seq;
    };

    if (!currentUser.isEmpty()) {
        if (currentUser.compare(loginPendingUser_, Qt::CaseInsensitive) == 0) {
            finish(true, QStringLiteral("confirmed"),
                   QStringLiteral("已登录为 %1（控制台会话已确认）").arg(currentUser));
        } else {
            finish(false, QStringLiteral("unconfirmed"),
                   QStringLiteral("控制台会话已登录为 %1，与请求账户 %2 不一致")
                       .arg(currentUser, loginPendingUser_));
        }
        return;
    }

    // Vista+ 密码正确时通常 3~5s 切桌面；XP 要 8~12s。给足预算，不提前报错。
    const int maxAttempts = isVistaPlusOs() ? 12 : 20;
    if (attempt >= maxAttempts) {
        if (loginTyped_) {
            finish(false, QStringLiteral("wrong_password"),
                   QStringLiteral("登录界面仍停在密码框（%1 秒），用户名 %2 的密码未通过验证")
                       .arg(attempt).arg(loginPendingUser_));
        } else {
            finish(false, QStringLiteral("unconfirmed"),
                   QStringLiteral("未确认会话状态：等待 %1 秒后控制台会话仍无用户")
                       .arg(attempt));
        }
        return;
    }
    QTimer::singleShot(1000, this, [this, seq, attempt]() {
        tickLoginConfirmation(seq, attempt + 1);
    });
}
