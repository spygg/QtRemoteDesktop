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
#include <QObject>
#include <QStringList>
#include <QTimer>
#include <QUrl>
#include <QWebSocket>

#ifdef _WIN32
#include <windows.h>
#include <objbase.h>
#include <oleauto.h>
#include <tlhelp32.h>
#include <wtypes.h>
#include <cwchar>
#include <QAbstractSocket>
#endif

// ---------------------------------------------------------------------------
// [B] 跨线程上报的中转对象
//
// 解锁序列跑在 CreateThread 出来的裸线程上（没有 Qt 事件循环）。那条线程里既不能
// 直接碰 QWebSocket（非线程安全），也不能用 QTimer::singleShot 投递到主线程：
// QSingleShotTimer 会在当前线程 startTimer，裸线程没有 event dispatcher，Qt 会打
// 「Timers can only be used with threads started with QThread」并静默丢弃这次上报
// （实测就是这样，服务端根本收不到 unlock_blocked）。所以用最经典的信号槽队列：
// 工作线程对主线程里的本对象调一次队列化的 invokeMethod，真正发送在主线程执行。
// ---------------------------------------------------------------------------
class UnlockReportSink : public QObject
{
    Q_OBJECT
public:
    explicit UnlockReportSink(QWebSocket* ws, QObject* parent = nullptr)
        : QObject(parent)
        , ws_(ws)
    {
    }

    // 任意线程可调：只投递，不碰 socket。
    void post(const QString& payload)
    {
        QMetaObject::invokeMethod(this, "deliver", Qt::QueuedConnection,
                                  Q_ARG(QString, payload));
    }

public slots:
    void deliver(const QString& payload)
    {
        if (ws_ && ws_->state() == QAbstractSocket::ConnectedState)
            ws_->sendTextMessage(payload);
    }

private:
    QWebSocket* ws_;
};


// ---------------------------------------------------------------------------
// 安全桌面解锁：让键鼠真正落在登录 UI 所在的桌面
//
// 前提（服务端 rdpserver_win.cpp startSecureInputProcess() 负责）：
//   本进程是用 lpDesktop="winsta0\<当前输入桌面>"、令牌改到活动控制台会话后拉起的。
//   lpDesktop 决定进程主线程的输入队列绑在哪张桌面，所以主线程的 SendInput
//   （WS 转发的 isChar / keydown / 鼠标）一开始就落在当前输入桌面上。
//     · Vista+：服务在隔离的 Session 0，问不到用户会话的输入桌面
//       （OpenInputDesktop 只会返回 Session 0 自己的 "Default"），所以由用户会话里的
//       helper 随 screen_locked 上报它看到的真实输入桌面（lockedDesktop）：
//         - 普通锁屏 → 登录 UI（LogonUI）在 winsta0\Winlogon，投 Winlogon；
//         - 屏保型锁屏 → 输入桌面是 winsta0\Screen-saver，投 Screen-saver，
//           否则 WS 发键/鼠标都落在一张没人看的桌面上（实测唤不醒屏保）。
//       投错的实测症状：进程起得来、WS 连得上、日志无异常，但锁屏后经 WS 发的
//       字符一个都进不了登录框（0 个圆点）。
//     · XP：无会话隔离，按当时的真实输入桌面（Screen-saver / Welcome / Winlogon）投。
//
// 各时代的解锁手段：
//   Vista+：LogonUI 是 DirectUI 自绘，永远没有 #32770 对话框可写；但主线程的输入队列
//     已经在当前输入桌面上，直接逐字 SendInput + 回车即可。
//     绝不能发 SAS —— 在 Vista+ 上 SAS 会弹出「切换用户/安全选项」，把密码框切走。
//     屏保型锁屏（输入桌面 = Screen-saver）：先投"无字符"的唤醒输入（鼠标微动 +
//     Shift），等桌面切回登录桌面；Win7 实测仅靠合成输入不足以退出安全屏保，再结束
//     .scr。整个过程**不在屏保桌面上键入任何密码字符**，只有桌面切到 Winlogon 并
//     通过 B 校验（IsPassword + LogonUI.exe）后才注入。
//   XP：登录 UI 可能在 Screen-saver/Welcome 安全桌面上，且进程主线程一建就连上队列、
//     之后换不掉，所以两条路径分别处理：
//       - 传统对话框：在新线程里附着当前安全桌面，枚举 #32770 直接写控件
//         （WM_SETTEXT 密码 + WM_COMMAND(IDOK) 提交）。XP 实测：对话框 class=#32770，
//         子控件依次为 用户名 Edit / 密码 Edit / 确定 Button(id=1)；
//       - 欢迎屏幕/屏保：没有可写控件，只能像人一样点磁贴、SendInput 逐字键入，
//         必要时先发 SAS（SAS 由 win32k 特判，不受输入队列/桌面绑定限制）把桌面
//         切到传统登录框。
//   新线程里必须先 SetThreadDesktop 到当前输入桌面，SendInput 才会生效：线程的输入
//   队列在第一次需要队列的调用时就被固定，之后换桌面不会迁移队列。
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

FillResult fillLogonDialog(const QString& username, const QString& password, QString* detail)
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
    DWORD_PTR setRes = 0;
    if (!username.isEmpty()) {
        // 登录模式：把用户名写进第 1 个 Edit（解锁路径传空串，行为不变）。
        const std::wstring wuser = username.toStdWString();
        if (!SendMessageTimeoutW(col.edits[0], WM_SETTEXT, 0,
                                 reinterpret_cast<LPARAM>(wuser.c_str()),
                                 SMTO_ABORTIFHUNG, 2000, &setRes)) {
            *detail = QStringLiteral("username box did not reply within 2s");
            return kRejected;
        }
        if (!setRes) {
            *detail = QStringLiteral("WM_SETTEXT rejected by username box");
            return kRejected;
        }
        Sleep(150);   // 让 msgina 处理完用户名变更再写密码
    }
    HWND pwdEdit = col.edits[1];
    const std::wstring wpwd = password.toStdWString();
    // 必须用 SendMessageTimeout 而不是 SendMessage：这是**跨进程**同步调用，而目标是
    // winlogon/msgina 的窗口。桌面切换的瞬间（刚发过 SAS、旧对话框还在拆）那个线程
    // 可能不再取消息，SendMessage 就永远不返回 —— 本进程的工作线程卡死在这里，
    // BusyGuard 的析构不会执行，g_unlockBusy 永久为 1，之后每次「解锁」都被
    // :499 的守卫直接丢弃；服务侧健康检查看到进程还活着、连接还在，也不会重启它。
    // 表现是"解锁按钮从此再也没反应，直到重启服务"。SMTO_ABORTIFHUNG 让卡住的
    // 目标最多等 2 秒就把控制权还给流程。
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
//
// 字符注入分两条路：
//   · Vista+：可打印字符**一律**走 KEYEVENTF_UNICODE（VK_PACKET + wScan=UTF-16 码元）。
//     旧实现只走 VkKeyScanW，字符映射失败就 `continue` 静默丢掉 —— "中文Ab€@9" 实测
//     只进 4 个圆点（中/文/€ 三个被丢）。Unicode 注入与键盘布局无关，也不产生
//     Shift/Ctrl/Alt 修饰位问题（AltGr 字符不再需要拼 Ctrl+Alt）。
//     一个 INPUT 只承载一个 UTF-16 码元，BMP 外字符（代理对）按两个码元顺序送即可。
//   · XP：保留 VkKeyScanW（GINA/FUS 老路径实测依赖扫描码），但把 VkKeyScanW 吐出的
//     Shift/Ctrl/Alt 三个修饰位**显式**发出来（旧代码只处理 Shift，state&6 被忽略，
//     需要 AltGr 的字符会被打成别的字符）；映射失败也不再静默跳过，计入 dropped。
// 控制键（Enter/Backspace/Tab）两条路都仍走 VK。
// [C] 一次注入的真实结果。typed 是"真的进了输入队列的字符数"，服务端据此判断
// 本轮到底有没有键入（有键入就不再重试，没键入才允许补一轮）。
struct PasswordInjectionStats {
    int typed = 0;     // 真正送进输入队列的字符数（下+上两次 SendInput 都成功）
    int unicode = 0;   // 其中走 KEYEVENTF_UNICODE 的码元数
    int vk = 0;        // 其中走 VK 的字符数（仅 XP）
    int dropped = 0;   // 没送出去的字符数（映射失败 / SendInput 被拒）
    int failed = 0;    // 被拒的 SendInput 调用次数
};

PasswordInjectionStats sendPasswordWithSendInput(const QString& password)
{
    PasswordInjectionStats s;
    const bool useUnicode = isVistaPlusOs();

    auto key = [&s](UINT vk, bool down) -> bool {
        INPUT in = { };
        in.type = INPUT_KEYBOARD;
        in.ki.wVk = static_cast<WORD>(vk);
        in.ki.wScan = static_cast<WORD>(MapVirtualKeyW(vk, MAPVK_VK_TO_VSC));
        if (!down) in.ki.dwFlags = KEYEVENTF_KEYUP;
        const bool ok = SendInput(1, &in, sizeof(INPUT)) == 1;
        if (!ok)
            ++s.failed;
        return ok;
    };

    // KEYEVENTF_UNICODE：wVk 必须为 0（VK_PACKET），字符放在 wScan。
    auto uniKey = [&s](wchar_t unit, bool down) -> bool {
        INPUT in = { };
        in.type = INPUT_KEYBOARD;
        in.ki.wVk = 0;
        in.ki.wScan = unit;
        in.ki.dwFlags = KEYEVENTF_UNICODE | (down ? 0 : KEYEVENTF_KEYUP);
        const bool ok = SendInput(1, &in, sizeof(INPUT)) == 1;
        if (!ok)
            ++s.failed;
        return ok;
    };

    QString droppedText;
    for (const QChar& ch : password) {
        const wchar_t unit = static_cast<wchar_t>(ch.unicode());

        if (useUnicode) {
            const bool downOk = uniKey(unit, true);
            Sleep(30);
            const bool upOk = uniKey(unit, false);
            Sleep(40);
            if (downOk && upOk) {
                ++s.typed;
                ++s.unicode;
            } else {
                ++s.dropped;
                droppedText.append(ch);
            }
            continue;
        }

        // XP 老路径：VkKeyScanW + 扫描码（安全桌面按扫描码认键）。
        const SHORT vkScan = VkKeyScanW(unit);
        if (vkScan == -1) {
            // 不再静默 continue：CJK/符号在 XP 布局上就是打不出来，必须如实计数上报。
            ++s.dropped;
            droppedText.append(ch);
            continue;
        }
        const BYTE vk = static_cast<BYTE>(vkScan & 0xFF);
        const BYTE state = static_cast<BYTE>((vkScan >> 8) & 0xFF);
        // VkKeyScanW 的修饰位：1=Shift，2=Ctrl，4=Alt（AltGr 常被报成 Ctrl+Alt）。
        const bool needShift = (state & 1) != 0;
        const bool needCtrl = (state & 2) != 0;
        const bool needAlt = (state & 4) != 0;
        if (needShift) key(VK_SHIFT, true);
        if (needCtrl) key(VK_CONTROL, true);
        if (needAlt) key(VK_MENU, true);
        const bool downOk = key(vk, true);
        Sleep(30);
        const bool upOk = key(vk, false);
        if (needAlt) key(VK_MENU, false);
        if (needCtrl) key(VK_CONTROL, false);
        if (needShift) key(VK_SHIFT, false);
        Sleep(40);
        if (downOk && upOk) {
            ++s.typed;
            ++s.vk;
        } else {
            ++s.dropped;
            droppedText.append(ch);
        }
    }

    // 提交：Enter 走 VK（不是"可打印字符"，不进 Unicode 路径）。
    key(VK_RETURN, true);
    Sleep(30);
    key(VK_RETURN, false);

    // 日志只报真实数字：typed 是实际进框的字符数，和 password.size() 分开写，
    // 以后一眼就能看出有没有丢字符、丢在哪条路上。
    qInfo() << "SecureInput: typed password (typed =" << s.typed << "of" << password.size()
            << "chars; unicode =" << s.unicode << ", vk =" << s.vk
            << ", dropped =" << s.dropped << ", rejected key events =" << s.failed
            << ", mode =" << (useUnicode ? "unicode" : "vk") << ")";
    if (s.dropped > 0) {
        QStringList cps;
        for (const QChar& c : droppedText)
            cps << QStringLiteral("U+%1").arg(static_cast<uint>(c.unicode()), 4, 16, QChar('0')).toUpper();
        qWarning() << "SecureInput: password injection dropped" << s.dropped
                   << "char(s):" << cps.join(QStringLiteral(", "));
    }
    return s;
}

// [C] 注入前先把密码框清空：LogonUI 上 Ctrl+A 不可靠（实测只删一个字符），
// 只能靠连续 Backspace。框本来就空时这些 Backspace 是无害的（DirectUI 忽略），
// 所以无条件先清，保证「框里有残留字符也能解开」。
// 代价实测：20 次 × ~20ms ≈ 0.4s（含每次 down+up 两次事件的 SendInput 开销）。
int clearPasswordField()
{
    const int kBackspaces = 20;
    const int kGapMs = 20;
    int sent = 0;
    for (int i = 0; i < kBackspaces; ++i) {
        INPUT in[2] = { };
        in[0].type = INPUT_KEYBOARD;
        in[0].ki.wVk = VK_BACK;
        in[0].ki.wScan = static_cast<WORD>(MapVirtualKeyW(VK_BACK, MAPVK_VK_TO_VSC));
        in[1].type = INPUT_KEYBOARD;
        in[1].ki.wVk = VK_BACK;
        in[1].ki.wScan = in[0].ki.wScan;
        in[1].ki.dwFlags = KEYEVENTF_KEYUP;
        if (SendInput(2, in, sizeof(INPUT)) == 2)
            ++sent;
        Sleep(kGapMs);
    }
    return sent;
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
    // [C] 服务端这一轮解锁的代号：progress 上报原样带回。服务端用它丢弃过期上报
    // （用户连点两次解锁时，第一轮的 typed/blocked 绝不能影响第二轮的状态机）。
    int seq;
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

// 屏保型锁屏的唤醒输入：**只发不会产生任何字符**的事件 —— 两次相对鼠标微动 +
// Shift 按下/抬起。目的只是让 Winlogon 认为"用户有活动"，把输入桌面从
// Screen-saver 切回登录桌面。
//
// 这里刻意不发：
//   · 任何可打印键（哪怕密码字符的第一个）—— 屏保桌面上不允许出现"字符被打进
//     任何窗口"的痕迹；
//   · 鼠标左右键 —— 屏保背后可能压着用户的普通窗口，点击会激活/触发它。
// Shift 单独按下/抬起不会输入文本，也不会触发快捷键（没有其它键同时按下）。
bool wakeScreenSaverDesktop()
{
    INPUT in[4] = { };
    in[0].type = INPUT_MOUSE;
    in[0].mi.dx = 2;
    in[0].mi.dy = 2;
    in[0].mi.dwFlags = MOUSEEVENTF_MOVE;
    in[1].type = INPUT_MOUSE;
    in[1].mi.dx = -2;
    in[1].mi.dy = -2;
    in[1].mi.dwFlags = MOUSEEVENTF_MOVE;
    in[2].type = INPUT_KEYBOARD;
    in[2].ki.wVk = VK_SHIFT;
    in[2].ki.wScan = static_cast<WORD>(MapVirtualKeyW(VK_SHIFT, MAPVK_VK_TO_VSC));
    in[3].type = INPUT_KEYBOARD;
    in[3].ki.wVk = VK_SHIFT;
    in[3].ki.wScan = in[2].ki.wScan;
    in[3].ki.dwFlags = KEYEVENTF_KEYUP;
    const UINT sent = SendInput(4, in, sizeof(INPUT));
    qInfo() << "SecureInput: screen-saver wake input sent (events =" << sent << "of 4 )";
    return sent == 4;
}

// 等输入桌面离开 Screen-saver。Winlogon 收掉屏保后通常切到 Winlogon；如果用户
// 本地已经动过鼠标（或屏保本来就不需要密码），也可能直接回 Default。
// 返回最后一次读到的桌面名（空 = OpenInputDesktop 一直失败，问不到）。
QString waitForLogonDesktop(int ms)
{
    QString last;
    for (int waited = 0; waited < ms; waited += 200) {
        last = currentInputDesktopName();
        if (!last.isEmpty() && !isScreensaverDesktop(last)) {
            qInfo() << "SecureInput: input desktop left Screen-saver ->" << last;
            return last;
        }
        Sleep(200);
    }
    return currentInputDesktopName();
}

// ---------------------------------------------------------------------------
// [B] 注入前目标校验（仅 Vista+，XP 不走这里）
//
// 为什么需要：Vista+ 的解锁只能靠 SendInput（LogonUI 是 DirectUI 自绘，没有可写的
// #32770 控件）。一旦眼前的界面不是"登录密码框"——上一轮密码被拒后弹着模态
// 「用户名或密码不正确」、屏保还盖在上面、或者机器其实已经解锁只剩普通桌面——
// 盲打就是把明文密码敲进弹窗或用户正在用的窗口里。
//
// 判据（高档为主）：UI Automation 的 FocusedElement，同时满足
//   IsPassword == true  且  ProcessId 归属于 LogonUI.exe
// 2026-11-04 在本机取证：LogonUI 密码框的 Name 是本地化字符串（"密码"）、
// ControlType 报成 Pane、AutomationId 为空、没有 ValuePattern —— 这些都不能当判据，
// 只有 IsPassword + 进程归属稳定（45ms 量级，桌面 IsPassword 元素数=1）。
//
// 低档只做**否定**：前台窗口进程不是 LogonUI.exe 就直接判"不是登录界面"，不注入。
// 低档永远不放行，放行只由高档（UIA）决定。
//
// 调用放在独立线程里、最多等 500ms：UIA 是跨进程 provider，遇到目标线程卡住会一直
// 不返回。超时/异常/空元素一律按"不确定"处理 —— 不注入，并把原因如实上报服务端
// （前端 unlock_result 的文案要能区分"焦点不在密码框"和"未能确认"）。
//
// 实测（probe_uia_thread.py，同一台 Win7 锁屏、焦点在空密码框）：
//   主线程（进程生来在 Winlogon）        GetFocusedElement  21.3ms → IsPassword=true pid=1964 LogonUI.exe
//   新线程（继承 Winlogon 桌面、不 Set）  4.0ms → 同上
//   新线程 SetThreadDesktop(Winlogon)    3.2ms → 同上
// 结论：解锁/注入线程先 SetThreadDesktop 再初始化 UIA 完全可用，不需要"进程启动时
// 初始化 UIA 再把结果缓存给注入线程"的额外结构。
// ---------------------------------------------------------------------------

enum : int {
    kUiaProcessIdProperty = 30002,
    kUiaControlTypeProperty = 30003,
    kUiaNameProperty = 30005,
    kUiaBoundingRectangleProperty = 30001,
    kUiaIsEnabledProperty = 30010,
    kUiaAutomationIdProperty = 30011,
    kUiaIsPasswordProperty = 30019,
};

// 控件类型（登录界面磁贴实测是 ListItem，部分系统版本/主题报 Button）
enum : int {
    kUiaButtonControlType = 50000,
    kUiaListItemControlType = 50007,
};

// 模式 id：Invoke / SelectionItem
enum : int {
    kUiaInvokePattern = 10000,
    kUiaSelectionItemPattern = 10010,
};

const GUID kClsidCUIAutomation = { 0xff48dba4, 0x60ef, 0x4201,
                                   { 0xaa, 0x87, 0x54, 0x10, 0x3e, 0xef, 0x59, 0x4e } };
const GUID kIidIUIAutomation = { 0x30cbe57d, 0xd9d0, 0x452a,
                                 { 0xab, 0x13, 0x7a, 0xc5, 0xac, 0x48, 0x25, 0xee } };

// 只按 vtable 槽位调用，不引入 uiautomationclient.h（本工程的旧 MinGW 没有该头/导入
// 库）。槽位顺序取自 MS 官方定义，IUnknown 占 0-2。
// AddRef/Release 必须写出真实签名（不能留 void*）：两类接口的 Release 都由
// uiaRelease() 调用，槽位 2 在 IUIAutomation / IUIAutomationElement 上位置相同。
typedef unsigned long (STDMETHODCALLTYPE* UiaAddRefFn)(void* self);
typedef unsigned long (STDMETHODCALLTYPE* UiaReleaseFn)(void* self);

struct UiaAutomationVtbl {
    void* queryInterface;        // 0
    UiaAddRefFn addRef;          // 1
    UiaReleaseFn release;        // 2
    void* compareElements;       // 3
    void* compareRuntimeIds;     // 4
    HRESULT (STDMETHODCALLTYPE* getRootElement)(void* self, void** element);     // 5
    void* elementFromHandle;     // 6
    void* elementFromPoint;      // 7
    HRESULT (STDMETHODCALLTYPE* getFocusedElement)(void* self, void** element);   // 8
};

struct UiaElementVtbl {
    void* queryInterface;        // 0
    UiaAddRefFn addRef;          // 1
    UiaReleaseFn release;        // 2
    void* setFocus;              // 3
    void* getRuntimeId;          // 4
    void* findFirst;             // 5
    void* findAll;               // 6
    void* findFirstBuildCache;   // 7
    void* findAllBuildCache;     // 8
    void* buildUpdatedCache;     // 9
    HRESULT (STDMETHODCALLTYPE* getCurrentPropertyValue)(void* self, int propId, void* value); // 10
    void* getCurrentPropertyValueEx; // 11
    void* getCachedPropertyValue;    // 12
    void* getCachedPropertyValueEx;  // 13
    void* getCurrentPatternAs;       // 14
    void* getCachedPatternAs;        // 15
    HRESULT (STDMETHODCALLTYPE* getCurrentPattern)(void* self, int patternId, void** pattern); // 16
};

// IUIAutomationTreeWalker：控制视图遍历（3=父 4=首子 5=末子 6=下一兄弟）
struct UiaWalkerVtbl {
    void* queryInterface;        // 0
    UiaAddRefFn addRef;          // 1
    UiaReleaseFn release;        // 2
    HRESULT (STDMETHODCALLTYPE* getParentElement)(void* self, void* elem, void** out);      // 3
    HRESULT (STDMETHODCALLTYPE* getFirstChildElement)(void* self, void* elem, void** out); // 4
    HRESULT (STDMETHODCALLTYPE* getLastChildElement)(void* self, void* elem, void** out);  // 5
    HRESULT (STDMETHODCALLTYPE* getNextSiblingElement)(void* self, void* elem, void** out); // 6
};

// UIA 模式接口（Invoke / SelectionItem）只需要前几个槽位
struct UiaPatternVtbl {
    void* queryInterface;        // 0
    UiaAddRefFn addRef;          // 1
    UiaReleaseFn release;        // 2
    HRESULT (STDMETHODCALLTYPE* action)(void* self);   // 3 (Invoke / Select)
};

UiaWalkerVtbl* uiaWalkerVtbl(void* p)
{
    return *reinterpret_cast<UiaWalkerVtbl**>(p);
}

UiaPatternVtbl* uiaPatternVtbl(void* p)
{
    return *reinterpret_cast<UiaPatternVtbl**>(p);
}

UiaAutomationVtbl* uiaAutomationVtbl(void* p)
{
    return *reinterpret_cast<UiaAutomationVtbl**>(p);
}

UiaElementVtbl* uiaElementVtbl(void* p)
{
    return *reinterpret_cast<UiaElementVtbl**>(p);
}

void uiaRelease(void* p)
{
    if (p)
        uiaAutomationVtbl(p)->release(p);   // 0-2 槽位在两类接口上位置相同
}

// 取 UIA 属性。VARIANT 的 vt 在 offset 0、union 在 offset 8（32/64 位一致）。
// 这里只读 VT_I4 / VT_BOOL，不产生分配，所以不必 VariantClear。
bool readUiaInt(void* elem, int propId, int* out)
{
    unsigned char var[32] = { 0 };
    if (FAILED(uiaElementVtbl(elem)->getCurrentPropertyValue(elem, propId, var)))
        return false;
    if (*reinterpret_cast<const short*>(var) != VT_I4)
        return false;
    *out = *reinterpret_cast<const int*>(var + 8);
    return true;
}

bool readUiaBool(void* elem, int propId, bool* out)
{
    unsigned char var[32] = { 0 };
    if (FAILED(uiaElementVtbl(elem)->getCurrentPropertyValue(elem, propId, var)))
        return false;
    if (*reinterpret_cast<const short*>(var) != VT_BOOL)
        return false;
    *out = *reinterpret_cast<const short*>(var + 8) != 0;
    return true;
}

// 读 VT_BSTR 属性（磁贴的 Name 就是用户名）。BSTR 由 provider 分配，
// 必须 VariantClear 释放，否则每次登录都泄漏一小段内存。
bool readUiaString(void* elem, int propId, QString* out)
{
    VARIANT var;
    VariantInit(&var);
    if (FAILED(uiaElementVtbl(elem)->getCurrentPropertyValue(elem, propId, &var)))
        return false;
    bool ok = false;
    if (var.vt == VT_BSTR) {
        if (var.bstrVal)
            *out = QString::fromWCharArray(var.bstrVal);
        else
            out->clear();
        ok = true;
    }
    VariantClear(&var);
    return ok;
}

// 读 VT_ARRAY|VT_R8 的 BoundingRectangle（left, top, width, height）。
// 磁贴 Invoke 不生效时用它算点击坐标（实测 Win7 磁贴 Name==用户名）。
bool readUiaRect(void* elem, RECT* out)
{
    VARIANT var;
    VariantInit(&var);
    if (FAILED(uiaElementVtbl(elem)->getCurrentPropertyValue(elem, kUiaBoundingRectangleProperty, &var)))
        return false;
    bool ok = false;
    if (var.vt == (VT_ARRAY | VT_R8) && var.parray) {
        double* p = nullptr;
        if (SUCCEEDED(SafeArrayAccessData(var.parray, reinterpret_cast<void**>(&p)))) {
            out->left = static_cast<LONG>(p[0]);
            out->top = static_cast<LONG>(p[1]);
            out->right = out->left + static_cast<LONG>(p[2]);
            out->bottom = out->top + static_cast<LONG>(p[3]);
            SafeArrayUnaccessData(var.parray);
            ok = (out->right > out->left && out->bottom > out->top);
        }
    }
    VariantClear(&var);
    return ok;
}

// 登录界面用户磁贴查找（仅 Vista+）。
// 实测 Win7 登录界面：磁贴是 ControlType=ListItem、Name==用户名、class=UserTile；
// 找不到就返回 user_not_found，绝不对着未知界面盲打密码。
enum TileFindResult {
    kTileFound = 0,
    kTileNotFound = 1,
    kTileUncertain = 2,
};

struct TileHit {
    void* element = nullptr;   // 命中的 UIA 元素（调用方负责 Release）
    QString name;
    int controlType = 0;
    RECT rect = { 0, 0, 0, 0 };
};

void scanTileTree(void* walker, void* elem, const QString& user, TileHit* hit,
                  int depth, int* nodes)
{
    if (hit->element || depth > 8 || *nodes > 600)
        return;
    void* child = nullptr;
    if (FAILED(uiaWalkerVtbl(walker)->getFirstChildElement(walker, elem, &child)) || !child)
        return;
    while (child) {
        ++(*nodes);
        int ctype = 0;
        readUiaInt(child, kUiaControlTypeProperty, &ctype);
        if (ctype == kUiaListItemControlType || ctype == kUiaButtonControlType) {
            QString nm;
            if (readUiaString(child, kUiaNameProperty, &nm)) {
                nm = nm.trimmed();
                if (!nm.isEmpty() && nm.compare(user, Qt::CaseInsensitive) == 0) {
                    hit->element = child;   // 引用转交调用方，此处不 Release
                    hit->name = nm;
                    hit->controlType = ctype;
                    readUiaRect(child, &hit->rect);
                    return;
                }
            }
        }
        scanTileTree(walker, child, user, hit, depth + 1, nodes);
        if (hit->element)
            return;   // 已命中：当前 child 的引用已转交，不能再 Release
        void* next = nullptr;
        if (FAILED(uiaWalkerVtbl(walker)->getNextSiblingElement(walker, child, &next)))
            next = nullptr;
        uiaRelease(child);
        child = next;
    }
}

TileFindResult findUserTile(const QString& user, TileHit* hit, QString* detail)
{
    hit->element = nullptr;
    const HRESULT hrInit = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if (FAILED(hrInit)) {
        *detail = QStringLiteral("CoInitializeEx 失败, hr=0x%1")
                      .arg(static_cast<quint32>(hrInit), 8, 16, QLatin1Char('0'));
        return kTileUncertain;
    }
    void* uia = nullptr;
    const HRESULT hrCreate = CoCreateInstance(kClsidCUIAutomation, nullptr,
                                              CLSCTX_INPROC_SERVER,
                                              kIidIUIAutomation, &uia);
    if (FAILED(hrCreate) || !uia) {
        CoUninitialize();
        *detail = QStringLiteral("创建 UIAutomation 失败, hr=0x%1")
                      .arg(static_cast<quint32>(hrCreate), 8, 16, QLatin1Char('0'));
        return kTileUncertain;
    }
    void* root = nullptr;
    const HRESULT hrRoot = uiaAutomationVtbl(uia)->getRootElement(uia, &root);
    if (FAILED(hrRoot) || !root) {
        uiaRelease(uia);
        CoUninitialize();
        *detail = QStringLiteral("取 UIA 根元素失败, hr=0x%1")
                      .arg(static_cast<quint32>(hrRoot), 8, 16, QLatin1Char('0'));
        return kTileUncertain;
    }

    // ControlViewWalker 是 IUIAutomation 的属性 getter，槽位 14。
    typedef HRESULT (STDMETHODCALLTYPE* GetWalkerFn)(void* self, void** out);
    GetWalkerFn getWalker =
        reinterpret_cast<GetWalkerFn>(reinterpret_cast<void**>(uiaAutomationVtbl(uia))[14]);
    void* walker = nullptr;
    const HRESULT hrWalk = getWalker ? getWalker(uia, &walker) : E_FAIL;
    if (FAILED(hrWalk) || !walker) {
        uiaRelease(root);
        uiaRelease(uia);
        CoUninitialize();
        *detail = QStringLiteral("创建 UIA TreeWalker 失败, hr=0x%1")
                      .arg(static_cast<quint32>(hrWalk), 8, 16, QLatin1Char('0'));
        return kTileUncertain;
    }

    int nodes = 0;
    scanTileTree(walker, root, user, hit, 0, &nodes);
    uiaRelease(walker);
    uiaRelease(root);
    uiaRelease(uia);
    CoUninitialize();

    if (hit->element) {
        *detail = QStringLiteral("找到磁贴 name=%1 type=%2 rect=%3,%4,%5,%6")
                      .arg(hit->name).arg(hit->controlType)
                      .arg(hit->rect.left).arg(hit->rect.top)
                      .arg(hit->rect.right).arg(hit->rect.bottom);
        return kTileFound;
    }
    *detail = QStringLiteral("遍历 %1 个元素，没有 Name==%2 的用户磁贴").arg(nodes).arg(user);
    return kTileNotFound;
}

// 选中磁贴：优先 UIA 模式（Invoke / SelectionItem），不行再按 BoundingRectangle 点击。
bool activateTile(const TileHit& hit, QString* how)
{
    if (hit.element) {
        void* pat = nullptr;
        if (SUCCEEDED(uiaElementVtbl(hit.element)->getCurrentPattern(
                hit.element, kUiaInvokePattern, &pat)) && pat) {
            const HRESULT hr = uiaPatternVtbl(pat)->action(pat);
            uiaRelease(pat);
            if (SUCCEEDED(hr)) {
                *how = QStringLiteral("UIA Invoke");
                return true;
            }
        }
        pat = nullptr;
        if (SUCCEEDED(uiaElementVtbl(hit.element)->getCurrentPattern(
                hit.element, kUiaSelectionItemPattern, &pat)) && pat) {
            const HRESULT hr = uiaPatternVtbl(pat)->action(pat);
            uiaRelease(pat);
            if (SUCCEEDED(hr)) {
                *how = QStringLiteral("UIA SelectionItem.Select");
                return true;
            }
        }
    }
    if (hit.rect.right > hit.rect.left && hit.rect.bottom > hit.rect.top) {
        const int cx = (hit.rect.left + hit.rect.right) / 2;
        const int cy = (hit.rect.top + hit.rect.bottom) / 2;
        if (clickScreen(cx, cy)) {
            *how = QStringLiteral("click tile at %1,%2").arg(cx).arg(cy);
            return true;
        }
    }
    *how = QStringLiteral("磁贴既没有可用的 UIA 模式，也没有可点击坐标");
    return false;
}

// ---------------------------------------------------------------------------
// 登录界面错误提示的自动收敛（仅 Vista+，登录模式专用）
//
// 2026-11-05 在 Win7 生产机上取证（probe_dialog.ps1，SYSTEM + 控制台会话 +
// winsta0\Winlogon）：
//
//   * 密码错误后的提示**不是**独立的 #32770 顶层窗口。winsta0\Winlogon 桌面上
//     只有一个顶层窗口（class = "AUTHUI.DLL: LogonUI Logon Window"），错误提示是
//     LogonUI 在同一窗口里内联渲染的 DirectUI 元素：
//         Text   aid=ShortMessage  （本地化文本，如"用户名或密码不正确。"）
//         Button aid=Ok           （本地化 Name，如"确定"，IsEnabled=true，有 Invoke）
//   * Invoke 掉 aid=Ok 后不会回到磁贴列表，而是回到该用户的"密码页"：
//         Pane      IsPassword=true（RestrictedEdit）
//         Button    aid=Submit
//         Button    aid=OtherTiles（本地化 Name"切换用户"）
//     Invoke 掉 aid=OtherTiles 才真正回到用户磁贴列表。
//
// 因此收敛用两个**稳定**判据（AutomationId，与本地化无关）：
//   1) aid=Ok 的 Button（错误提示的确定按钮）；
//   2) aid=OtherTiles 的 Button（密码页的切换用户）。
// 只使用 UIA Invoke，不做坐标点击。收不掉/找不到按钮就回 stuck_dialog，
// 绝不往任何弹窗里键入密码。
//
// 语义：本函数是**幂等**的"整理界面"，只在下一轮新请求开始时调用；
// 绝不在 wrong_password 之后自动重放密码（避免连环尝试）。
// ---------------------------------------------------------------------------
enum ConvergeResult {
    kConvergeNone = 0,      // 界面本来就是磁贴列表 / 没有可收敛的东西
    kConvergeDone = 1,      // 已把错误提示/密码页收敛回磁贴列表
    kConvergeFailed = 2,    // 检测到错误界面但收不掉 → stuck_dialog，不输入
};

struct UiaTreeCtx {
    bool comInit = false;
    void* uia = nullptr;
    void* root = nullptr;
    void* walker = nullptr;
};

void closeUiaTree(UiaTreeCtx* t)
{
    if (t->walker)
        uiaRelease(t->walker);
    if (t->root)
        uiaRelease(t->root);
    if (t->uia)
        uiaRelease(t->uia);
    t->walker = t->root = t->uia = nullptr;
    if (t->comInit) {
        CoUninitialize();
        t->comInit = false;
    }
}

bool openUiaTree(UiaTreeCtx* t, QString* err)
{
    const HRESULT hrInit = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if (FAILED(hrInit)) {
        *err = QStringLiteral("CoInitializeEx 失败, hr=0x%1")
                   .arg(static_cast<quint32>(hrInit), 8, 16, QLatin1Char('0'));
        return false;
    }
    t->comInit = true;
    const HRESULT hrCreate = CoCreateInstance(kClsidCUIAutomation, nullptr,
                                              CLSCTX_INPROC_SERVER,
                                              kIidIUIAutomation, &t->uia);
    if (FAILED(hrCreate) || !t->uia) {
        closeUiaTree(t);
        *err = QStringLiteral("创建 UIAutomation 失败, hr=0x%1")
                   .arg(static_cast<quint32>(hrCreate), 8, 16, QLatin1Char('0'));
        return false;
    }
    const HRESULT hrRoot = uiaAutomationVtbl(t->uia)->getRootElement(t->uia, &t->root);
    if (FAILED(hrRoot) || !t->root) {
        closeUiaTree(t);
        *err = QStringLiteral("取 UIA 根元素失败, hr=0x%1")
                   .arg(static_cast<quint32>(hrRoot), 8, 16, QLatin1Char('0'));
        return false;
    }
    typedef HRESULT (STDMETHODCALLTYPE* GetWalkerFn)(void* self, void** out);
    GetWalkerFn getWalker =
        reinterpret_cast<GetWalkerFn>(reinterpret_cast<void**>(uiaAutomationVtbl(t->uia))[14]);
    const HRESULT hrWalk = getWalker ? getWalker(t->uia, &t->walker) : E_FAIL;
    if (FAILED(hrWalk) || !t->walker) {
        closeUiaTree(t);
        *err = QStringLiteral("创建 UIA TreeWalker 失败, hr=0x%1")
                   .arg(static_cast<quint32>(hrWalk), 8, 16, QLatin1Char('0'));
        return false;
    }
    return true;
}

// 按 AutomationId 找元素（wantType=0 表示不限控件类型）。aid 是 LogonUI 内建的
// 稳定标识；Name 会随系统语言变化，不能用来定位。
void scanTreeByAid(void* walker, void* elem, const QString& aid, int wantType,
                   void** found, int depth, int* nodes)
{
    if (*found || depth > 12 || *nodes > 800)
        return;
    void* child = nullptr;
    if (FAILED(uiaWalkerVtbl(walker)->getFirstChildElement(walker, elem, &child)) || !child)
        return;
    while (child) {
        ++(*nodes);
        QString a;
        if (readUiaString(child, kUiaAutomationIdProperty, &a)
                && a.compare(aid, Qt::CaseInsensitive) == 0) {
            int ctype = 0;
            readUiaInt(child, kUiaControlTypeProperty, &ctype);
            if (wantType == 0 || ctype == wantType) {
                *found = child;   // 引用转交调用方
                return;
            }
        }
        scanTreeByAid(walker, child, aid, wantType, found, depth + 1, nodes);
        if (*found)
            return;
        void* next = nullptr;
        if (FAILED(uiaWalkerVtbl(walker)->getNextSiblingElement(walker, child, &next)))
            next = nullptr;
        uiaRelease(child);
        child = next;
    }
}

void* findAidElement(UiaTreeCtx* t, const QString& aid, int wantType)
{
    int nodes = 0;
    void* found = nullptr;
    scanTreeByAid(t->walker, t->root, aid, wantType, &found, 0, &nodes);
    return found;
}

bool invokeUiaElement(void* elem, QString* how)
{
    void* pat = nullptr;
    if (SUCCEEDED(uiaElementVtbl(elem)->getCurrentPattern(elem, kUiaInvokePattern, &pat))
            && pat) {
        const HRESULT hr = uiaPatternVtbl(pat)->action(pat);
        uiaRelease(pat);
        if (SUCCEEDED(hr)) {
            *how = QStringLiteral("UIA Invoke");
            return true;
        }
        *how = QStringLiteral("UIA Invoke 返回 hr=0x%1")
                   .arg(static_cast<quint32>(hr), 8, 16, QLatin1Char('0'));
        return false;
    }
    *how = QStringLiteral("没有 Invoke 模式");
    return false;
}

ConvergeResult convergeLogonUiState(QString* detail)
{
    UiaTreeCtx t;
    QString err;
    if (!openUiaTree(&t, &err)) {
        // UIA 起不来时无法知道界面长什么样；交给后面的 findUserTile 按原语义
        // 回 uncertain，这里不冒充 stuck_dialog（也绝不盲打）。
        qWarning() << "SecureInput: converge UIA unavailable -" << err;
        *detail = err;
        return kConvergeNone;
    }

    // 等待指定 aid 从界面消失（Invoke 后的转场是异步的）。
    auto gone = [&](const QString& aid, int ms) -> bool {
        const DWORD end = GetTickCount() + static_cast<DWORD>(ms);
        while (GetTickCount() < end) {
            void* e = findAidElement(&t, aid, kUiaButtonControlType);
            if (!e)
                return true;
            uiaRelease(e);
            Sleep(100);
        }
        void* e = findAidElement(&t, aid, kUiaButtonControlType);
        if (e) {
            uiaRelease(e);
            return false;
        }
        return true;
    };

    bool acted = false;
    QString errorText;
    void* msg = findAidElement(&t, QStringLiteral("ShortMessage"), 0);
    if (msg) {
        readUiaString(msg, kUiaNameProperty, &errorText);
        uiaRelease(msg);
    }

    // 1) 错误提示：aid=Ok 的确定按钮（不按 Name 硬编码"确定"/"OK"）。
    void* okBtn = findAidElement(&t, QStringLiteral("Ok"), kUiaButtonControlType);
    if (okBtn) {
        bool enabled = true;
        readUiaBool(okBtn, kUiaIsEnabledProperty, &enabled);
        if (!enabled) {
            uiaRelease(okBtn);
            closeUiaTree(&t);
            *detail = QStringLiteral("错误提示的确定按钮不可用（disabled），未点击：%1")
                          .arg(errorText);
            return kConvergeFailed;
        }
        QString how;
        const bool invoked = invokeUiaElement(okBtn, &how);
        uiaRelease(okBtn);
        if (!invoked) {
            closeUiaTree(&t);
            *detail = QStringLiteral("发现错误提示（%1），但确定按钮点不了（%2），未输入密码")
                          .arg(errorText.isEmpty() ? QStringLiteral("无文本") : errorText, how);
            return kConvergeFailed;
        }
        acted = true;
        if (!gone(QStringLiteral("Ok"), 3000)) {
            closeUiaTree(&t);
            *detail = QStringLiteral("已点击错误提示的确定按钮，但提示 3 秒内没有消失（%1）")
                          .arg(errorText);
            return kConvergeFailed;
        }
        qInfo() << "SecureInput: dismissed logon error dialog -" << errorText;
    }

    // 2) 密码页：aid=OtherTiles 的"切换用户"。点掉错误提示后 Win7 会停在密码页，
    //    必须再切换一次才回到磁贴列表；若上一轮遗留密码页（没有弹窗）同样处理。
    void* switchBtn = nullptr;
    if (acted) {
        // 错误提示消失后密码页要几百 ms 才出现，给它一个等待窗口。
        const DWORD end = GetTickCount() + 2500;
        while (GetTickCount() < end) {
            switchBtn = findAidElement(&t, QStringLiteral("OtherTiles"), kUiaButtonControlType);
            if (switchBtn)
                break;
            Sleep(100);
        }
    } else {
        switchBtn = findAidElement(&t, QStringLiteral("OtherTiles"), kUiaButtonControlType);
    }
    if (switchBtn) {
        bool enabled = true;
        readUiaBool(switchBtn, kUiaIsEnabledProperty, &enabled);
        if (!enabled) {
            uiaRelease(switchBtn);
            closeUiaTree(&t);
            *detail = QStringLiteral("登录界面停在密码页，但「切换用户」按钮不可用，未输入密码");
            return kConvergeFailed;
        }
        QString how;
        const bool invoked = invokeUiaElement(switchBtn, &how);
        uiaRelease(switchBtn);
        if (!invoked) {
            closeUiaTree(&t);
            *detail = QStringLiteral("登录界面停在密码页，但「切换用户」点不了（%1），未输入密码")
                          .arg(how);
            return kConvergeFailed;
        }
        acted = true;
        if (!gone(QStringLiteral("OtherTiles"), 3000)
                || !gone(QStringLiteral("Submit"), 2000)) {
            closeUiaTree(&t);
            *detail = QStringLiteral("已点击「切换用户」，但 3 秒内没有回到用户磁贴列表，未输入密码");
            return kConvergeFailed;
        }
        qInfo() << "SecureInput: switched from credential page back to tile list";
    }

    closeUiaTree(&t);
    if (!acted) {
        *detail = QStringLiteral("登录界面无需收敛（已在磁贴列表）");
        return kConvergeNone;
    }
    *detail = QStringLiteral("已自动关掉错误提示并切回用户磁贴列表");
    return kConvergeDone;
}

// 进程 id → 可执行文件名（不含路径）。用 tlhelp32 而不是
// QueryFullProcessImageNameW：后者在旧 MinGW 里要额外链 psapi，而快照本来就在用。
QString processExeName(DWORD pid)
{
    if (!pid)
        return QString();
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE)
        return QString();
    PROCESSENTRY32W pe = { sizeof(pe) };
    QString out;
    if (Process32FirstW(snap, &pe)) {
        do {
            if (pe.th32ProcessID == pid) {
                out = QString::fromWCharArray(pe.szExeFile);
                break;
            }
        } while (Process32NextW(snap, &pe));
    }
    CloseHandle(snap);
    return out;
}

// 屏保被唤醒后，Winlogon 桌面先出现、LogonUI 的前台窗口稍后才建出来。这段空窗里
// GetForegroundWindow() 返回 NULL（B 校验会立即判 uncertain 并拒绝注入 —— 实测
// 唤醒后 ~0.85s 正好命中）。这里**只**在"刚从屏保唤醒"的路径上等 LogonUI 建出
// 前台窗口；等不到仍按 B 的原判据判 uncertain，不放宽任何放行条件（是否真的在
// 密码框上，仍由 validatePasswordTarget 的 IsPassword + LogonUI.exe 决定）。
bool waitForLogonUiForeground(int ms)
{
    for (int waited = 0; waited < ms; waited += 200) {
        HWND fg = GetForegroundWindow();
        DWORD pid = 0;
        if (fg)
            GetWindowThreadProcessId(fg, &pid);
        if (pid && processExeName(pid).compare(QLatin1String("LogonUI.exe"),
                                               Qt::CaseInsensitive) == 0) {
            qInfo() << "SecureInput: LogonUI foreground appeared after" << waited << "ms";
            return true;
        }
        Sleep(200);
    }
    return false;
}

enum TargetVerdict {
    kTargetPasswordBox = 0,   // 焦点确实在 LogonUI 密码框 → 允许注入
    kTargetNotLogonUi = 1,    // 明确不是登录界面 → 拒绝（"未通过验证"）
    kTargetUncertain = 2,     // 超时/异常/拿不到焦点 → 拒绝（"未能确认"）
};

// 校验线程的执行阶段：超时时把它写进原因里，便于区分"卡在绑桌面"和"卡在 UIA"。
enum TargetStage {
    kStageStart = 0,
    kStageDesktopOpened = 1,
    kStageDesktopBound = 2,
    kStageForegroundChecked = 3,
    kStageComInit = 4,
    kStageUiaCreated = 5,
    kStageFocusQueried = 6,
    kStagePropsRead = 7,
};

struct TargetCheck {
    volatile LONG refs;
    volatile LONG done;
    volatile LONG stage;
    DWORD createdTick;   // 发起校验的时刻，用来量冷启动成本
    int verdict;
    int elapsedMs;   // GetFocusedElement 本身的耗时
    int setupMs;     // 线程进入到发起 UIA 查询的耗时（冷启动成本主要在这里）
    DWORD focusPid;
    bool isPassword;
    wchar_t desktop[64];
    wchar_t detail[256];
};

// 引用计数：调用方超时返回到工作线程收尾之间不能释放，否则工作线程写已释放内存。
void targetCheckRelease(TargetCheck* c)
{
    if (InterlockedDecrement(&c->refs) == 0)
        delete c;
}

DWORD WINAPI targetCheckThreadProc(LPVOID param)
{
    TargetCheck* c = static_cast<TargetCheck*>(param);
    c->verdict = kTargetUncertain;
    c->detail[0] = 0;
    InterlockedExchange(&c->stage, kStageStart);

    auto finish = [c](TargetVerdict v, const QString& detail) {
        c->verdict = static_cast<int>(v);
        const std::wstring w = detail.toStdWString();
        wcsncpy(c->detail, w.c_str(), sizeof(c->detail) / sizeof(wchar_t) - 1);
        InterlockedExchange(&c->done, 1);
        targetCheckRelease(c);
    };

    // 1) 绑到"当前输入桌面"。新线程默认继承进程启动桌面（Winlogon），但屏保型锁屏的
    //    输入桌面是 Screen-saver；绑过去才能如实检查用户眼前那块界面。
    HDESK input = OpenInputDesktop(0, FALSE, GENERIC_ALL);
    if (!input) {
        finish(kTargetUncertain, QStringLiteral("拿不到输入桌面, err=%1").arg(GetLastError()));
        return 0;
    }
    InterlockedExchange(&c->stage, kStageDesktopOpened);
    wchar_t nm[64] = { 0 };
    DWORD need = 0;
    if (GetUserObjectInformationW(input, UOI_NAME, nm, sizeof(nm) - sizeof(wchar_t), &need))
        wcsncpy(c->desktop, nm, sizeof(c->desktop) / sizeof(wchar_t) - 1);
    const bool bound = SetThreadDesktop(input) != FALSE;
    const DWORD bindErr = GetLastError();
    CloseDesktop(input);
    if (!bound) {
        // 绑不上就无从判断"眼前的界面"，按不确定处理（宁可不动，也不要打错窗口）。
        finish(kTargetUncertain,
               QStringLiteral("SetThreadDesktop failed, err=%1").arg(bindErr));
        return 0;
    }
    InterlockedExchange(&c->stage, kStageDesktopBound);

    // 2) 低档（只做否定）：前台窗口必须属于 LogonUI.exe。
    HWND fg = GetForegroundWindow();
    DWORD fgPid = 0;
    if (fg)
        GetWindowThreadProcessId(fg, &fgPid);
    const QString fgExe = processExeName(fgPid);
    if (fgExe.isEmpty()) {
        finish(kTargetUncertain, QStringLiteral("取不到前台窗口进程, pid=%1").arg(fgPid));
        return 0;
    }
    if (fgExe.compare(QLatin1String("LogonUI.exe"), Qt::CaseInsensitive) != 0) {
        finish(kTargetNotLogonUi,
               QStringLiteral("前台进程是 %1，不是 LogonUI").arg(fgExe));
        return 0;
    }
    InterlockedExchange(&c->stage, kStageForegroundChecked);

    // 3) 高档（放行判据）：UIA FocusedElement 的 IsPassword + 进程归属。
    const HRESULT hrInit = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if (FAILED(hrInit)) {
        finish(kTargetUncertain,
               QStringLiteral("CoInitializeEx 失败, hr=0x%1")
                   .arg(static_cast<quint32>(hrInit), 8, 16, QLatin1Char('0')));
        return 0;
    }
    InterlockedExchange(&c->stage, kStageComInit);
    void* uia = nullptr;
    // MinGW 的 CoCreateInstance 声明是 const IID&（Windows SDK 的 REFCLSID 同义），
    // 所以这里传引用而不是取址。
    const HRESULT hrCreate = CoCreateInstance(kClsidCUIAutomation, nullptr,
                                              CLSCTX_INPROC_SERVER,
                                              kIidIUIAutomation, &uia);
    if (FAILED(hrCreate) || !uia) {
        CoUninitialize();
        finish(kTargetUncertain,
               QStringLiteral("创建 UIAutomation 失败, hr=0x%1")
                   .arg(static_cast<quint32>(hrCreate), 8, 16, QLatin1Char('0')));
        return 0;
    }
    InterlockedExchange(&c->stage, kStageUiaCreated);
    void* elem = nullptr;
    c->setupMs = static_cast<int>(GetTickCount() - c->createdTick);
    const DWORD t0 = GetTickCount();
    const HRESULT hrFocus = uiaAutomationVtbl(uia)->getFocusedElement(uia, &elem);
    c->elapsedMs = static_cast<int>(GetTickCount() - t0);
    if (FAILED(hrFocus) || !elem) {
        uiaRelease(uia);
        CoUninitialize();
        finish(kTargetUncertain,
               QStringLiteral("UIA 未返回焦点元素, hr=0x%1")
                   .arg(static_cast<quint32>(hrFocus), 8, 16, QLatin1Char('0')));
        return 0;
    }
    InterlockedExchange(&c->stage, kStageFocusQueried);

    int pid = 0;
    bool isPwd = false;
    const bool pidOk = readUiaInt(elem, kUiaProcessIdProperty, &pid);
    const bool pwdOk = readUiaBool(elem, kUiaIsPasswordProperty, &isPwd);
    c->focusPid = pidOk ? static_cast<DWORD>(pid) : 0;
    c->isPassword = pwdOk && isPwd;
    const QString focusExe = pidOk ? processExeName(static_cast<DWORD>(pid)) : QString();
    uiaRelease(elem);
    uiaRelease(uia);
    CoUninitialize();
    InterlockedExchange(&c->stage, kStagePropsRead);

    if (!pidOk || !pwdOk) {
        finish(kTargetUncertain, QStringLiteral("读不到焦点元素属性（pid/IsPassword）"));
        return 0;
    }
    if (!isPwd) {
        finish(kTargetNotLogonUi,
               QStringLiteral("焦点不是密码框（pid=%1, 进程=%2）")
                   .arg(pid).arg(focusExe.isEmpty() ? QStringLiteral("?") : focusExe));
        return 0;
    }
    if (focusExe.compare(QLatin1String("LogonUI.exe"), Qt::CaseInsensitive) != 0) {
        finish(kTargetNotLogonUi,
               QStringLiteral("焦点密码框不属于 LogonUI（进程=%1）")
                   .arg(focusExe.isEmpty() ? QStringLiteral("?") : focusExe));
        return 0;
    }
    finish(kTargetPasswordBox,
           QStringLiteral("焦点在 LogonUI 密码框（pid=%1, 准备 %2ms, 查询 %3ms, 桌面=%4）")
               .arg(pid).arg(c->setupMs).arg(c->elapsedMs)
               .arg(QString::fromWCharArray(c->desktop)));
    return 0;
}

// 同步等待上限：UIA 正常 3~45ms（UIAutomationCore 首次加载会到几百 ms，启动时已
// 用一次预热摊掉），超过 500ms 一律视为"不确定"（不注入）。
const int kTargetCheckTimeoutMs = 500;

TargetVerdict validatePasswordTarget(QString* reasonCode, QString* detail)
{
    TargetCheck* c = new TargetCheck();
    c->refs = 2;   // 调用方 + 工作线程
    c->done = 0;
    c->stage = kStageStart;
    c->createdTick = GetTickCount();
    c->verdict = kTargetUncertain;
    c->elapsedMs = 0;
    c->setupMs = 0;
    c->focusPid = 0;
    c->isPassword = false;
    c->desktop[0] = 0;
    c->detail[0] = 0;

    HANDLE th = CreateThread(nullptr, 0, targetCheckThreadProc, c, 0, nullptr);
    if (!th) {
        const DWORD err = GetLastError();
        c->refs = 1;
        targetCheckRelease(c);
        *reasonCode = QStringLiteral("uncertain");
        *detail = QStringLiteral("无法启动校验线程, err=%1").arg(err);
        return kTargetUncertain;
    }
    const DWORD w = WaitForSingleObject(th, static_cast<DWORD>(kTargetCheckTimeoutMs));
    CloseHandle(th);

    TargetVerdict v = kTargetUncertain;
    if (w == WAIT_OBJECT_0 && c->done == 1) {
        v = static_cast<TargetVerdict>(c->verdict);
        const QString d = QString::fromWCharArray(c->detail);
        *detail = d;
    } else {
        const int stage = static_cast<int>(InterlockedCompareExchange(&c->stage, 0, 0));
        QString stageNote;
        switch (stage) {
        case kStageUiaCreated:
        case kStageFocusQueried:
        case kStagePropsRead:
            stageNote = QStringLiteral("UIA 已创建但查询未返回");
            break;
        case kStageComInit:
            stageNote = QStringLiteral("UIAutomation 初始化未完成");
            break;
        case kStageForegroundChecked:
            stageNote = QStringLiteral("前台窗口检查后卡住");
            break;
        case kStageDesktopOpened:
        case kStageDesktopBound:
            stageNote = QStringLiteral("输入桌面绑定/前台检查未完成");
            break;
        default:
            stageNote = QStringLiteral("校验线程刚启动");
            break;
        }
        *detail = QStringLiteral("UI 校验超时 %1ms（%2，按不确定处理）")
                      .arg(kTargetCheckTimeoutMs).arg(stageNote);
        qWarning() << "SecureInput: target check timed out, stage =" << stage
                   << "pid =" << c->focusPid
                   << "desktop =" << QString::fromWCharArray(c->desktop);
    }
    *reasonCode = (v == kTargetNotLogonUi) ? QStringLiteral("not_logon_ui")
                 : (v == kTargetUncertain) ? QStringLiteral("uncertain")
                                           : QStringLiteral("ok");
    targetCheckRelease(c);
    return v;
}

// [C] 上报服务端：本轮解锁进行到了哪一步（取代原来的 unlock_blocked 单一上报）。
//   stage = verified   目标校验通过、即将键入；服务端可据此知道"这一轮还在推进"
//   stage = typed      密码真的进了输入队列（带 typed/dropped/cleared 计数）——
//                      服务端见到 typed 就**不再补发第二轮**、也不再用 15s 判失败
//   stage = blocked    明确拒绝（焦点不在密码框，典型是错误密码后的模态弹窗）
//   stage = uncertain  校验超时/异常，出于安全不注入（可被服务端补一轮重试）
// 指向主线程里的 UnlockReportSink（见文件开头的说明）；解锁线程只 post，不碰 socket。
UnlockReportSink* g_reportSink = nullptr;

void reportUnlockProgress(const QString& stage, const QString& reasonCode,
                          const QString& message, int attempt, int seq,
                          const QJsonObject& extra = QJsonObject())
{
    QJsonObject o {
        { QStringLiteral("type"), QStringLiteral("unlock_progress") },
        { QStringLiteral("stage"), stage },
        { QStringLiteral("reason"), reasonCode },
        { QStringLiteral("message"), message },
        { QStringLiteral("attempt"), attempt },
        { QStringLiteral("seq"), seq },
        { QStringLiteral("desktop"), currentInputDesktopName() },
    };
    for (const QString& k : extra.keys())
        o.insert(k, extra.value(k));
    const QString payload = QString::fromUtf8(
        QJsonDocument(o).toJson(QJsonDocument::Compact));

    if (stage == QLatin1String("blocked") || stage == QLatin1String("uncertain"))
        qWarning() << "SecureInput: unlock progress" << stage << "-" << reasonCode << message
                   << "attempt =" << attempt << "seq =" << seq;
    else
        qInfo() << "SecureInput: unlock progress" << stage << "-" << reasonCode
                << "attempt =" << attempt << "seq =" << seq;

    if (!g_reportSink) {
        // 没登记中转对象（理论上不会发生）：只能记日志，别在裸线程里动 socket。
        qWarning() << "SecureInput: no report sink, unlock_progress not sent to service";
        return;
    }
    g_reportSink->post(payload);
}

// 登录模式上报：stage = verified / typed / blocked / uncertain。
// blocked 的 reason 用固定枚举（user_not_found / not_logon_ui / no_credentials），
// 服务端据此外发 login_result；typed 之后由服务端查 WTS 会话用户名做最终确认。
void reportLoginProgress(const QString& stage, const QString& reasonCode,
                         const QString& message, int seq,
                         const QJsonObject& extra = QJsonObject())
{
    QJsonObject o {
        { QStringLiteral("type"), QStringLiteral("login_progress") },
        { QStringLiteral("stage"), stage },
        { QStringLiteral("reason"), reasonCode },
        { QStringLiteral("message"), message },
        { QStringLiteral("seq"), seq },
        { QStringLiteral("desktop"), currentInputDesktopName() },
    };
    for (const QString& k : extra.keys())
        o.insert(k, extra.value(k));
    const QString payload = QString::fromUtf8(
        QJsonDocument(o).toJson(QJsonDocument::Compact));

    if (stage == QLatin1String("blocked") || stage == QLatin1String("uncertain"))
        qWarning() << "SecureInput: login progress" << stage << "-" << reasonCode << message
                   << "seq =" << seq;
    else
        qInfo() << "SecureInput: login progress" << stage << "-" << reasonCode << "seq =" << seq;

    if (!g_reportSink) {
        qWarning() << "SecureInput: no report sink, login_progress not sent to service";
        return;
    }
    g_reportSink->post(payload);
}

// ---------------------------------------------------------------------------
// UIA 冷启动预热
//
// CoCreateInstance(CLSID_CUIAutomation) 首次调用要加载 UIAutomationCore 并建跨进程
// provider：实测在本机首次查询要 400~500ms（第一轮校验就是这样超时被判"不确定"的），
// 而预热之后同一个查询只要几毫秒。进程是锁屏时才被拉起的，于是启动时就在登录桌面上跑一遍和
// 正式校验完全相同的代码，把冷启动成本挪到"还没人按解锁"的时候。
// 这里只记日志、绝不上报：服务端此时可能刚记下一次解锁请求，预热结果不能冒充它。
// ---------------------------------------------------------------------------
DWORD WINAPI uiaWarmupThreadProc(LPVOID)
{
    QString reason;
    QString detail;
    const TargetVerdict v = validatePasswordTarget(&reason, &detail);
    qInfo() << "SecureInput: UIA warmup verdict =" << reason << detail;
    (void)v;
    return 0;
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
    // lpDesktop=<当前输入桌面> 拉起的，输入队列一开始就绑在这张桌面上，SendInput
    // 可直接进入密码框。所以不必等对话框、更不能发 SAS（SAS 会弹出「切换用户」
    // 把密码框切走）。屏保型锁屏按下面的"唤醒 → 等桌面切换 → B 校验 → 注入"处理。
    if (isVistaPlusOs()) {
        if (isScreensaverDesktop(deskName)) {
            // 屏保型锁屏：登录界面还没接管（输入桌面名是 Screen-saver）。先投一次
            // **不含任何字符**的唤醒输入，等 Winlogon 把输入桌面换成登录桌面后，
            // 再做 B 校验 + 注入。在这张桌面上绝不键入密码。
            if (ctx->attempt > 0) {
                // 上一轮已经唤醒过，仍停在屏保桌面 → 宁可不解锁，也不"随便往任何
                // 桌面打密码"。明确回 blocked，前端可重试。
                reportUnlockProgress(
                    QStringLiteral("blocked"), QStringLiteral("screensaver_stuck"),
                    QStringLiteral("远端仍停在屏保安全桌面，未注入密码"
                                   "（唤醒后输入桌面没有切回登录界面）"),
                    ctx->attempt, ctx->seq);
                detail = QStringLiteral("input desktop still Screen-saver after wake");
                return 0;
            }

            wakeScreenSaverDesktop();
            QString afterWake = waitForLogonDesktop(2500);
            if (afterWake.isEmpty() || isScreensaverDesktop(afterWake)) {
                // Win7 实测：单纯 SendInput 不足以让安全屏保退出（屏保窗口就在
                // Screen-saver 桌面上，Winlogon 不会因为合成输入就收屏保）。
                // 与 XP 分支同一手段：结束 .scr，Winlogon 收到屏保进程退出后把输入
                // 桌面切回登录桌面。结束屏保不绕过密码 —— 下一步仍要过 B 校验。
                qInfo() << "SecureInput: wake input did not leave Screen-saver, killing .scr";
                const int killed = killScreenSaverProcesses();
                qInfo() << "SecureInput: terminated .scr =" << killed;
                afterWake = waitForLogonDesktop(4500);
            }

            if (afterWake.compare(QLatin1String("default"), Qt::CaseInsensitive) == 0) {
                qInfo() << "SecureInput: input desktop is Default after wake - already unlocked";
                return 0;
            }
            if (afterWake.isEmpty() || isScreensaverDesktop(afterWake)) {
                reportUnlockProgress(
                    QStringLiteral("blocked"), QStringLiteral("screensaver_stuck"),
                    QStringLiteral("远端仍停在屏保安全桌面，未注入密码"
                                   "（唤醒后输入桌面没有切回登录界面）"),
                    ctx->attempt, ctx->seq);
                detail = QStringLiteral("input desktop is %1 after wake")
                             .arg(afterWake.isEmpty() ? QStringLiteral("unknown") : afterWake);
                return 0;
            }

            // 桌面已切到登录桌面（通常 Winlogon）。本线程的桌面绑定停在
            // Screen-saver 且无法改绑（输入队列已建立），必须交回新线程重新
            // attach（afterSas 语义：等到非屏保桌面）后走正常的 B 校验 + 注入。
            if (ctx->attempt + 1 < 3) {
                Sleep(200);
                auto* next = new UnlockCtx { pw, ctx->attempt + 1, ctx->seq };
                HANDLE th = CreateThread(nullptr, 0, unlockThreadProc, next, 0, nullptr);
                if (th) {
                    CloseHandle(th);
                    handOff = true;
                    return 0;
                }
                delete next;
                qWarning() << "SecureInput: retry thread failed, err="
                           << static_cast<int>(GetLastError());
            }
            detail = QStringLiteral("input desktop switched to %1 but retry hand-off failed")
                         .arg(afterWake);
            return 0;
        }
        // 屏保唤醒后的过渡：桌面已经叫 Winlogon，但 LogonUI 的前台窗口可能还没建
        // 出来。只在这条路径（attempt>0 = 由屏保唤醒接力）上等窗口出现，避免 B 校验
        // 在过渡空窗里拿到 pid=0 直接判 uncertain；等不到仍按 B 原判据拒绝注入。
        if (ctx->attempt > 0)
            waitForLogonUiForeground(4000);
        Sleep(300);   // 登录界面刚起来时给一点稳定时间

        // [B] 注入前目标校验：确认焦点真的在 LogonUI 的密码框上。
        // 上一轮密码被拒后弹着模态「用户名或密码不正确」、屏保还盖着、甚至已经解锁，
        // 这里都会拦下来 —— 绝不把明文密码敲进弹窗或普通窗口。
        // 校验在工作线程里跑，最多等 500ms；超时/异常按"不确定"一律不注入。
        //
        // 屏保唤醒接力（attempt>0）是有界重试的**唯一**场景：Win7 实测 09:31/09:33
        // 两次屏保解锁都在 attempt=1 上出现 stage=UiaCreated 的 500ms 超时（前台
        // LogonUI 已经确认，只是新线程里的首次 getFocusedElement 慢），随后服务端
        // 整轮重试就恢复成 0ms。因此这里在 ≤3s 内最多重试 3 次（每次仍 500ms），
        // not_logon_ui 是硬拒绝不重试，判据（IsPassword + LogonUI.exe）一个字不放宽。
        // 普通锁屏（attempt=0）保持原来的单发 500ms，行为不变。
        QString reasonCode;
        QString why;
        bool verified = false;
        const int targetTries = (ctx->attempt > 0) ? 3 : 1;
        for (int i = 0; i < targetTries; ++i) {
            if (validatePasswordTarget(&reasonCode, &why) == kTargetPasswordBox) {
                if (i > 0)
                    qInfo() << "SecureInput: target check succeeded on retry" << (i + 1)
                            << "of" << targetTries;
                verified = true;
                break;
            }
            qInfo() << "SecureInput: target check try" << (i + 1) << "of" << targetTries
                    << "verdict =" << reasonCode << why;
            if (reasonCode == QLatin1String("not_logon_ui"))
                break;
            if (i + 1 < targetTries)
                Sleep(300);
        }
        if (!verified) {
            const QString userMessage =
                (reasonCode == QLatin1String("not_logon_ui"))
                    ? QStringLiteral("未注入密码：当前焦点不在登录密码框（%1），"
                                     "为避免把密码打进弹窗或普通窗口，已取消本次注入。").arg(why)
                    : QStringLiteral("未能确认登录密码框（%1），为安全起见未注入密码，"
                                     "请稍后重试。").arg(why);
            reportUnlockProgress(reasonCode == QLatin1String("not_logon_ui")
                                     ? QStringLiteral("blocked") : QStringLiteral("uncertain"),
                                 reasonCode, userMessage, ctx->attempt, ctx->seq);
            detail = QStringLiteral("injection blocked: %1").arg(why);
            return 0;
        }

        qInfo() << "SecureInput: Vista+ on desktop" << deskName
                << "- target verified (" << why << "), typing password via SendInput";
        reportUnlockProgress(QStringLiteral("verified"), QStringLiteral("ok"),
                             QStringLiteral("登录密码框校验通过，开始注入"),
                             ctx->attempt, ctx->seq);

        // [C] 先清空密码框，再键入。LogonUI 上 Ctrl+A 不可靠（实测只删 1 个字符），
        // 只能用连续 Backspace；框本来就空时这些按键是无害的。这样「框里有残留字符」
        // 也能一次解开，不必人工去点。
        const int cleared = clearPasswordField();
        qInfo() << "SecureInput: cleared password box with" << cleared << "backspaces";
        const PasswordInjectionStats st = sendPasswordWithSendInput(pw);

        reportUnlockProgress(st.typed > 0 ? QStringLiteral("typed") : QStringLiteral("uncertain"),
                             st.typed > 0 ? QStringLiteral("ok") : QStringLiteral("nothing_typed"),
                             st.typed > 0
                                 ? QStringLiteral("密码已键入（%1 字符）").arg(st.typed)
                                 : QStringLiteral("密码字符一个都没能送进输入队列"),
                             ctx->attempt, ctx->seq,
                             QJsonObject {
                                 { QStringLiteral("typed"), st.typed },
                                 { QStringLiteral("dropped"), st.dropped },
                                 { QStringLiteral("cleared"), cleared },
                             });
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
            auto* next = new UnlockCtx { pw, ctx->attempt + 1, ctx->seq };
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
            res = fillLogonDialog(QString(), pw, &detail);
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
        auto* next = new UnlockCtx { pw, ctx->attempt + 1, ctx->seq };
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

// ---------------------------------------------------------------------------
// 登录模式（注销后 / 开机停在登录界面，且**没有任何用户会话**）
//
// 与 unlockThreadProc 的区别：
//   * 不依赖 helper / 用户令牌（进程本来就是服务用 SYSTEM 令牌拉起的）；
//   * Vista+ 需要先在 Winlogon 桌面找到 Name==用户名 的磁贴并选中，
//     随后仍走 B 判据（前台 LogonUI + IsPassword）→ C 清空 → A 注入 → Enter；
//   * XP 走传统 #32770，写入用户名 + 密码后 IDOK；
//     欢迎屏幕下先 SAS 切传统登录框，避免在不认识用户名的情况下点错磁贴。
// ---------------------------------------------------------------------------
struct LoginCtx {
    QString user;
    QString pw;
    int seq;
};

volatile LONG g_loginBusy = 0;
int g_loginXpAttempt = 0;   // 仅 XP：本轮登录已经 SAS 过几次（每次登录请求重置）

DWORD WINAPI loginThreadProc(LPVOID param)
{
    QScopedPointer<LoginCtx> ctx(static_cast<LoginCtx*>(param));
    struct BusyGuard {
        bool release = true;
        ~BusyGuard() { if (release) InterlockedExchange(&g_loginBusy, 0); }
    } busyGuard;

    const QString user = ctx->user;
    const QString pw = ctx->pw;
    const int seq = ctx->seq;
    QString detail;

    const QString deskName = attachInputDesktop(false);
    qInfo() << "SecureInput: login start, user =" << user << "desktop =" << deskName;

    if (deskName.compare(QLatin1String("default"), Qt::CaseInsensitive) == 0) {
        reportLoginProgress(QStringLiteral("blocked"), QStringLiteral("not_logon_ui"),
                            QStringLiteral("输入桌面已是用户桌面，当前不是登录界面"), seq);
        return 0;
    }

    if (isScreensaverDesktop(deskName)) {
        const int killed = killScreenSaverProcesses();
        const QString afterWake = waitForLogonDesktop(4500);
        qInfo() << "SecureInput: login desktop is Screen-saver - killed .scr =" << killed
                << "afterWake =" << afterWake;
        if (!afterWake.isEmpty() && !isScreensaverDesktop(afterWake)) {
            // 桌面已切回登录桌面；桌面绑定换不掉，必须换新线程重新 attach。
            auto* next = new LoginCtx { user, pw, seq };
            HANDLE th = CreateThread(nullptr, 0, loginThreadProc, next, 0, nullptr);
            if (th) {
                CloseHandle(th);
                busyGuard.release = false;   // 所有权移交给新线程
                return 0;
            }
            delete next;
        }
        reportLoginProgress(QStringLiteral("blocked"), QStringLiteral("not_logon_ui"),
                            QStringLiteral("远端停在屏保安全桌面，无法进入登录界面"), seq);
        return 0;
    }

    if (isVistaPlusOs()) {
        // 刚注销/刚开机时 LogonUI 可能还在起界面，等前台窗口出现再找磁贴。
        waitForLogonUiForeground(4000);
        Sleep(200);

        // 上一轮密码错误留下的错误提示 / 密码页：新一轮请求先把界面幂等收敛回磁贴
        // 列表（Invoke aid=Ok → Invoke aid=OtherTiles）。收不掉就回 stuck_dialog，
        // 绝不在错误弹窗/密码页上盲打。wrong_password 之后绝不自动重放密码。
        QString convergeDetail;
        const ConvergeResult cr = convergeLogonUiState(&convergeDetail);
        if (cr == kConvergeFailed) {
            reportLoginProgress(QStringLiteral("blocked"), QStringLiteral("stuck_dialog"),
                                QStringLiteral("登录界面残留错误提示，且无法自动收敛回磁贴列表"
                                               "（未键入密码）：%1")
                                    .arg(convergeDetail),
                                seq);
            return 0;
        }
        if (cr == kConvergeDone)
            qInfo() << "SecureInput: login UI converged -" << convergeDetail;

        TileHit hit;
        TileFindResult fr = kTileNotFound;
        // 刚收敛完（关弹窗 + 切换用户）时磁贴列表还在重绘，给它最多 ~2.4s；
        // 没有收敛动作时保持原来的"一次判定"，不拖慢 user_not_found。
        const int tileTries = (cr == kConvergeDone) ? 12 : 1;
        for (int i = 0; i < tileTries; ++i) {
            fr = findUserTile(user, &hit, &detail);
            if (fr != kTileNotFound)
                break;
            Sleep(200);
        }
        if (fr == kTileNotFound) {
            reportLoginProgress(QStringLiteral("blocked"), QStringLiteral("user_not_found"),
                                QStringLiteral("登录界面上找不到用户 %1 的磁贴（未键入密码）：%2")
                                    .arg(user, detail),
                                seq);
            return 0;
        }
        if (fr == kTileUncertain) {
            reportLoginProgress(QStringLiteral("uncertain"), QStringLiteral("uncertain"),
                                QStringLiteral("无法读取登录界面磁贴（未键入密码）：%1").arg(detail),
                                seq);
            return 0;
        }

        QString how;
        const bool activated = activateTile(hit, &how);
        if (hit.element)
            uiaRelease(hit.element);
        if (!activated) {
            reportLoginProgress(QStringLiteral("uncertain"), QStringLiteral("uncertain"),
                                QStringLiteral("找到磁贴但无法选中（未键入密码）：%1").arg(how),
                                seq);
            return 0;
        }
        qInfo() << "SecureInput: login tile selected via" << how << "(" << detail << ")";

        // 选中磁贴后密码框才出现；沿用 B 判据严格确认，绝不盲打。
        bool ready = false;
        QString reasonCode;
        QString why;
        for (int i = 0; i < 15; ++i) {
            Sleep(200);
            if (validatePasswordTarget(&reasonCode, &why) == kTargetPasswordBox) {
                ready = true;
                break;
            }
        }
        if (!ready) {
            const bool knownUi = (reasonCode == QLatin1String("not_logon_ui"));
            reportLoginProgress(knownUi ? QStringLiteral("blocked") : QStringLiteral("uncertain"),
                                reasonCode,
                                QStringLiteral("选中磁贴后未确认到登录密码框（%1），未键入密码").arg(why),
                                seq);
            return 0;
        }

        reportLoginProgress(QStringLiteral("verified"), QStringLiteral("ok"),
                            QStringLiteral("登录密码框校验通过，开始注入密码"), seq);
        const int cleared = clearPasswordField();
        const PasswordInjectionStats st = sendPasswordWithSendInput(pw);
        reportLoginProgress(st.typed > 0 ? QStringLiteral("typed") : QStringLiteral("uncertain"),
                            st.typed > 0 ? QStringLiteral("ok") : QStringLiteral("nothing_typed"),
                            st.typed > 0
                                ? QStringLiteral("用户名 %1 的密码已键入（%2 字符）").arg(user).arg(st.typed)
                                : QStringLiteral("密码字符一个都没能送进输入队列"),
                            seq,
                            QJsonObject {
                                { QStringLiteral("typed"), st.typed },
                                { QStringLiteral("dropped"), st.dropped },
                                { QStringLiteral("cleared"), cleared },
                                { QStringLiteral("username"), user },
                            });
        return 0;
    }

    // ---------------- XP ----------------
    FillResult res = kNoDialog;
    for (int i = 0; i < 25; ++i) {
        res = fillLogonDialog(user, pw, &detail);
        if (res != kNoDialog)
            break;
        if (hasWelcomeScreen())
            break;
        Sleep(200);
    }

    if (res == kSubmitted) {
        reportLoginProgress(QStringLiteral("typed"), QStringLiteral("ok"),
                            QStringLiteral("用户名 %1 / 密码已写入传统登录框并提交：%2")
                                .arg(user, detail),
                            seq);
        return 0;
    }
    if (res == kRejected) {
        reportLoginProgress(QStringLiteral("uncertain"), QStringLiteral("uncertain"),
                            QStringLiteral("登录框拒绝写入（未确认登录）：%1").arg(detail), seq);
        return 0;
    }

    // 没有 #32770：XP 欢迎屏幕无法按用户名定位磁贴。先 SAS 切到传统登录框
    // （那里能写用户名 + 密码，且与解锁链路完全一致），再让新线程重试。
    if (hasWelcomeScreen()) {
        if (g_loginXpAttempt < 2) {
            ++g_loginXpAttempt;
            qInfo() << "SecureInput: XP login welcome screen, sending SAS (attempt"
                    << g_loginXpAttempt << ")";
            sendSasKeys();
            Sleep(300);
            auto* next = new LoginCtx { user, pw, seq };
            HANDLE th = CreateThread(nullptr, 0, loginThreadProc, next, 0, nullptr);
            if (th) {
                CloseHandle(th);
                busyGuard.release = false;
                return 0;
            }
            delete next;
            reportLoginProgress(QStringLiteral("uncertain"), QStringLiteral("uncertain"),
                                QStringLiteral("无法重试登录线程"), seq);
            return 0;
        }
        // 仍停在欢迎屏幕（该机器不接受 SAS 切传统框）：退回老路径点磁贴键入。
        // 仅适用于欢迎屏幕只有一个账户的机器（本机验收账户 justrock）。
        QString wdetail;
        if (fillWelcomeScreen(pw, &wdetail)) {
            reportLoginProgress(QStringLiteral("typed"), QStringLiteral("ok"),
                                QStringLiteral("欢迎屏幕已键入密码：%1").arg(wdetail), seq);
        } else {
            reportLoginProgress(QStringLiteral("uncertain"), QStringLiteral("uncertain"),
                                QStringLiteral("欢迎屏幕键入失败：%1").arg(wdetail), seq);
        }
        return 0;
    }

    reportLoginProgress(QStringLiteral("blocked"), QStringLiteral("not_logon_ui"),
                        QStringLiteral("当前不是可操作的登录界面（%1）").arg(detail), seq);
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
    // [B] 解锁线程拒绝注入密码时要上报原因，而 QWebSocket 只能在主线程用。
    // 解锁线程是 CreateThread 出来的裸线程（没有 Qt 事件循环，连 QTimer 都不合法），
    // 所以登记一个主线程里的中转对象，由它做队列化投递（见 UnlockReportSink）。
    UnlockReportSink reportSink(&ws);
    g_reportSink = &reportSink;
    // [B] UIA 冷启动预热：见 uiaWarmupThreadProc 注释。失败只记日志，不影响判断。
    // XP 没有 UI Automation，整块 B（预热 + 校验 + 上报）都不碰，解锁走下面的老路径。
    if (isVistaPlusOs()) {
        if (HANDLE warm = CreateThread(nullptr, 0, uiaWarmupThreadProc, nullptr, 0, nullptr))
            CloseHandle(warm);
    }
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
            qInfo() << "SecureInput: unlock requested (seq =" << obj["seq"].toInt() << ")";
            // 只允许一条解锁序列：服务端会补发 unlock，重复序列的 SAS 会互相打断
            if (InterlockedCompareExchange(&g_unlockBusy, 1, 0) != 0) {
                qInfo() << "SecureInput: unlock already in progress, ignoring duplicate request";
                return;
            }
            HANDLE th = CreateThread(nullptr, 0, unlockThreadProc,
                                    new UnlockCtx { pw, 0, obj["seq"].toInt() }, 0, nullptr);
            if (th) {
                CloseHandle(th);
            } else {
                InterlockedExchange(&g_unlockBusy, 0);
                qWarning() << "SecureInput: failed to spawn unlock thread, err="
                           << static_cast<int>(GetLastError());
            }
            return;
        }

        if (type == QStringLiteral("login")) {
            // 登录模式：注销后/开机的登录界面，没有用户会话也没有 helper。
            // 用户名+密码由服务端下发（密码复用 unlockPassword 加密通道）。
            const QString user = obj["username"].toString();
            const QString pw = obj["password"].toString();
            if (user.isEmpty() || pw.isEmpty()) {
                qWarning() << "SecureInput: login requested without username/password";
                return;
            }
            if (InterlockedCompareExchange(&g_loginBusy, 1, 0) != 0) {
                qInfo() << "SecureInput: login already in progress, ignoring duplicate request";
                return;
            }
            g_loginXpAttempt = 0;
            qInfo() << "SecureInput: login requested, user =" << user
                    << "seq =" << obj["seq"].toInt();
            HANDLE th = CreateThread(nullptr, 0, loginThreadProc,
                                    new LoginCtx { user, pw, obj["seq"].toInt() }, 0, nullptr);
            if (th) {
                CloseHandle(th);
            } else {
                InterlockedExchange(&g_loginBusy, 0);
                qWarning() << "SecureInput: failed to spawn login thread, err="
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
    const int rc = app.exec();
    g_reportSink = nullptr;   // [B] 事件循环结束后不再有任何上报
    return rc;
#else
    (void)argc; (void)argv; (void)wsPort;
    qCritical("Secure input process is not supported on this platform");
    return 1;
#endif
}

// Q_OBJECT 类定义在本 .cpp 内（UnlockReportSink），moc 产物由 CMake AUTOMOC 生成。
#include "secure_input_process.moc"
