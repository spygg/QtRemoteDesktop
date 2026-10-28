#include "secure_input_process.h"

#include "systeminfo.h"

#include <QCoreApplication>
#include <QDir>
#include <QScopedPointer>
#include <cstring>
#include <string>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMessageLogContext>
#include <QTimer>
#include <QUrl>
#include <QWebSocket>

#ifdef _WIN32
#include <windows.h>
#include <tlhelp32.h>
#endif

// ---------------------------------------------------------------------------
// 安全桌面解锁：绕过 SendInput
//
// 实测（VMware 上的 Windows XP SP3「安全屏保」锁屏）：
//   secure-input 进程的 SendInput 只作用于该进程自身窗口站的默认桌面（winsta0\default），
//   而锁屏 UI 在 winsta0\Screen-saver / winsta0\Winlogon 安全桌面上。结果是
//     - SAS(Ctrl+Alt+Del) 有效：它由 win32k 特判处理，不经过输入队列；
//     - 其它所有按键（含密码字符）全部落在 default 桌面 → 登录框密码框永远空白，
//       回车提交的是空密码，解锁必然失败，且没有任何错误提示。
//   SetThreadDesktop 也救不了：线程的输入队列在第一次需要队列的调用时就被固定到
//   当时的桌面，之后换桌面不会迁移队列（实测密码框仍收不到任何字符）。
//
// 可靠做法：在独立线程里附着当前安全桌面，枚举登录对话框并直接操作其控件
//   （WM_SETTEXT 写密码 + WM_COMMAND(IDOK) 提交），完全不依赖输入队列。
//   XP 实测：对话框 class=#32770，子控件依次为 用户名 Edit / 密码 Edit / 确定 Button(id=1)。
// ---------------------------------------------------------------------------
#ifdef _WIN32
namespace {

BOOL CALLBACK findVisibleDialogProc(HWND h, LPARAM lp)
{
    char cls[64] = { 0 };
    GetClassNameA(h, cls, sizeof(cls) - 1);
    if (IsWindowVisible(h) && strcmp(cls, "#32770") == 0) {
        *reinterpret_cast<HWND*>(lp) = h;
        return FALSE;  // 只取第一个可见对话框
    }
    return TRUE;
}

struct EditCollector {
    HWND edits[8] = { 0 };
    int  n = 0;
    HWND okBtn = nullptr;
};

BOOL CALLBACK collectChildProc(HWND h, LPARAM lp)
{
    auto* c = reinterpret_cast<EditCollector*>(lp);
    char cls[128] = { 0 };
    GetClassNameA(h, cls, sizeof(cls) - 1);
    if (strcmp(cls, "Edit") == 0) {
        if (c->n < 8) c->edits[c->n++] = h;
    } else if (strcmp(cls, "Button") == 0) {
        if (GetDlgCtrlID(h) == 1) c->okBtn = h;   // IDOK
    }
    return TRUE;
}

// 把密码写进登录对话框并提交。三态返回：区分「桌面还没有登录对话框」和
// 「有对话框但被拒」，前者可以等/补 SAS，后者只能走 SendInput 兜底（避免重复提交）。
enum FillResult { kNoDialog, kRejected, kSubmitted };

FillResult fillLogonDialog(const QString& password, QString* detail)
{
    HWND dlg = nullptr;
    EnumWindows(findVisibleDialogProc, reinterpret_cast<LPARAM>(&dlg));
    if (!dlg) {
        *detail = QStringLiteral("no visible #32770 logon dialog");
        return kNoDialog;
    }
    EditCollector col;
    EnumChildWindows(dlg, collectChildProc, reinterpret_cast<LPARAM>(&col));
    if (col.n < 2) {
        *detail = QStringLiteral("logon dialog has %1 Edit control(s), need >=2").arg(col.n);
        return kNoDialog;
    }
    // 第 1 个 Edit 是用户名，第 2 个是密码框
    HWND pwdEdit = col.edits[1];
    const std::wstring wpwd = password.toStdWString();
    // 必须用 SendMessageTimeout 而不是 SendMessage：这是**跨进程**同步调用，而目标是
    // winlogon/msgina 的窗口。桌面切换的瞬间（刚发过 SAS、旧对话框还在拆）那个线程
    // 可能不再取消息，SendMessage 就永远不返回 —— 本进程的工作线程卡死在这里，
    // BusyGuard 的析构不会执行，g_unlockBusy 永久为 1，之后每次「解锁」都被
    // :499 的守卫直接丢弃；服务侧健康检查看到进程还活着、连接还在，也不会重启它。
    // 表现是"解锁按钮从此再也没反应，直到重启服务"。SMTO_ABORTIFHUNG 让卡住的
    // 目标最多等 2 秒就把控制权还给流程。
    DWORD_PTR setRes = 0;
    const BOOL setOk = SendMessageTimeoutW(pwdEdit, WM_SETTEXT, 0,
                                           reinterpret_cast<LPARAM>(wpwd.c_str()),
                                           SMTO_ABORTIFHUNG, 2000, &setRes);
    if (!setOk) {
        *detail = QStringLiteral("password box did not reply within 2s (logon thread busy?)");
        return kRejected;
    }
    if (!setRes) {
        *detail = QStringLiteral("WM_SETTEXT rejected by password box");
        return kRejected;
    }
    // 提交：msgina 的解锁对话框响应 WM_COMMAND(IDOK)（实测有效，跨进程无 UIPI 限制）
    SendMessageTimeoutW(dlg, WM_COMMAND, MAKEWPARAM(IDOK, BN_CLICKED),
                        reinterpret_cast<LPARAM>(col.okBtn), SMTO_ABORTIFHUNG, 2000, nullptr);
    Sleep(600);
    if (IsWindow(dlg)) {
        // 兜底：直接点按钮
        if (col.okBtn) {
            SendMessageTimeoutW(col.okBtn, BM_CLICK, 0, 0, SMTO_ABORTIFHUNG, 2000, nullptr);
            Sleep(400);
        }
    }
    *detail = QStringLiteral("password written to logon dialog and submitted");
    return kSubmitted;
}

// SAS：唯一能在安全桌面上生效的注入（win32k 特判，不经过输入队列）。
// 用途：XP 锁屏可能是「安全屏保」(Screen-saver 桌面) 或「欢迎屏幕」(没有密码框)，
// 两者都只能靠 Ctrl+Alt+Del 切到带用户名/密码的传统登录框。
void sendSasKeys()
{
    auto key = [](UINT vk, bool down) {
        INPUT in = { };
        in.type = INPUT_KEYBOARD;
        in.ki.wVk = static_cast<WORD>(vk);
        in.ki.wScan = static_cast<WORD>(MapVirtualKeyW(vk, MAPVK_VK_TO_VSC));
        if (!down) in.ki.dwFlags = KEYEVENTF_KEYUP;
        SendInput(1, &in, sizeof(INPUT));
    };
    key(VK_CONTROL, true);
    key(VK_MENU, true);
    key(VK_DELETE, true);
    Sleep(50);
    key(VK_DELETE, false);
    key(VK_MENU, false);
    key(VK_CONTROL, false);
}

// 兜底路径：本线程输入队列已绑定到当前安全桌面，SendInput 才会落到正确的桌面
void sendPasswordWithSendInput(const QString& password)
{
    int failed = 0;
    auto key = [&failed](UINT vk, bool down) {
        INPUT in = { };
        in.type = INPUT_KEYBOARD;
        in.ki.wVk = static_cast<WORD>(vk);
        in.ki.wScan = static_cast<WORD>(MapVirtualKeyW(vk, MAPVK_VK_TO_VSC));
        if (!down) in.ki.dwFlags = KEYEVENTF_KEYUP;
        if (SendInput(1, &in, sizeof(INPUT)) != 1)
            ++failed;
    };
    for (const QChar& ch : password) {
        SHORT vkScan = VkKeyScanW(static_cast<wchar_t>(ch.unicode()));
        if (vkScan == -1) continue;
        const BYTE vk = vkScan & 0xFF;
        const BYTE state = (vkScan >> 8) & 0xFF;
        if (state & 1) key(VK_SHIFT, true);
        key(vk, true);
        Sleep(30);
        key(vk, false);
        if (state & 1) key(VK_SHIFT, false);
        Sleep(40);
    }
    key(VK_RETURN, true);
    Sleep(30);
    key(VK_RETURN, false);
    qInfo() << "SecureInput: typed password (" << password.size() << " chars), rejected key events ="
            << failed;
}

// 在安全桌面上点一下（绝对坐标，0..65535 归一化）。
// 返回 false 表示 SendInput 被拒（安全桌面上常见原因：线程桌面不是输入桌面）。
bool clickScreen(int x, int y)
{
    const int w = GetSystemMetrics(SM_CXSCREEN);
    const int h = GetSystemMetrics(SM_CYSCREEN);
    if (w <= 1 || h <= 1)
        return false;
    const LONG nx = (x * 65535) / (w - 1);
    const LONG ny = (y * 65535) / (h - 1);
    INPUT mv = { };
    mv.type = INPUT_MOUSE;
    mv.mi.dx = nx;
    mv.mi.dy = ny;
    mv.mi.dwFlags = MOUSEEVENTF_MOVE | MOUSEEVENTF_ABSOLUTE;
    SendInput(1, &mv, sizeof(mv));
    Sleep(40);
    INPUT dn = { };
    dn.type = INPUT_MOUSE;
    dn.mi.dwFlags = MOUSEEVENTF_LEFTDOWN;
    INPUT up = dn;
    up.mi.dwFlags = MOUSEEVENTF_LEFTUP;
    const UINT downSent = SendInput(1, &dn, sizeof(dn));
    Sleep(40);
    const UINT upSent = SendInput(1, &up, sizeof(up));
    return downSent == 1 && upSent == 1;
}

// 欢迎屏幕展开后密码框是真正的 Edit 子控件（DirectUIHWND 的兄弟窗口）；
// 没有它就说明磁贴还没点开。
struct EditScan { HWND edit; };

BOOL CALLBACK findEditProc(HWND h, LPARAM lp)
{
    char cls[64] = { 0 };
    GetClassNameA(h, cls, sizeof(cls) - 1);
    if (strcmp(cls, "Edit") != 0)
        return TRUE;
    reinterpret_cast<EditScan*>(lp)->edit = h;
    return FALSE;
}

HWND findPasswordEdit()
{
    HWND host = FindWindowA("NativeHWNDHost", nullptr);
    if (!host)
        return nullptr;
    EditScan scan { nullptr };
    EnumChildWindows(host, findEditProc, reinterpret_cast<LPARAM>(&scan));
    return scan.edit;
}

// 当前键盘焦点是不是一个 Edit（欢迎屏幕点开磁贴后的密码框）。
bool focusIsEdit(QString* clsOut)
{
    GUITHREADINFO gti;
    memset(&gti, 0, sizeof(gti));
    gti.cbSize = sizeof(gti);
    if (!GetGUIThreadInfo(0, &gti) || !gti.hwndFocus)
        return false;
    char cls[64] = { 0 };
    GetClassNameA(gti.hwndFocus, cls, sizeof(cls) - 1);
    if (clsOut)
        *clsOut = QString::fromLatin1(cls);
    return strcmp(cls, "Edit") == 0;
}

// XP 的 FUS「欢迎屏幕」：NativeHWNDHost + DirectUIHWND，整页是自绘的，
// 没有任何 #32770 子控件可以 WM_SETTEXT（实测 Winlogon 桌面上只剩一个隐藏的
// "GINA Logon" #32770）。它只能像人一样操作：
//   1) 点用户名磁贴 —— 页面展开一行带密码 Edit 的输入区；
//   2) 点一下那个 Edit 让它拿到键盘焦点（实测磁贴展开后 logonui 经常**不**把焦点
//      交给 Edit，焦点还停在 DirectUIHWND 上；此时再点磁贴没有任何反应）；
//   3) 逐字 SendInput 输入密码 + 回车。
// 实测（2310x1398）：磁贴图标/用户名在 (1190..1340, 660..708)，展开后的密码框在
// (1254,705)+(155x21)。磁贴的纵向位置在不同锁屏状态下会差十几像素（开屏保锁 vs
// 直接锁工作站），所以按候选点依次点、每次都看焦点有没有真的落进 Edit。
bool fillWelcomeScreen(const QString& password, QString* detail)
{
    const int w = GetSystemMetrics(SM_CXSCREEN);
    const int h = GetSystemMetrics(SM_CYSCREEN);
    if (w <= 1 || h <= 1) {
        *detail = QStringLiteral("no metrics for welcome screen");
        return false;
    }

    auto settleAndType = [&](const char* how) -> bool {
        QString cls;
        for (int k = 0; k < 15; ++k) {
            Sleep(100);
            if (focusIsEdit(&cls)) {
                *detail = QStringLiteral("welcome screen: %1, focus=%2").arg(how, cls);
                qInfo() << "SecureInput:" << *detail;
                sendPasswordWithSendInput(password);
                return true;
            }
        }
        return false;
    };

    HWND edit = findPasswordEdit();
    qInfo() << "SecureInput: welcome screen" << w << "x" << h
            << "existing password edit =" << (void*)edit;

    if (!edit) {
        // 磁贴：点开后 DirectUI 会创建密码 Edit（VM 上实测要 1.8s 左右，给 2.5s）
        const int tx = w * 53 / 100;
        const int ty = h * 49 / 100;
        qInfo() << "SecureInput: welcome tile click at" << tx << ty
                << "sendInputOk =" << clickScreen(tx, ty);
        for (int k = 0; k < 25 && !edit; ++k) {
            Sleep(100);
            edit = findPasswordEdit();
        }
    }

    // 框已经存在：直接点框中心（位置现取，不猜）
    for (int round = 0; edit && round < 2; ++round) {
        RECT r;
        if (!GetWindowRect(edit, &r) || r.right <= r.left || r.bottom <= r.top)
            break;
        const int cx = (r.left + r.right) / 2;
        const int cy = (r.top + r.bottom) / 2;
        clickScreen(cx, cy);
        if (settleAndType("password box clicked"))
            return true;
        qInfo() << "SecureInput: click into password box at" << cx << cy << "did not take focus";
        edit = findPasswordEdit();
    }

    // 兜底：有的状态下框已经画出来、Edit 却还没创建（DirectUI 懒创建），
    // 按实测的框区域再点几下，能不能拿到焦点仍然只看 GetGUIThreadInfo。
    const int cand[][2] = {
        { w * 58 / 100, h * 51 / 100 },   // 密码框中心一带
        { w * 55 / 100, h * 50 / 100 },   // 密码框上沿一带
        { w * 57 / 100, h * 49 / 100 },   // 磁贴右侧（用户名文字）
    };
    for (int i = 0; i < 3; ++i) {
        clickScreen(cand[i][0], cand[i][1]);
        if (settleAndType("password box area clicked"))
            return true;
    }
    *detail = QStringLiteral("welcome screen: password box never got focus");
    return false;
}

// 解锁上下文：每次重试用全新线程（线程与桌面的绑定一旦建立就换不掉，
// 复用线程会导致第二轮枚举到的还是旧桌面的窗口）。
struct UnlockCtx {
    QString pwd;
    int attempt;
};

// 同一时刻只允许一条解锁序列在跑。服务端会在 5s 后补发一次 unlock，如果上一轮
// 还在重试，两条序列会各自发 SAS 互相打断 —— XP 上一次 SAS 会把传统登录框切回
// 欢迎屏幕，交替打断的结果是永远拿不到 #32770 密码框。
volatile LONG g_unlockBusy = 0;

// 桌面名是否属于「不可能有登录对话框」的锁屏桌面
bool isScreensaverDesktop(const QString& name)
{
    return name.compare(QStringLiteral("Screen-saver"), Qt::CaseInsensitive) == 0;
}

// 结束 XP 的屏保进程（*.scr）。
// 屏保恢复锁屏时登录界面在另一张桌面上，Screen-saver 这张只有屏保画面：点击、键入
// 全部落空。杀掉 .scr 后 Winlogon 会把输入桌面切回登录桌面，那边才有用户磁贴。
// 安全屏保被杀不会绕过密码 —— Winlogon 紧接着弹的是登录界面。
int killScreenSaverProcesses()
{
    int killed = 0;
    HANDLE hSnap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (hSnap == INVALID_HANDLE_VALUE)
        return 0;
    PROCESSENTRY32W pe = { sizeof(pe) };
    if (Process32FirstW(hSnap, &pe)) {
        do {
            const QString exe = QString::fromWCharArray(pe.szExeFile);
            if (!exe.endsWith(QLatin1String(".scr"), Qt::CaseInsensitive))
                continue;
            HANDLE hProc = OpenProcess(PROCESS_TERMINATE, FALSE, pe.th32ProcessID);
            if (!hProc)
                continue;
            if (TerminateProcess(hProc, 0))
                ++killed;
            CloseHandle(hProc);
            qInfo() << "SecureInput: terminated screen saver" << exe;
        } while (Process32NextW(hSnap, &pe));
    }
    CloseHandle(hSnap);
    return killed;
}

// 当前桌面是否停在 XP 的「欢迎屏幕」形态（有背景窗口、没有可见的 #32770 登录框）。
// 这种形态没有密码输入框，必须再补一次 SAS 才会切到传统登录框。
struct WelcomeScan { bool found; };

BOOL CALLBACK findWelcomeProc(HWND h, LPARAM lp)
{
    if (!IsWindowVisible(h))
        return TRUE;
    char cls[128] = { 0 };
    GetClassNameA(h, cls, sizeof(cls) - 1);
    if (strstr(cls, "LogonUIBackground") || strcmp(cls, "NativeHWNDHost") == 0) {
        reinterpret_cast<WelcomeScan*>(lp)->found = true;
        return FALSE;
    }
    return TRUE;
}

// 真实 OS 版本统一由 SystemInfo 提供（server/systeminfo/systeminfo.h）。
// 本机可能被设了「以兼容模式运行 Windows XP」的兼容性标记，连 RtlGetVersion
// 和注册表 CurrentVersion / ProductName 都会被伪造，所以分支判断只信
// isVistaPlusOs()（kernel32 导出探测），不看任何版本数字。

bool hasWelcomeScreen()
{
    WelcomeScan c { false };
    EnumWindows(findWelcomeProc, reinterpret_cast<LPARAM>(&c));
    return c.found;
}

DWORD WINAPI unlockThreadProc(LPVOID param);

// 附着当前输入桌面，返回桌面名。
// afterSas=false：还没发 SAS，直接取当前桌面即可。锁屏是「安全屏保」时输入桌面会
//   一直停在 Screen-saver（屏保桌面不可能出现登录对话框），在这里干等纯属浪费 —— 
//   实测白等 8s 后才去发 SAS，把整个解锁拖到 16s。
// afterSas=true：刚发过 SAS，桌面从 Screen-saver 切到 Winlogon 需要 3.5~4.2s（XP 实测），
//   因此一直等到桌面离开 Screen-saver 再附着（给 8s 余量）。
QString attachInputDesktop(bool afterSas)
{
    QString deskName;
    const int rounds = afterSas ? 40 : 1;
    for (int i = 0; i < rounds; ++i) {
        HDESK input = OpenInputDesktop(0, FALSE, GENERIC_ALL);
        if (!input) { Sleep(200); continue; }
        wchar_t nm[256] = { 0 };
        DWORD needed = 0;
        if (GetUserObjectInformationW(input, UOI_NAME, nm, sizeof(nm) - sizeof(wchar_t), &needed))
            deskName = QString::fromWCharArray(nm);
        if (afterSas && isScreensaverDesktop(deskName) && i < rounds - 1) {
            // 仍是屏保桌面：桌面切换还没完成，继续等
            CloseDesktop(input);
            Sleep(200);
            continue;
        }
        if (!SetThreadDesktop(input))
            qWarning() << "SecureInput: SetThreadDesktop(" << deskName << ") failed, err="
                       << static_cast<int>(GetLastError());
        CloseDesktop(input);
        return deskName;
    }
    qWarning() << "SecureInput: OpenInputDesktop failed, err="
               << static_cast<int>(GetLastError());
    return deskName;
}

// 读取当前输入桌面名（只读查询，不改变本线程的桌面绑定）。
QString currentInputDesktopName()
{
    HDESK h = OpenInputDesktop(0, FALSE, GENERIC_READ);
    if (!h)
        return QString();
    wchar_t nm[256] = { 0 };
    DWORD needed = 0;
    QString out;
    if (GetUserObjectInformationW(h, UOI_NAME, nm, sizeof(nm) - sizeof(wchar_t), &needed))
        out = QString::fromWCharArray(nm);
    CloseDesktop(h);
    return out;
}

// 键入密码后确认是否真的解开：登录成功后输入桌面会切回 Default。
// 返回 true 表示已解锁、或暂时问不到桌面名（别用 SAS 去打断可能正在进行的登录）。
bool likelyUnlocked(int ms)
{
    for (int waited = 0; waited <= ms; waited += 200) {
        const QString d = currentInputDesktopName();
        if (d.isEmpty())
            return true;
        if (d.compare(QLatin1String("default"), Qt::CaseInsensitive) == 0)
            return true;
        Sleep(200);
    }
    return false;
}

// 独立线程：附着当前输入桌面 → 等登录对话框 → 控件写入 + 提交。
// 必须独立线程 —— 主线程已发过 SAS，其输入队列被固定在旧桌面。
DWORD WINAPI unlockThreadProc(LPVOID param)
{
    QScopedPointer<UnlockCtx> ctx(static_cast<UnlockCtx*>(param));
    // 忙碌标志所有权：正常退出时释放；若把重试交给新线程，则移交所有权，
    // 避免上一轮先解除锁、下一轮还在跑时又插进一条序列。
    bool handOff = false;
    struct BusyGuard {
        bool* handOff;
        ~BusyGuard() { if (!*handOff) InterlockedExchange(&g_unlockBusy, 0); }
    } guard { &handOff };

    const QString pw = ctx->pwd;
    const bool afterSas = ctx->attempt > 0;
    const QString deskName = attachInputDesktop(afterSas);
    qInfo() << "SecureInput: unlock attempt" << ctx->attempt << "on input desktop" << deskName;

    QString detail;
    FillResult res = kNoDialog;

    // 已经解开了（本地有人输了密码 / 上一步就把屏保打掉）：输入桌面回到 Default。
    // 这时候任何注入都只会落到用户正在用的窗口上，必须直接收工。
    if (deskName.compare(QLatin1String("default"), Qt::CaseInsensitive) == 0) {
        qInfo() << "SecureInput: input desktop is Default - already unlocked, nothing to do";
        return 0;
    }

    // Vista+：LogonUI 是自绘界面，永远不会出现 #32770；但本进程是用
    // lpDesktop=<输入桌面> 拉起的，输入队列一开始就绑在登录桌面上，SendInput
    // 可直接进入密码框。所以不必等对话框、更不能发 SAS（SAS 会弹出「切换用户」
    // 把密码框切走）。屏保型锁屏先晃一下鼠标把屏保打掉，再输密码。
    if (isVistaPlusOs()) {
        if (isScreensaverDesktop(deskName)) {
            INPUT mv = { };
            mv.type = INPUT_MOUSE;
            mv.mi.dx = 1;
            mv.mi.dy = 1;
            mv.mi.dwFlags = MOUSEEVENTF_MOVE;
            SendInput(1, &mv, sizeof(mv));
            Sleep(700);
        } else {
            Sleep(300);   // 登录界面刚起来时给一点稳定时间
        }
        qInfo() << "SecureInput: Vista+ on desktop" << deskName
                << "- typing password via SendInput";
        sendPasswordWithSendInput(pw);
        return 0;
    }

    // XP FUS 欢迎屏幕 / 锁定界面：没有任何 #32770 可写，只能像人一样操作 ——
    // 点屏幕中偏右的用户磁贴 → 密码框（真 Edit 控件）展开并拿到焦点 → 键入密码回车。
    // 键入后必须用结果确认（输入桌面切回 Default）；仍锁着就返回 false 交给上层继续
    // 重试，而不是当作成功收工。
    auto tryWelcomeScreen = [&]() -> bool {
        if (!hasWelcomeScreen())
            return false;
        qInfo() << "SecureInput: welcome screen detected, clicking user tile";
        QString wdetail;
        if (!fillWelcomeScreen(pw, &wdetail)) {
            detail = wdetail;
            return false;
        }
        if (likelyUnlocked(4000)) {
            qInfo() << "SecureInput: unlock submitted -" << wdetail;
            detail = wdetail;
            return true;
        }
        detail = wdetail + QStringLiteral("; still locked after typing");
        return false;
    };

    if (isScreensaverDesktop(deskName)) {
        // XP 屏保恢复锁屏：输入桌面是 Screen-saver，登录界面在另一张桌面上，在这张
        // 桌面上点磁贴/键入全部落空。先结束屏保进程，Winlogon 随后把输入桌面切回
        // 登录桌面；线程与桌面的绑定换不掉，所以必须换新线程重新附着。
        // （每次尝试等待桌面切换 8s 已够；次数上限压到 3，保证整链仍在服务端的
        //   30s 解锁窗口内。）
        qInfo() << "SecureInput: XP input desktop is Screen-saver - killing .scr";
        const int killed = killScreenSaverProcesses();
        if (ctx->attempt + 1 < 3) {
            Sleep(300);
            auto* next = new UnlockCtx { pw, ctx->attempt + 1 };
            HANDLE th = CreateThread(nullptr, 0, unlockThreadProc, next, 0, nullptr);
            if (th) {
                CloseHandle(th);
                handOff = true;
                return 0;
            }
            delete next;
            qWarning() << "SecureInput: retry thread failed, err=" << static_cast<int>(GetLastError());
        }
        detail = QStringLiteral("input desktop is Screen-saver (killed %1 .scr)").arg(killed);
    } else if (tryWelcomeScreen()) {
        return 0;
    } else {
        // 界面可能还在过渡（桌面切换 + 登录 UI 起界面）：先按传统对话框等 5s；
        // 期间欢迎屏幕可能才起来，每轮都重扫一次，别对着欢迎屏幕空等 #32770。
        for (int i = 0; i < 25; ++i) {
            res = fillLogonDialog(pw, &detail);
            if (res != kNoDialog)
                break;
            if (hasWelcomeScreen()) {
                if (tryWelcomeScreen())
                    return 0;
                break;
            }
            Sleep(200);
        }
    }

    if (res == kSubmitted) {
        qInfo() << "SecureInput: unlock submitted -" << detail;
        return 0;
    }

    if (res == kNoDialog && ctx->attempt + 1 < 4) {
        // XP：锁屏是「安全屏保」/「欢迎屏幕」时 SendInput 全部被吞，必须先 SAS 把
        // 桌面切到传统登录框，再走控件直操作。每次都在全新线程里重试（本线程的
        // 桌面绑定换不掉）；桌面切换的等待交给下一轮的 attachInputDesktop(true)。
        qInfo() << "SecureInput: no logon dialog (" << detail << "), welcome screen ="
                << hasWelcomeScreen() << ", sending SAS and retrying";
        sendSasKeys();
        Sleep(200);
        auto* next = new UnlockCtx { pw, ctx->attempt + 1 };
        HANDLE th = CreateThread(nullptr, 0, unlockThreadProc, next, 0, nullptr);
        if (th) {
            CloseHandle(th);
            handOff = true;   // 所有权交给重试线程
        } else {
            delete next;
            qWarning() << "SecureInput: retry thread failed, err=" << static_cast<int>(GetLastError());
        }
        return 0;
    }

    if (ctx->attempt > 0) {
        // 盲打只在仍然锁着时做：上一步（例如结束屏保）可能已经把机器解开，那时
        // 输入桌面是 Default，盲打等于往当前焦点窗口里敲一串登录密码。
        const QString now = currentInputDesktopName();
        if (now.compare(QLatin1String("default"), Qt::CaseInsensitive) == 0) {
            qInfo() << "SecureInput: input desktop is Default - already unlocked, skip blind typing";
            return 0;
        }
        qWarning() << "SecureInput: control path failed (" << detail << "), falling back to SendInput";
        sendPasswordWithSendInput(pw);
    } else {
        qWarning() << "SecureInput: control path failed (" << detail << ")";
    }
    return 0;
}

}  // namespace
#endif

void logToFile(QtMsgType type, const QMessageLogContext& lg, const QString& msg);
void applyLogLevelFromArgs(int argc, char* argv[]);
// 日志角色后缀：secure-input 与 service 是独立进程，共用日志文件会互相交错
void setLogRoleSuffix(const QString& suffix);

int SecureInputProcess::run(int argc, char* argv[], int wsPort)
{
#ifdef _WIN32
    QCoreApplication app(argc, argv);
    QDir::setCurrent(QCoreApplication::applicationDirPath());

    QString logDir = QString("%1/logs").arg(QCoreApplication::applicationDirPath());
    QDir().mkpath(logDir);
    applyLogLevelFromArgs(argc, argv);
    setLogRoleSuffix(QStringLiteral("_secure-input"));
    qInstallMessageHandler(logToFile);

    {
        // 启动戳：既确认跑的是哪一份构建，也把本机的真实版本号写进日志，
        // 便于判断解锁分支选择是否正确。
        //   os/build 可能被"XP 兼容模式"伪造，vistaPlus 走的是导出探测（可信）；
        //   两者矛盾时以 vistaPlus 为准，product 用来确认注册表有没有被伪造。
        const WinOsInfo& osv = winOsInfo();
        qInfo() << "SecureInput: build" << __DATE__ << __TIME__ << "os"
                << QStringLiteral("%1.%2.%3").arg(osv.major).arg(osv.minor).arg(osv.build)
                << QStringLiteral("src=%1").arg(QLatin1String(osv.source))
                << "product" << osv.productName
                << QStringLiteral("infoFaked=%1").arg(osv.infoFaked ? "true" : "false")
                << "vistaPlus" << osv.vistaPlus;
    }

    bool useSsl = false;
    for (int i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "--ssl") == 0) { useSsl = true; break; }
    }
    QString wsScheme = useSsl ? "wss" : "ws";

    QWebSocket ws;
    // [P2] 修饰键状态必须随连接生命周期走：static 变量跨重连存活，断线时若
    // Ctrl/Alt/Shift 正"按下"，重连后本地状态与真实按键不符 → 修饰键卡死。
    bool ctrlHeld = false, altHeld = false, shiftHeld = false;
    QObject::connect(&ws, &QWebSocket::connected, &app, [&]() {
        qInfo() << "SecureInput: connected to service";
        ctrlHeld = altHeld = shiftHeld = false;
    });
    QObject::connect(&ws, &QWebSocket::disconnected, &app, [&]() {
        qWarning() << "SecureInput: disconnected, exiting";
        QCoreApplication::quit();
    });
    QObject::connect(&ws, &QWebSocket::sslErrors, &app, [&](const QList<QSslError>& errors) {
        for (const auto& err : errors)
            qWarning() << "SecureInput: SSL error" << err.errorString();
        ws.ignoreSslErrors();
    });
    QObject::connect(&ws, &QWebSocket::textMessageReceived, &app, [&](const QString& msg) {
        QJsonDocument doc = QJsonDocument::fromJson(msg.toUtf8());
        if (!doc.isObject()) return;
        QJsonObject obj = doc.object();
        QString type = obj["type"].toString();

        if (type == QStringLiteral("unlock")) {
            // 服务端在锁屏时下发密码，由本进程在安全桌面上直接操作登录对话框。
            // 不能在主线程做：主线程发过 SAS，输入队列已固定，SetThreadDesktop 无效。
            const QString pw = obj["password"].toString();
            if (pw.isEmpty()) {
                qWarning() << "SecureInput: unlock requested without password";
                return;
            }
            qInfo() << "SecureInput: unlock requested";
            // 只允许一条解锁序列：服务端会补发 unlock，重复序列的 SAS 会互相打断
            if (InterlockedCompareExchange(&g_unlockBusy, 1, 0) != 0) {
                qInfo() << "SecureInput: unlock already in progress, ignoring duplicate request";
                return;
            }
            HANDLE th = CreateThread(nullptr, 0, unlockThreadProc, new UnlockCtx { pw, 0 }, 0, nullptr);
            if (th) {
                CloseHandle(th);
            } else {
                InterlockedExchange(&g_unlockBusy, 0);
                qWarning() << "SecureInput: failed to spawn unlock thread, err="
                           << static_cast<int>(GetLastError());
            }
            return;
        }

        if (type == QStringLiteral("sas")) {
            // Secure Attention Sequence：XP 的「安全屏保」锁屏把 UI 放在 winsta0\Screen-saver
            // 安全桌面，普通字符键会被屏保/登录桌面过滤（表现为「密码不正确」）。必须先由
            // 当前输入桌面上的本进程投一次 Ctrl+Alt+Del，把桌面切到 Winlogon 登录框。
            auto sasKey = [](int vk, bool down) {
                INPUT in = {};
                in.type = INPUT_KEYBOARD;
                in.ki.wVk = static_cast<WORD>(vk);
                in.ki.wScan = static_cast<WORD>(MapVirtualKeyW(static_cast<UINT>(vk), MAPVK_VK_TO_VSC));
                if (!down) in.ki.dwFlags = KEYEVENTF_KEYUP;
                SendInput(1, &in, sizeof(INPUT));
            };
            sasKey(VK_CONTROL, true);
            sasKey(VK_MENU, true);
            sasKey(VK_DELETE, true);
            Sleep(50);
            sasKey(VK_DELETE, false);
            sasKey(VK_MENU, false);
            sasKey(VK_CONTROL, false);
            // 复位修饰键跟踪，避免后续普通输入被残留修饰键污染
            ctrlHeld = altHeld = shiftHeld = false;
            return;
        }

        if (obj["isChar"].toBool() && obj["keycode"].toInt() > 0) {
            wchar_t ch = static_cast<wchar_t>(obj["keycode"].toInt());
            bool isDown = (type == "keydown");

            // VkKeyScanW 用当前键盘布局将字符映射为 VK 码（兼容 XP GINA）。
            SHORT vkScan = VkKeyScanW(ch);
            if (vkScan != -1) {
                BYTE vk = vkScan & 0xFF;
                BYTE state = (vkScan >> 8) & 0xFF;
                int n = 0;
                INPUT in[4] = {};
                if (state & 1) { in[n].type = INPUT_KEYBOARD; in[n].ki.wVk = VK_SHIFT; n++; }
                if (state & 2) { in[n].type = INPUT_KEYBOARD; in[n].ki.wVk = VK_CONTROL; n++; }
                if (state & 4) { in[n].type = INPUT_KEYBOARD; in[n].ki.wVk = VK_MENU; n++; }
                in[n].type = INPUT_KEYBOARD; in[n].ki.wVk = vk; n++;
                // Winlogon 安全桌面（XP 锁屏/屏保）按扫描码识别按键：只填 wVk、wScan=0
                // 时字符会被折算错，表现为「密码不正确」。逐个补齐扫描码。
                for (int i = 0; i < n; ++i)
                    in[i].ki.wScan = static_cast<WORD>(MapVirtualKeyW(in[i].ki.wVk, MAPVK_VK_TO_VSC));
                if (isDown) {
                    SendInput(n, in, sizeof(INPUT));
                } else {
                    for (int i = n - 1; i >= 0; i--) {
                        INPUT up = in[i];
                        up.ki.dwFlags = KEYEVENTF_KEYUP;
                        SendInput(1, &up, sizeof(INPUT));
                    }
                }
            } else {
                // VkKeyScanW 失败（如 CJK），回退 Unicode 路径（Win7+）。
                INPUT uni = {};
                uni.type = INPUT_KEYBOARD;
                uni.ki.dwFlags = KEYEVENTF_UNICODE;
                uni.ki.wScan = ch;
                if (!isDown) uni.ki.dwFlags |= KEYEVENTF_KEYUP;
                SendInput(1, &uni, sizeof(INPUT));
            }
            return;
        }

        if (type == "keydown" || type == "keyup") {
            int keycode = obj["keycode"].toInt();
            bool isDown = (type == "keydown");
            bool ctrl = obj["ctrl"].toBool();
            bool alt = obj["alt"].toBool();
            bool shift = obj["shift"].toBool();

            auto sendVk = [](int vk, bool down) {
                INPUT in = {};
                in.type = INPUT_KEYBOARD;
                in.ki.wVk = static_cast<WORD>(vk);
                // 同 isChar 路径：安全桌面按扫描码识别，缺 wScan 会错字/吞键。
                in.ki.wScan = static_cast<WORD>(MapVirtualKeyW(static_cast<UINT>(vk), MAPVK_VK_TO_VSC));
                if (!down) in.ki.dwFlags = KEYEVENTF_KEYUP;
                SendInput(1, &in, sizeof(INPUT));
            };

            if (ctrl != ctrlHeld) { sendVk(VK_CONTROL, ctrl); ctrlHeld = ctrl; }
            if (alt != altHeld) { sendVk(VK_MENU, alt); altHeld = alt; }
            if (shift != shiftHeld) { sendVk(VK_SHIFT, shift); shiftHeld = shift; }

            sendVk(keycode, isDown);
        } else if (type == "mousemove") {
            INPUT in = {};
            in.type = INPUT_MOUSE;
            // 锁屏/Session0 等场景 GetSystemMetrics 可能返回 0，直接做除数会崩掉
            // 安全输入 helper（锁屏输入就此永久失效）。
            const int screenW = GetSystemMetrics(SM_CXSCREEN);
            const int screenH = GetSystemMetrics(SM_CYSCREEN);
            in.mi.dx = screenW > 0 ? obj["x"].toInt() * 65535 / screenW : 0;
            in.mi.dy = screenH > 0 ? obj["y"].toInt() * 65535 / screenH : 0;
            in.mi.dwFlags = MOUSEEVENTF_MOVE | MOUSEEVENTF_ABSOLUTE;
            SendInput(1, &in, sizeof(INPUT));
        } else if (type == "mousedown" || type == "mouseup") {
            bool isDown = (type == "mousedown");
            int x = obj["x"].toInt(), y = obj["y"].toInt();
            int btn = obj["button"].toInt();
            INPUT mv = {};
            mv.type = INPUT_MOUSE;
            const int screenW = GetSystemMetrics(SM_CXSCREEN);
            const int screenH = GetSystemMetrics(SM_CYSCREEN);
            mv.mi.dx = screenW > 0 ? x * 65535 / screenW : 0;
            mv.mi.dy = screenH > 0 ? y * 65535 / screenH : 0;
            mv.mi.dwFlags = MOUSEEVENTF_MOVE | MOUSEEVENTF_ABSOLUTE;
            SendInput(1, &mv, sizeof(INPUT));
            DWORD flags = 0;
            if (btn == 0) flags = isDown ? MOUSEEVENTF_LEFTDOWN : MOUSEEVENTF_LEFTUP;
            else if (btn == 1) flags = isDown ? MOUSEEVENTF_MIDDLEDOWN : MOUSEEVENTF_MIDDLEUP;
            else if (btn == 2) flags = isDown ? MOUSEEVENTF_RIGHTDOWN : MOUSEEVENTF_RIGHTUP;
            if (flags) { INPUT in = {}; in.type = INPUT_MOUSE; in.mi.dwFlags = flags; SendInput(1, &in, sizeof(INPUT)); }
        } else if (type == "wheel") {
            INPUT in = {};
            in.type = INPUT_MOUSE;
            in.mi.mouseData = obj["delta"].toInt() * WHEEL_DELTA;
            in.mi.dwFlags = MOUSEEVENTF_WHEEL;
            SendInput(1, &in, sizeof(INPUT));
        }
    });
    ws.open(QUrl(QString("%1://127.0.0.1:%2/secure-input").arg(wsScheme).arg(wsPort)));
    return app.exec();
#else
    (void)argc; (void)argv; (void)wsPort;
    qCritical("Secure input process is not supported on this platform");
    return 1;
#endif
}
