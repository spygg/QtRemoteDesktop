#include "inputmanager.h"
#include <QCursor>
#include <QDebug>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QGuiApplication>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QScreen>
#include <QSaveFile>
#include <QThread>
#include <QTimer>

#include <X11/Xlib.h>
#include <X11/Xatom.h>
#include <X11/extensions/XTest.h>
#include <X11/keysym.h>

#include <linux/uinput.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <cstring>

#include <QDBusConnection>
#include <QDBusMessage>
#include <QDBusArgument>
#include <QDBusVariant>
#include <QDBusObjectPath>
#include <QDBusPendingCall>
#include <QDBusPendingCallWatcher>
#include <QDBusPendingReply>
#include <QVariantMap>

static const QString PORTAL_SERVICE = QStringLiteral("org.freedesktop.portal.Desktop");
static const QString PORTAL_PATH    = QStringLiteral("/org/freedesktop/portal/desktop");
static const QString PORTAL_IFACE   = QStringLiteral("org.freedesktop.portal.RemoteDesktop");
static const QString REQUEST_IFACE  = QStringLiteral("org.freedesktop.portal.Request");

// 说明：QtDBus 原生支持将 QVariantMap 直接序列化为 D-Bus a{sv} 参数，
// 因此门户方法的 options 参数一律直接传 QVariantMap，不要再包装成 QDBusArgument
// （包装后 QDBusMarshaller 会因类型未注册而报错 "type QVariant is not registered"）。

namespace {
    Display* xdisp(void* p) { return static_cast<Display*>(p); }

    // 查询根窗口下光标的绝对坐标。服务模式(QCoreApplication)下 QCursor::pos()
    // 不可用，必须走 XQueryPointer。失败时返回 (-1,-1) 表示“位置未知”。
    bool queryPointer(void* dpy, int* x, int* y)
    {
        if (!dpy) return false;
        Display* display = static_cast<Display*>(dpy);
        Window root = DefaultRootWindow(display);
        Window rootRet, childRet;
        int rx, ry, wx, wy;
        unsigned int mask;
        if (!XQueryPointer(display, root, &rootRet, &childRet, &rx, &ry, &wx, &wy, &mask))
            return false;
        if (x) *x = rx;
        if (y) *y = ry;
        return true;
    }

    Window findLockScreenWindowRecursive(Display* dpy, Window root, Window target)
    {
        // Check target window's state
        Atom wmState = XInternAtom(dpy, "_NET_WM_STATE", True);
        Atom wmStateFullscreen = XInternAtom(dpy, "_NET_WM_STATE_FULLSCREEN", True);
        Atom wmStateAbove = XInternAtom(dpy, "_NET_WM_STATE_ABOVE", True);
        Atom wmWindowType = XInternAtom(dpy, "_NET_WM_WINDOW_TYPE", True);
        Atom wmTypeDesktop = XInternAtom(dpy, "_NET_WM_WINDOW_TYPE_DESKTOP", True);
        Atom wmTypeDock = XInternAtom(dpy, "_NET_WM_WINDOW_TYPE_DOCK", True);

        if (target == root || target == None)
            return None;

        XWindowAttributes attr;
        if (!XGetWindowAttributes(dpy, target, &attr))
            return None;
        if (attr.map_state != IsViewable)
            return None;
        if (attr.c_class != InputOutput)
            return None;

        // Skip desktop and dock
        if (wmWindowType != None && wmTypeDesktop != None) {
            Atom actualType; int actualFormat;
            unsigned long nitems, bytesAfter;
            unsigned char* data = nullptr;
            if (XGetWindowProperty(dpy, target, wmWindowType, 0, 1, False,
                    XA_ATOM, &actualType, &actualFormat,
                    &nitems, &bytesAfter, &data) == Success && data) {
                Atom* atoms = reinterpret_cast<Atom*>(data);
                // nitems 可能为 0：XGetWindowProperty 成功但属性为空时不校验就访问
                // atoms[0] 属越界读（下方 wmState 分支已有 j<nitems 判界，这里漏了）
                bool skip = nitems > 0
                            && (atoms[0] == wmTypeDesktop || atoms[0] == wmTypeDock);
                XFree(data);
                if (skip) return None;
            }
        }

        // Check for fullscreen or above state
        if (wmState != None && wmStateFullscreen != None) {
            Atom actualType; int actualFormat;
            unsigned long nitems, bytesAfter;
            unsigned char* data = nullptr;
            if (XGetWindowProperty(dpy, target, wmState, 0, 32, False,
                    XA_ATOM, &actualType, &actualFormat,
                    &nitems, &bytesAfter, &data) == Success && data) {
                Atom* states = reinterpret_cast<Atom*>(data);
                bool fs = false, above = false;
                for (unsigned long j = 0; j < nitems; j++) {
                    if (states[j] == wmStateFullscreen) fs = true;
                    if (states[j] == wmStateAbove) above = true;
                }
                XFree(data);
                if (fs || above)
                    return target;
            }
        }

        // Recurse into children
        Window dummyRoot, parent;
        Window* children = nullptr;
        unsigned int nchildren = 0;
        if (!XQueryTree(dpy, target, &dummyRoot, &parent, &children, &nchildren))
            return None;

        Window found = None;
        for (unsigned int i = 0; i < nchildren && found == None; i++)
            found = findLockScreenWindowRecursive(dpy, root, children[i]);

        if (children) XFree(children);
        return found;
    }

    Window findLockScreenWindow(Display* dpy)
    {
        Window root = DefaultRootWindow(dpy);

        // Try direct children first (fast path)
        Window dummyRoot, parent;
        Window* children = nullptr;
        unsigned int nchildren = 0;
        if (XQueryTree(dpy, root, &dummyRoot, &parent, &children, &nchildren)) {
            for (unsigned int i = 0; i < nchildren; i++) {
                Window w = findLockScreenWindowRecursive(dpy, root, children[i]);
                if (w != None) {
                    if (children) XFree(children);
                    return w;
                }
            }
            if (children) XFree(children);
        }

        // Fallback: try _NET_CLIENT_LIST
        Atom netClientList = XInternAtom(dpy, "_NET_CLIENT_LIST", True);
        if (netClientList != None) {
            Atom actualType; int actualFormat;
            unsigned long nitems, bytesAfter;
            unsigned char* data = nullptr;
            if (XGetWindowProperty(dpy, root, netClientList, 0, 1024, False,
                    XA_WINDOW, &actualType, &actualFormat,
                    &nitems, &bytesAfter, &data) == Success && data) {
                Window* windows = reinterpret_cast<Window*>(data);
                for (unsigned long i = 0; i < nitems; i++) {
                    Window w = findLockScreenWindowRecursive(dpy, root, windows[i]);
                    if (w != None) {
                        XFree(data);
                        return w;
                    }
                }
                XFree(data);
            }
        }

        return None;
    }

    // 把 ASCII 码点映射为 X keysym。锁屏密码等逐字符注入场景前端只给 Unicode
    // 码点（code 为空、isChar=true），需要在此还原成 keysym 才能注入。
    // needShift 输出该字符在标准 US 键盘布局下是否必须配合 Shift 才能打出
    // （大写字母与 !@#$ 等符号位于键的上档）。
    unsigned long asciiToKeysym(int ch, bool* needShift)
    {
        *needShift = false;
        if (ch >= 'a' && ch <= 'z') return XK_a + (ch - 'a');
        if (ch >= 'A' && ch <= 'Z') { *needShift = true; return XK_a + (ch - 'A'); }
        if (ch >= '0' && ch <= '9') return XK_0 + (ch - '0');
        switch (ch) {
            case ' ':  return XK_space;
            case '!':  *needShift = true; return XK_1;
            case '"':  *needShift = true; return XK_apostrophe;
            case '#':  *needShift = true; return XK_3;
            case '$':  *needShift = true; return XK_4;
            case '%':  *needShift = true; return XK_5;
            case '&':  *needShift = true; return XK_7;
            case '\'': return XK_apostrophe;
            case '(':  *needShift = true; return XK_9;
            case ')':  *needShift = true; return XK_0;
            case '*':  *needShift = true; return XK_8;
            case '+':  *needShift = true; return XK_equal;
            case ',':  return XK_comma;
            case '-':  return XK_minus;
            case '.':  return XK_period;
            case '/':  return XK_slash;
            case ':':  *needShift = true; return XK_semicolon;
            case ';':  return XK_semicolon;
            case '<':  *needShift = true; return XK_comma;
            case '=':  return XK_equal;
            case '>':  *needShift = true; return XK_period;
            case '?':  *needShift = true; return XK_slash;
            case '@':  *needShift = true; return XK_2;
            case '[':  return XK_bracketleft;
            case '\\': return XK_backslash;
            case ']':  return XK_bracketright;
            case '^':  *needShift = true; return XK_6;
            case '_':  *needShift = true; return XK_minus;
            case '`':  return XK_grave;
            case '{':  *needShift = true; return XK_bracketleft;
            case '|':  *needShift = true; return XK_backslash;
            case '}':  *needShift = true; return XK_bracketright;
            case '~':  *needShift = true; return XK_grave;
        }
        return NoSymbol;
    }
}

// 桌面会话是否为 Wayland：WAYLAND_DISPLAY 显式设置，或 $XDG_RUNTIME_DIR 下存在
// 合成器 socket（wayland-*，排除 *.lock）。SSH/systemd 等脱离桌面会话启动的进程
// 常缺 WAYLAND_DISPLAY，但 socket 依然存在；纯 X11 桌面没有该 socket。
bool InputManager::desktopSessionIsWayland()
{
    // [compat] qEnvironmentVariableIsEmpty 是 Qt 5.10 API，Qt 5.9 用 qgetenv
    if (!qgetenv("WAYLAND_DISPLAY").isEmpty())
        return true;
    const QByteArray rt = qgetenv("XDG_RUNTIME_DIR");
    if (rt.isEmpty())
        return false;
    const QStringList socks = QDir(QString::fromUtf8(rt)).entryList(
        QStringList() << QStringLiteral("wayland-*"), QDir::System | QDir::Files);
    for (const QString& s : socks) {
        if (!s.endsWith(QLatin1String(".lock")))
            return true;
    }
    return false;
}

// 探测逐键 API 是否被服务器实现：Xlib 的默认错误处理器只打印不退出，但会把每个
// 失败刷成 "X Error of failed request"；这里临时接管，记录错误码后立刻还原。
// 非 0 = 请求失败（如某些极简 X server 未实现 KBKey|KBAutoRepeatMode）。
// 定义提前到卫生检查之前：启动恢复也要用同一个错误收集器。
namespace {
int g_repeatControlError = 0;
int captureRepeatControlError(Display*, XErrorEvent* ev)
{
    g_repeatControlError = ev ? static_cast<int>(ev->error_code) : -1;
    return 0;
}
} // namespace

// 键位卫生状态的落盘位置：$XDG_RUNTIME_DIR（登录会话）或 /run/remotedesk
// （root 系统服务，/run 是 tmpfs，随重启清理；不能放 systemd RuntimeDirectory ——
// 服务崩溃后 systemd 会在下次启动前删掉该目录，恰好把恢复所需的状态丢了）。
// 只写本进程自己抑制过的 keycode 与原值，文件很小（几十字节）。
static QString keyHygieneStatePath()
{
    QString dir;
    const QByteArray rt = qgetenv("XDG_RUNTIME_DIR");
    if (!rt.isEmpty() && QDir(QString::fromUtf8(rt)).exists())
        dir = QString::fromUtf8(rt) + QStringLiteral("/remotedesk");
    else
        dir = QStringLiteral("/run/remotedesk");
    if (!QDir().mkpath(dir) || !QFileInfo(dir).isWritable()) {
        dir = QStringLiteral("/tmp/remotedesk-%1").arg(::getuid());
        QDir().mkpath(dir);
    }
    return dir + QStringLiteral("/key_state.json");
}

// 惰性打开 X display（首次注入时调用）。
// 线程约束：所有 inject* 注入入口最终都经 RDPServer::onInputReceived 在主线程
// 串行执行（WS 的 inputReceived 为队列连接），因此此处对 xDisplay_ 的写无需
// 加锁。若未来把注入移到独立线程，必须先加同步保护。
bool InputManager::ensureXDisplay()
{
    // 服务端会动态切换 DISPLAY/XAUTHORITY（虚拟屏 :99 ↔ 物理会话 :0、
    // cookie 修复后换成另一个 auth 文件）。已经打开的 X 连接不会跟着换：
    // 采集重启到了新屏，注入却还留在旧屏/旧凭据上 —— 这就是"画面正常、
    // 鼠标键盘全不动"的另一种成因。每次注入前比对环境，变了先关旧连接。
    const QByteArray curDpy = qgetenv("DISPLAY");
    const QByteArray curAuth = qgetenv("XAUTHORITY");
    if (xDisplay_ && (curDpy != openedDisplay_ || curAuth != openedXauth_)) {
        qInfo() << "InputManager: DISPLAY/XAUTHORITY changed ("
                << openedDisplay_ << "->" << curDpy << "), reopening X display";
        // 键盘映射是**服务器全局**的，连接一关就再也还不回去：必须在下一次
        // XCloseDisplay 之前归还借用的 keycode。
        restoreUnicodeKeycode();
        // 逐键 auto-repeat 同样是服务器端持久状态：换连接前必须把这批 keycode
        // 恢复成默认（旧连接还活着，还能发请求），否则新连接上再也没机会还。
        restoreAutoRepeatKeycodes();
        XCloseDisplay(xdisp(xDisplay_));
        xDisplay_ = nullptr;
        autoRepeatDisplay_ = nullptr;
        repeatControlProbed_ = false;
        lastXOpenFailMs_ = 0;   // 新环境值立刻重试，不要等 1.5s 限流
        // 旧屏的空闲键不代表新屏也空闲，借用状态与"借不到"的结论一起作废。
        unicodeScanFailed_ = false;
    }
    if (xDisplay_)
        return true;
    // 失败限流：display 不可用时，mousemove 会以 60Hz 调到这里。无节流时每秒
    // 做 60 次 XOpenDisplay（每次都是一次 X 协议连接/鉴权握手，失败路径可能上百
    // 微秒到毫秒），主线程被持续占用。1.5s 重试一次足够覆盖 display 后启动的场景。
    const qint64 nowMs = QDateTime::currentMSecsSinceEpoch();
    if (lastXOpenFailMs_ != 0 && nowMs - lastXOpenFailMs_ < 1500)
        return false;
    Display* d = XOpenDisplay(nullptr);
    if (!d) {
        lastXOpenFailMs_ = nowMs;
        // X display 仍不可用：纯 Wayland 且无 Xwayland 时，惰性发起 portal（只一次）。
        if (waylandMode_ && !portalInitStarted_) {
            portalInitStarted_ = true;
            initWaylandPortal();
        }
        return false;
    }
    xDisplay_ = d;
    openedDisplay_ = curDpy;
    openedXauth_ = curAuth;
    qInfo() << "InputManager: X display opened, DISPLAY =" << curDpy
            << "XAUTHORITY =" << curAuth;
    // [HYGIENE] 新连接（服务启动/首次拿到 X）立刻核对上一次进程遗留的键位。
    // kill -9 会绕过所有收尾路径，逐键 auto-repeat 与按下的键都留在 X 服务器上。
    hygieneRestoreOnNewDisplay();
    // [自动重复] 这里**从不**调用 XAutoRepeatOff / XAutoRepeatOn。
    // 旧实现为了压住"某键 keyup 丢失后 X 自动重复灌出一串字母"（实测
    // "cccc测试"/"zzzzz中文"）关掉了整个 X 服务器的自动重复 —— 但那是**全局**
    // 设置：本机物理键盘会被一起改成不重复，且没有恢复路径（进程退出、断开连接
    // 都不还原）。现在卡键的根因已在别处消除，而且本进程也不再依赖 X 的自动重复：
    //   · 中文输入开关打开时，拼音首字母根本不发往远端（index.html 先挂起，
    //     compositionstart 到点整条丢弃），组合期间不会留下按住的键；
    //   · 连接断开/失焦/观看模式切换时 releaseHeldRemoteInput() 会补发 keyup；
    //   · 长按的重复由服务端产生：rdpserver 把浏览器 repeat keydown 统一翻译成
    //     "先松再按"，不丢弃 repeat。因此 X 的 auto-repeat 开着或关着
    //     （`xset r off`）长按行为都一样。
    // 生命周期：本进程从不修改服务器的自动重复设置，也就没有"恢复"义务。
    if (waylandMode_ && !desktopSessionIsWayland()) {
        // 真正的 X11 桌面（无 Wayland 合成器 socket）：优先 X11 输入（XTest），
        // 不再尝试 uinput/Wayland portal。
        qInfo() << "InputManager: X display opened lazily, switching to X11 (XTest) input";
        waylandMode_ = false;
    } else if (waylandMode_) {
        // 桌面是 Wayland：XOpenDisplay 打开的是 XWayland，不能作为走 XTest 的
        // 依据 —— XTest 事件只进 XWayland，mutter 合成器看不到（输入全死的根因）。
        // 保持 uinput 通道：Xorg 与 mutter 都从内核 evdev 读取，Wayland/X11 通吃。
        qInfo() << "InputManager: X display is XWayland on a Wayland desktop; staying on uinput input";
    }
    return true;
}

// ── 键位卫生检查（跨进程崩溃恢复） ───────────────────────────────────────────
// 为什么安全：只处理状态文件里"本进程自己记录过"的 keycode。
//   · 释放按键：仅当该 keycode 在当前 XQueryKeymap 里仍处于按下状态才补一次
//     keyup。没有记录过的键（也就是用户物理键盘上真按住的键）一个都不碰。
//   · 恢复重复位：写回的是抑制前用 XGetKeyboardControl 记录的原值（On/Off），
//     不是 AutoRepeatModeDefault，因此不会把空闲键码解析成 off。
//   · 状态文件按 DISPLAY 绑定：换了显示会话的记录直接丢弃，不误伤新显示。
void InputManager::hygieneRestoreOnNewDisplay()
{
    const QString path = keyHygieneStatePath();
    QFile f(path);
    qInfo() << "InputManager: key hygiene check, state file" << path
            << "exists =" << f.exists()
            << "connection DISPLAY =" << openedDisplay_
            << "current DISPLAY =" << QString::fromUtf8(qgetenv("DISPLAY"));
    if (!f.exists())
        return;
    if (!f.open(QIODevice::ReadOnly)) {
        qWarning() << "InputManager: 无法读取键位卫生状态" << path;
        return;
    }
    const QJsonObject root = QJsonDocument::fromJson(f.readAll()).object();
    f.close();
    const QString savedDisplay = root.value(QStringLiteral("display")).toString();
    // 比较对象必须是"当前这条 X 连接对应的显示"，而不是此刻的进程环境 DISPLAY：
    // 服务在锁屏/解锁时会先改环境再重建 InputManager，旧实例析构时环境已是新显示，
    // 若按环境比较会把"旧连接上仍按下的键"误判成来自另一块屏而跳过释放。
    const QString curDisplay = openedDisplay_.isEmpty()
        ? QString::fromUtf8(qgetenv("DISPLAY")) : openedDisplay_;
    if (savedDisplay != curDisplay) {
        // 记录属于另一块显示（会话已换）。旧记录的 X 服务器可能已不存在，
        // 无法也不该在当前显示上补 keyup / 改 repeat。
        qInfo() << "InputManager: key hygiene state for display" << savedDisplay
                << "ignored (current display is" << curDisplay << ")";
        clearPersistedKeyHygieneState();
        return;
    }

    Display* dpy = xdisp(xDisplay_);
    const QJsonObject keys = root.value(QStringLiteral("keys")).toObject();
    char keymap[32];
    const bool haveMap = XQueryKeymap(dpy, keymap) != 0;
    QStringList released, restored;
    int (*prevHandler)(Display*, XErrorEvent*) = XSetErrorHandler(captureRepeatControlError);
    g_repeatControlError = 0;
    // 先还原上一次进程借用的 Unicode keycode 映射（kill -9 会把 kc → Uxxxx 留在
    // X 服务器上）。只按记录过的原始行还原，不发散到其它 keycode。
    const QJsonObject uni = root.value(QStringLiteral("unicode")).toObject();
    const int uniKc = uni.value(QStringLiteral("keycode")).toInt(0);
    const QJsonArray uniRow = uni.value(QStringLiteral("row")).toArray();
    if (uniKc > 8 && uniKc < 256 && !uniRow.isEmpty()) {
        if (haveMap && (keymap[uniKc / 8] & (1 << (uniKc % 8)))) {
            XTestFakeKeyEvent(dpy, static_cast<unsigned int>(uniKc), False, CurrentTime);
            released << QString::number(uniKc);
        }
        QVector<KeySym> orig;
        orig.reserve(uniRow.size());
        for (const QJsonValue& v : uniRow)
            orig.append(static_cast<KeySym>(v.toDouble()));
        XChangeKeyboardMapping(dpy, uniKc, orig.size(), orig.data(), 1);
        const int uniRepeat = uni.value(QStringLiteral("repeat")).toInt(-1);
        if (uniRepeat >= 0) {
            XKeyboardControl ctrl;
            memset(&ctrl, 0, sizeof(ctrl));
            ctrl.key = uniKc;
            ctrl.auto_repeat_mode = uniRepeat;
            XChangeKeyboardControl(dpy, KBKey | KBAutoRepeatMode, &ctrl);
            restored << QStringLiteral("unicode:") + QString::number(uniKc);
        }
        qInfo() << "InputManager: startup hygiene restored borrowed unicode keycode" << uniKc;
    }
    for (auto it = keys.constBegin(); it != keys.constEnd(); ++it) {
        const int kc = it.key().toInt();
        if (kc <= 0 || kc >= 256)
            continue;
        // 只释放"本产品记录过、且现在仍按下"的 keycode：这是上一次进程注入后
        // 没来得及回放的 keyup（含 kill -9 场景）。没有记录的用户按键不在此列。
        const bool down = haveMap && (keymap[kc / 8] & (1 << (kc % 8)));
        if (down) {
            XTestFakeKeyEvent(dpy, static_cast<unsigned int>(kc), False, CurrentTime);
            released << QString::number(kc);
        }
        XKeyboardControl ctrl;
        memset(&ctrl, 0, sizeof(ctrl));
        ctrl.key = kc;
        ctrl.auto_repeat_mode = it.value().isDouble()
            ? it.value().toInt() : AutoRepeatModeDefault;
        XChangeKeyboardControl(dpy, KBKey | KBAutoRepeatMode, &ctrl);
        restored << QString::number(kc);
    }
    XSync(dpy, False);
    XSetErrorHandler(prevHandler);
    if (g_repeatControlError != 0)
        qWarning() << "InputManager: 启动卫生检查恢复逐键重复位失败 (X error"
                   << g_repeatControlError << ")";
    qInfo() << "InputManager: startup key hygiene - released stuck keys" << released
            << ", restored auto-repeat for" << restored;
    clearPersistedKeyHygieneState();
}

void InputManager::persistKeyHygieneState()
{
    // 借用的 Unicode keycode 同样属于"本产品改过的键位"：kill -9 后映射会留在
    // X 服务器上（xmodmap 里能看到 kc → Uxxxx），也必须落盘待下次启动还原。
    if (autoRepeatSuppressed_.isEmpty() && unicodeKeycode_ == 0) {
        clearPersistedKeyHygieneState();
        return;
    }
    QJsonObject keys;
    for (auto it = autoRepeatSuppressed_.constBegin(); it != autoRepeatSuppressed_.constEnd(); ++it)
        keys.insert(QString::number(it.key()),
                    autoRepeatOriginal_.value(it.key(), AutoRepeatModeDefault));
    QJsonObject root;
    // 与 hygieneRestoreOnNewDisplay 的比对口径保持一致：记录"连接实际指向的显示"。
    root.insert(QStringLiteral("display"), openedDisplay_.isEmpty()
        ? QString::fromUtf8(qgetenv("DISPLAY")) : openedDisplay_);
    root.insert(QStringLiteral("keys"), keys);
    if (unicodeKeycode_ != 0 && unicodeDisplay_ == xDisplay_ && unicodeRowLen_ > 0) {
        QJsonObject u;
        u.insert(QStringLiteral("keycode"), static_cast<int>(unicodeKeycode_));
        QJsonArray row;
        for (int i = 0; i < unicodeOriginalRow_.size(); ++i)
            row.append(static_cast<double>(unicodeOriginalRow_.at(i)));
        u.insert(QStringLiteral("row"), row);
        u.insert(QStringLiteral("repeat"),
                 autoRepeatOriginal_.value(static_cast<int>(unicodeKeycode_), -1));
        root.insert(QStringLiteral("unicode"), u);
    }
    qInfo() << "InputManager: persist key hygiene" << root;
    QSaveFile f(keyHygieneStatePath());
    if (!f.open(QIODevice::WriteOnly)) {
        qWarning() << "InputManager: 无法写入键位卫生状态" << f.fileName();
        return;
    }
    f.write(QJsonDocument(root).toJson(QJsonDocument::Compact));
    if (!f.commit())
        qWarning() << "InputManager: 提交键位卫生状态失败" << f.fileName();
}

void InputManager::clearPersistedKeyHygieneState()
{
    const QString path = keyHygieneStatePath();
    if (QFile::exists(path) && !QFile::remove(path))
        qWarning() << "InputManager: 删除键位卫生状态失败" << path;
}

// ── 逐键 auto-repeat 抑制 ───────────────────────────────────────────────────
// 为什么是"逐键"而不是整显示 XAutoRepeatOff：
//   · X 的 auto-repeat 是**显示级全局状态**，XAutoRepeatOff 会把本机物理键盘一起
//     改成不重复；本服务端要负责恢复，而进程被杀（kill -9）时没有任何收尾机会，
//     全局状态会永久停在 off —— 这正是旧版遗留问题。
//   · 我们要解决的只是「远端按住的那颗键不该由 X 再重复一次」：远端长按的重复
//     完全由服务端产生（rdpserver 把浏览器 repeat keydown 翻译成 keyup+keydown）。
//     影响面天然就是"某一枚 keycode"，用逐键 API 正好对应。
//   · 恢复值必须是**该 keycode 抑制前的原值**（抑制前用 XGetKeyboardControl 的
//     auto_repeats 位图记录），不能统一写 AutoRepeatModeDefault：Default 只是
//     "跟随服务器全局设置"，对空闲 keycode 没有可继承的默认重复行为，部分
//     服务器会解析成 off —— 借用的临时键码恢复后若本机再按到它就不重复了。
//     写回原值（On/Off）才能真正回到改动前的状态，也不动全局 `xset r` 设置。
//
// 时序：抑制请求必须在 keydown 之前进入服务器队列（X 在"键按下 + delay 到期"时
// 才产 typematic），恢复请求在 keyup 之后。两者都在同一个 XFlush 批次里按序处理。

bool InputManager::setKeyAutoRepeat(unsigned int keycode, bool suppress)
{
    if (!xDisplay_ || keycode == 0 || keyRepeatControlUnsupported_)
        return false;

    Display* dpy = xdisp(xDisplay_);
    if (autoRepeatDisplay_ != xDisplay_) {
        // X 连接换过了：旧记录属于旧连接（旧显示的状态已随连接关闭不复存在），
        // 重新建账，并允许对新连接重新探测一次。
        autoRepeatSuppressed_.clear();
        autoRepeatOriginal_.clear();
        autoRepeatDisplay_ = xDisplay_;
        lastRepeatReconcileMs_ = 0;
        repeatControlProbed_ = false;
    }

    const int kc = static_cast<int>(keycode);
    const bool tracked = autoRepeatSuppressed_.contains(kc);
    if (suppress == tracked) {
        // 仍在按住：刷新"最近按下"时刻，让惰性对账不会把活跃的长按误判成 keyup 丢失。
        if (suppress)
            autoRepeatSuppressed_[kc] = QDateTime::currentMSecsSinceEpoch();
        return true;
    }

    // 抑制前先记下这枚 keycode 的原值（每枚键只查一次并长期缓存：恢复时总是写回
    // 原值，X 侧状态因此不会被本进程改脏，缓存不会过期）。XKeyboardState 的
    // auto_repeats 是逐键位图，1 = 该键会重复。
    if (suppress && !autoRepeatOriginal_.contains(kc)) {
        XKeyboardState st;
        if (kc < 256 && XGetKeyboardControl(dpy, &st) != 0) {
            const int byte = kc / 8;
            const int bit = kc % 8;
            autoRepeatOriginal_[kc] = (st.auto_repeats[byte] & (1 << bit))
                ? AutoRepeatModeOn : AutoRepeatModeOff;
        } else {
            // 查不到就退回 Default（不改行为，只是"原值未知"）。
            autoRepeatOriginal_[kc] = AutoRepeatModeDefault;
        }
    }

    XKeyboardControl ctrl;
    memset(&ctrl, 0, sizeof(ctrl));
    ctrl.key = kc;
    ctrl.auto_repeat_mode = suppress
        ? AutoRepeatModeOff
        : autoRepeatOriginal_.value(kc, AutoRepeatModeDefault);

    if (!repeatControlProbed_) {
        // 首次调用做一次同步往返：这是唯一能确认"逐键 API 可用"的时机。
        // 只探测一次，之后每次都只发请求 + XFlush，不给每个按键加往返延迟。
        int (*prevHandler)(Display*, XErrorEvent*) = XSetErrorHandler(captureRepeatControlError);
        g_repeatControlError = 0;
        XChangeKeyboardControl(dpy, KBKey | KBAutoRepeatMode, &ctrl);
        XSync(dpy, False);
        XSetErrorHandler(prevHandler);
        repeatControlProbed_ = true;
        if (g_repeatControlError != 0) {
            // 如实报告限制，不静默失败：注入仍照常进行，只是长按会多出 1~2 个字符
            //（X 的 typematic 与浏览器首个 repeat 重叠）。
            keyRepeatControlUnsupported_ = true;
            autoRepeatSuppressed_.clear();
            autoRepeatOriginal_.clear();
            qWarning() << "InputManager: X 服务器未实现逐键 auto-repeat 控制 (X error"
                       << g_repeatControlError << ")，无法抑制 X 的 typematic；长按可能多出"
                       << "1~2 个字符。未改动全局自动重复设置（xset q 不受影响）";
            return false;
        }
    } else {
        XChangeKeyboardControl(dpy, KBKey | KBAutoRepeatMode, &ctrl);
        XFlush(dpy);
    }

    if (suppress)
        autoRepeatSuppressed_.insert(kc, QDateTime::currentMSecsSinceEpoch());
    else
        autoRepeatSuppressed_.remove(kc);
    // [HYGIENE] 抑制账目变化时落盘（keydown 记录/全部释放时删除文件），
    // 供 kill -9 后的下一次启动恢复。文件只有几十字节，按每次按键的粒度写可接受。
    if (suppress)
        persistKeyHygieneState();
    else if (autoRepeatSuppressed_.isEmpty())
        clearPersistedKeyHygieneState();
    return true;
}

// 还清所有"欠恢复"的 keycode：keyup 丢失、断线、客户端切换、换 X 连接、析构
// 都走这里。任何路径都不能让某颗键永久停在"不重复"。
//
// 已知限制：进程被 kill -9 时没有任何收尾机会，那几颗键的"不重复"会留在 X
// 服务器上（逐键状态不随客户端连接关闭还原）；恢复办法是对相应 keycode 执行
// `xset r <keycode>`（实测 `xset r on` 只改全局模式，清不掉逐键残留）。
// 这是极端场景，正常退出/断线/失焦都已被上面的路径覆盖。
void InputManager::restoreAutoRepeatKeycodes()
{
    if (autoRepeatSuppressed_.isEmpty()) {
        autoRepeatDisplay_ = xDisplay_;
        // 正常收尾（断线/析构）没有待恢复项：清掉落盘状态，下一轮启动无副作用。
        clearPersistedKeyHygieneState();
        return;
    }
    if (!xDisplay_ || autoRepeatDisplay_ != xDisplay_) {
        // 旧连接已经关了：那份逐键状态留在旧显示服务器上（连接关闭不会自动还原
        // 键盘映射/重复标志），但本进程已无法再向它发请求。本地账目作废。
        autoRepeatSuppressed_.clear();
        autoRepeatOriginal_.clear();
        autoRepeatDisplay_ = xDisplay_;
        clearPersistedKeyHygieneState();
        return;
    }

    Display* dpy = xdisp(xDisplay_);
    const QHash<int, qint64> keys = autoRepeatSuppressed_;
    autoRepeatSuppressed_.clear();
    int (*prevHandler)(Display*, XErrorEvent*) = XSetErrorHandler(captureRepeatControlError);
    g_repeatControlError = 0;
    for (auto it = keys.constBegin(); it != keys.constEnd(); ++it) {
        XKeyboardControl ctrl;
        memset(&ctrl, 0, sizeof(ctrl));
        ctrl.key = it.key();
        // 写回抑制前记录的原值（On/Off）；见 setKeyAutoRepeat 里的记录逻辑。
        ctrl.auto_repeat_mode = autoRepeatOriginal_.value(it.key(), AutoRepeatModeDefault);
        XChangeKeyboardControl(dpy, KBKey | KBAutoRepeatMode, &ctrl);
    }
    XSync(dpy, False);
    XSetErrorHandler(prevHandler);
    if (g_repeatControlError != 0) {
        qWarning() << "InputManager: 恢复逐键自动重复失败 (X error" << g_repeatControlError
                   << ")；若发现本机某颗键长按不出重复，对该 keycode 执行 `xset r <keycode>`"
                   << "恢复（`xset r on` 只改全局模式，清不掉逐键残留）";
    } else {
        qInfo() << "InputManager: 已恢复" << keys.size() << "枚按键的自动重复设置";
    }
    clearPersistedKeyHygieneState();
}

// 惰性对账（不引入任何定时器/线程等待）：每次键盘注入前最多每 2s 查一次服务器
// 的按键状态。集合里有、但服务器显示已抬起的 keycode 说明 keyup 丢失或被其它
// 路径释放了 —— 此时它不可能再产生 typematic，立即写回原值，避免永久卡住。
void InputManager::reconcileAutoRepeatKeycodes()
{
    if (autoRepeatSuppressed_.isEmpty() || !xDisplay_
            || autoRepeatDisplay_ != xDisplay_ || keyRepeatControlUnsupported_)
        return;
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    if (lastRepeatReconcileMs_ != 0 && now - lastRepeatReconcileMs_ < 2000)
        return;
    lastRepeatReconcileMs_ = now;

    Display* dpy = xdisp(xDisplay_);
    char keyState[32];
    if (XQueryKeymap(dpy, keyState) == 0)
        return;

    QList<int> released;
    for (auto it = autoRepeatSuppressed_.constBegin(); it != autoRepeatSuppressed_.constEnd(); ++it) {
        const int kc = it.key();
        if (kc < 0 || kc >= 256)
            continue;
        // 最近 3s 内还在被"按下"刷新 → 这是一次正常长按（repeat 翻译每 ~50ms 刷新
        // 一次），不能当成 keyup 丢失，否则会周期性误放开重复。
        if (now - it.value() < 3000)
            continue;
        const int byte = kc / 8;
        const int bit = kc % 8;
        if (!(keyState[byte] & (1 << bit)))
            released.append(kc);
    }
    if (released.isEmpty())
        return;

    int (*prevHandler)(Display*, XErrorEvent*) = XSetErrorHandler(captureRepeatControlError);
    g_repeatControlError = 0;
    for (int kc : released) {
        XKeyboardControl ctrl;
        memset(&ctrl, 0, sizeof(ctrl));
        ctrl.key = kc;
        // 同 restoreAutoRepeatKeycodes：写回该键抑制前的原值。
        ctrl.auto_repeat_mode = autoRepeatOriginal_.value(kc, AutoRepeatModeDefault);
        XChangeKeyboardControl(dpy, KBKey | KBAutoRepeatMode, &ctrl);
        autoRepeatSuppressed_.remove(kc);
    }
    XSync(dpy, False);
    XSetErrorHandler(prevHandler);
    if (g_repeatControlError != 0)
        qWarning() << "InputManager: 对账恢复逐键自动重复失败 (X error" << g_repeatControlError << ")";
    else
        qInfo() << "InputManager: 检测到 keyup 丢失，已恢复" << released.size()
                << "枚按键的自动重复设置";
    // 账目变了：同步落盘（全空时自动删除），保持崩溃恢复记录准确。
    persistKeyHygieneState();
}

// ── Unicode 上屏字符注入（中文等）──────────────────────────────────────────
// X11 里没有"直接送一个字符"的入口：字符是 keycode → 服务器键盘映射 → keysym →
// 客户端查表得来的。输入法上屏的汉字在远端没有对应实体键，远端往往也没装中文
// 输入法（注入字母也组不出词）。这里借一枚空闲 keycode 当"临时汉字键"：
// XChangeKeyboardMapping 把它的映射行指向 Unicode keysym（0x01000000 + 码点），
// 再 XTEST 按下/抬起，焦点窗口解出的就是这个汉字。
// 实测结论（Xvfb 1.21 + 自建接收窗口 + xterm 落盘双验证）：
//   · 改 core 键盘映射在开着 XKB 的 Xorg 上同样生效，XKeysymToKeycode 立刻可查；
//   · 连续改 60 次再还原，整张 keymap 逐字节与改动前一致，不影响远端真实键盘；
//   · xterm 收到的是 UTF-8 三字节（E4 B8 AD = 中），中文能直接上屏。
// ASCII/字母绝不走这条路：仍按逐个按键注入，远端装了中文输入法时由远端自己组词。
//
// 竞态出在接收端的键表缓存上：按下事件一定到（日志里 keycode 有），但接收端是用
// 自己缓存的键表解 keysym 的，缓存只在它处理到 MappingNotify 时才刷新。于是有：
//   ① 借来的键**第一次**从 NoSymbol 变成有值：实测 5/10 次解成 keysym=0x0（丢字），
//      而且把按下前的等待从 5 ms 加到 15 ms 也压不下去（4/10）—— 多等不等于对端
//      真的刷了缓存。改用在借用时先绑一枚不产出文本的 F24 预热（见 borrow 里那段），
//      把"刷新"这次走完之后，20/20、12/12 全对。
//   ② 下一个字符改映射太早：本次按键被解成**下一个**字符（实测整串错位一位：
//      "中文你好测试一下é" → "文你好测试一下éé"）。所以一枚字符的映射要在它的
//      down/up 之后继续挂保持期（默认 20 ms）才允许再改。
// 保持期只在字符之间生效：同一字符的抬起不改映射，也就不额外花时间。
//
// 默认保持期 20 ms 是扫描出来的（Xvfb + xterm 落盘，12 字符连打 ×5 轮，期望串由码点算出）：
//   · 间隔 0 ms：只有最后一个字符对（整串错位）；加到 5 ms 仍有丢字，15 ms 起 5/5 全对。
//   · 把同样的时间花在"按下前等"（settle=5 / hold=0）几乎无效 —— 决定性的量是字符之间
//     的间隔，不是按下前的等待，所以预算主要给 hold。
//   · 空闲机上 2 ms 就够；把 24 核全部吃满（模拟远端正在采集+编码）后 5/10 ms 会偶发
//     丢字，15 ms 起才稳。取 20 ms 留余量。慢打字时上一次绑定早就过了保持期，
//     队列 pump 不会真的等；稳定期（默认 5 ms）短于保持期，不额外花时间。
// 保持期（字符之间）与稳定期（改映射到按键之间）默认值来自实测，但不同
// compositor/客户端刷新键表缓存的快慢差异很大，允许运行时用环境变量覆盖，
// 便于现场调优（0 = 不等待，仅排障用，可能出现丢字/错位）。
//
// 默认值由 20/5ms 调整为 60/12ms：20ms 保持期下 GTK 类应用（mousepad）偶发把
// 某个汉字解成 NoSymbol 直接吞掉（同一序列 6 次里丢 2 次"有"字，xev 侧确认 7 个
// keysym 全部送达 → 丢在接收端读键表这一步）。代价只在中文字符上（队列串行），
// 英文/ASCII 与开关关闭态完全不受影响。
static int unicodeHoldMs()
{
    static const int v = []() {
        bool ok = false;
        const int m = qEnvironmentVariableIntValue("QTRD_UNICODE_HOLD_MS", &ok);
        return ok ? qBound(0, m, 500) : 60;
    }();
    return v;
}

static int unicodeSettleMs()
{
    static const int v = []() {
        bool ok = false;
        const int m = qEnvironmentVariableIntValue("QTRD_UNICODE_SETTLE_MS", &ok);
        return ok ? qBound(0, m, 200) : 12;
    }();
    return v;
}

static const unsigned long F24KeySym = 0xffbe;   // 借用时预热用：功能键，不产出文本

// 码点 → 要绑到借用键上的 keysym（Latin-1 内沿用老规矩，其余加 Unicode 前缀）
static unsigned long unicodeWantKeySym(unsigned int codePoint)
{
    return codePoint < 0x100 ? codePoint : 0x01000000UL + codePoint;
}

// 距 sinceMs 不足保持期就补足等待。只用于极少发生的一次性收尾（还原映射），
// 逐字符注入已改为队列 + QTimer，不再在这里阻塞主线程。
static void waitUnicodeHoldMs(qint64 sinceMs)
{
    const qint64 remain = unicodeHoldMs() - (QDateTime::currentMSecsSinceEpoch() - sinceMs);
    if (remain > 0)
        QThread::msleep(static_cast<unsigned long>(qMin<qint64>(remain, 600)));
}

bool InputManager::borrowUnicodeKeycode()
{
    if (unicodeKeycode_ != 0 && unicodeDisplay_ == xDisplay_)
        return true;                       // 同一 X 连接上已借好，直接用
    if (unicodeScanFailed_ || !xDisplay_)
        return false;

    // 连接换过了（旧屏 → 新屏）：旧记录一律作废，重新找空闲键。
    unicodeKeycode_ = 0;
    unicodeBound_ = 0;
    unicodeOriginalRow_.clear();
    unicodeRowLen_ = 0;
    unicodeDisplay_ = nullptr;

    Display *dpy = xdisp(xDisplay_);
    int minKc = 0, maxKc = 0, nsyms = 0;
    XDisplayKeycodes(dpy, &minKc, &maxKc);
    KeySym *map = XGetKeyboardMapping(dpy, minKc, maxKc - minKc + 1, &nsyms);
    if (!map || nsyms <= 0 || maxKc < minKc) {
        if (map) XFree(map);
        unicodeScanFailed_ = true;
        qWarning() << "InputManager: 读取键盘映射失败，非 ASCII 字符无法注入";
        return false;
    }

    // 被登记成修饰键的 keycode 不能借：改了它们，远端的 Ctrl/Shift/Lock 行为会变。
    QVector<char> used;
    used.resize(maxKc + 1);
    for (int i = 0; i <= maxKc; ++i) used[i] = 0;
    XModifierKeymap *mod = XGetModifierMapping(dpy);
    if (mod) {
        for (int i = 0; i < 8 * mod->max_keypermod; ++i) {
            KeyCode kc = mod->modifiermap[i];
            if (kc > 0 && kc <= maxKc) used[kc] = 1;
        }
        XFreeModifiermap(mod);
    }

    // 从大到小找：真实键盘的键集中在小编号，尾部整段留空，挑编号最大的那枚最安全。
    int chosen = 0;
    for (int kc = maxKc; kc >= minKc; --kc) {
        if (kc <= 9 || used[kc]) continue;         // Esc/Tab 等一律不动
        bool empty = true;
        for (int i = 0; i < nsyms; ++i) {
            if (map[(kc - minKc) * nsyms + i] != NoSymbol) { empty = false; break; }
        }
        if (empty) { chosen = kc; break; }
    }
    if (chosen == 0) {
        XFree(map);
        unicodeScanFailed_ = true;
        qWarning() << "InputManager: 键盘映射里没有空闲 keycode，非 ASCII 字符无法注入";
        return false;
    }

    unicodeOriginalRow_.resize(nsyms);
    for (int i = 0; i < nsyms; ++i)
        unicodeOriginalRow_[i] = map[(chosen - minKc) * nsyms + i];
    XFree(map);
    unicodeRowLen_ = nsyms;
    unicodeKeycode_ = chosen;
    unicodeDisplay_ = xDisplay_;

    // 记录"借用前"的逐键重复位：XChangeKeyboardMapping 把空闲键码变成已用键时，
    // X 服务器会顺手把它的 auto-repeat 位打开。若等 setKeyAutoRepeat 在首次按下时
    // 才查询原值，就会把"映射后的 on"当成基线，归还后 xset q 位图回不到原状
    //（实测借用 keycode 250/253 会残留一位 on）。这里趁映射发生前先存原值。
    {
        XKeyboardState st;
        const int byte = chosen / 8;
        const int bit = chosen % 8;
        if (XGetKeyboardControl(dpy, &st) != 0)
            autoRepeatOriginal_[chosen] = (st.auto_repeats[byte] & (1 << bit))
                ? AutoRepeatModeOn : AutoRepeatModeOff;
        else
            autoRepeatOriginal_[chosen] = AutoRepeatModeDefault;
        qInfo() << "InputManager: 借用 keycode" << chosen
                << "repeat 原值 =" << autoRepeatOriginal_.value(chosen)
                << "bitmap byte =" << QString::number(
                       static_cast<int>(st.auto_repeats[chosen / 8]), 16);
    }

    // 借用动作本身就要落盘：kill -9 可能发生在"已改映射、还没注入/抑制"的窗口，
    // 状态文件里没有它，下次启动就无法把 kc → Uxxxx 的映射还原回去。
    persistKeyHygieneState();

    // 预热一次映射：接收端是在处理 MappingNotify 时才回头刷新自己的键表缓存，
    // 而"刚借来的键第一次由 NoSymbol 变成有值"这一步实测最容易踩到缓存还没刷新
    // 的窗口（按下事件确实到了，解出来却是 keysym=0x0，字符直接丢）。这里先把它
    // 绑成一枚不产生文本的功能键，让接收端先把刷新走一遍；F24 不产出文本，
    // 万一真被按下也只是个没人用的功能键。
    {
        Display *dpy = xdisp(xDisplay_);
        QVector<KeySym> prime(unicodeRowLen_, static_cast<KeySym>(F24KeySym));
        XChangeKeyboardMapping(dpy, unicodeKeycode_, unicodeRowLen_, prime.data(), 1);
        XFlush(dpy);
        XSync(dpy, False);
        unicodeBound_ = F24KeySym;      // 非 0：下一个真实字符会正常走改映射流程
        unicodeBoundMs_ = QDateTime::currentMSecsSinceEpoch();
    }

    qInfo() << "InputManager: 借用 keycode" << chosen << "注入非 ASCII 字符（每键"
            << nsyms << "个 keysym）";
    return true;
}

void InputManager::restoreUnicodeKeycode()
{
    if (unicodeKeycode_ == 0)
        return;
    // 借用键码也可能正被远端按住（Unicode 字符的 down 还没等到 up）：先把它从
    // "不重复"名单里还清，再还原映射，避免这颗键在断线后永久失去自动重复。
    const int ukc = static_cast<int>(unicodeKeycode_);
    if (autoRepeatSuppressed_.contains(ukc))
        setKeyAutoRepeat(static_cast<unsigned int>(ukc), false);
    // 只借过映射、没真正按下（或按下已还清）时，映射本身可能已把该 keycode 的
    // repeat 位改成 on；归还映射前按"借用前原值"写回，只碰本产品借用的这一枚。
    if (xDisplay_ && autoRepeatOriginal_.contains(ukc)) {
        Display *dpy = xdisp(xDisplay_);
        XKeyboardControl ctrl;
        memset(&ctrl, 0, sizeof(ctrl));
        ctrl.key = ukc;
        ctrl.auto_repeat_mode = autoRepeatOriginal_.value(ukc, AutoRepeatModeDefault);
        XChangeKeyboardControl(dpy, KBKey | KBAutoRepeatMode, &ctrl);
        XFlush(dpy);
        autoRepeatOriginal_.remove(ukc);
    }
    // 队列里尚未执行的字符一并作废：映射随后就被还原，再注入必然出错。
    unicodePending_.clear();
    unicodePumpScheduled_ = false;
    if (xDisplay_ && unicodeDisplay_ == xDisplay_ && unicodeRowLen_ > 0) {
        Display *dpy = xdisp(xDisplay_);
        // 原始行全为 NoSymbol（只借整行空白的键），还原后那个字符就再也解不出来了：
        // 最后一个字符可能还没被接收端读走，先等满保持期再还原。
        // 这是收尾路径（析构/换连接），只发生一次，阻塞可接受。
        if (unicodeBound_ != 0)
            waitUnicodeHoldMs(unicodeBoundMs_);
        XChangeKeyboardMapping(dpy, unicodeKeycode_, unicodeRowLen_,
                               unicodeOriginalRow_.data(), 1);
        XFlush(dpy);
    }
    unicodeKeycode_ = 0;
    unicodeBound_ = 0;
    unicodeBoundMs_ = 0;
    unicodeRowLen_ = 0;
    unicodeOriginalRow_.clear();
    unicodeDisplay_ = nullptr;
    // 映射已按原行还原、借用码也不再占用：同步清掉落盘的借用记录（若还有被抑制
    // 的键，persist 会重写一份只含这些键的文件）。
    persistKeyHygieneState();
}

// [残留按键] 旧实现这里用 XQueryKeymap 把**整个服务器**处于按下状态的键全部
// 补发抬起（只跳过借用键码）。这在共享桌面上是错的：会把本机物理用户真正按住的
// Ctrl/Shift 悄悄"松掉"，也会干扰同一台机器上的其它客户端。已删除。
// 现在的做法：只由拥有该按键记账的一方释放 —— rdpserver 按 clientId 记录
// pressedKeys_，断线/失焦时由前端 releaseHeldRemoteInput() 与
// RDPServer::onClientDisconnected 各发各的 keyup（互不影响别人的键）。
// 中文输入打开态更彻底：拼音首字母根本不会发往远端（index.html 先挂起），
// 不存在"组合期间遗留按住键"这个前提。

bool InputManager::sendUnicodeChar(unsigned int codePoint, bool isDown)
{
    if (!ensureXDisplay())
        return false;
    if (!borrowUnicodeKeycode())
        return false;

    // 只入队，不在调用线程（主线程/事件循环）里等待保持期或稳定期。
    // 改映射与按下/抬起由 pumpUnicodeQueue 在等待到期后执行，入队顺序即注入顺序，
    // 同一字符的 down/up 也保持先后。
    //
    // 乱序问题（"中a" 落成 "a中"）：字符注入要等映射的稳定期/保持期，而 ASCII
    // 若仍走 injectKeyboard 同步注入就会"超车"。因此中文输入开关打开时，rdpserver
    // 把该连接的**所有**键（含 ASCII/Enter）也交给 enqueueKeyboard，走同一个 FIFO，
    // 由本队列串行执行 —— 顺序就是消息到达顺序。开关关闭（默认）时 rdpserver 在
    // onInputReceived 入口丢弃 isChar 并直接调 injectKeyboard，本队列完全不参与。
    PendingUnicodeKey pending;
    pending.codePoint = codePoint;
    pending.isDown = isDown;
    pending.isChar = true;
    return enqueueUnicode(pending);
}

bool InputManager::enqueueKeyboard(int keycode, const QString &code, bool isDown,
                                   bool ctrl, bool alt, bool shift, bool isChar, bool meta)
{
    if (!ensureXDisplay())
        return false;
    PendingUnicodeKey pending;
    pending.isChar = isChar;
    pending.codePoint = static_cast<unsigned int>(keycode);
    pending.isDown = isDown;
    pending.keycode = keycode;
    pending.code = code;
    pending.ctrl = ctrl;
    pending.alt = alt;
    pending.shift = shift;
    pending.meta = meta;
    return enqueueUnicode(pending);
}

bool InputManager::enqueueUnicode(const PendingUnicodeKey &pending)
{
    // 字符注入需要先借到一枚空闲键码（惰性、只借一次）。普通键码不需要，
    // 借不到也不能因此把 ASCII/Enter 一起丢掉。
    if (pending.isChar && !borrowUnicodeKeycode())
        return false;
    // 上限：队列按"条目"计（一个上屏字符 = down+up 两条），256 条≈128 个汉字，
    // 覆盖一次输入法上屏/整段替换的常见上限。无上限时客户端狂发 isChar（或断线
    // 前积压）会让队列越拉越长、延迟无限增长；超限丢最旧的：用户看到的是"少字"，
    // 而不是"越打越卡死"。按 60ms 保持期，128 字的最坏排空时间约 9s（只在一次
    // 上屏就超过 128 字时才会遇到；正常逐句上屏每句 <20 字，约 1.5s 内落完）。
    static const int kMaxUnicodePending = 256;
    if (unicodePending_.size() >= kMaxUnicodePending) {
        while (unicodePending_.size() >= kMaxUnicodePending)
            unicodePending_.dequeue();
    }
    unicodePending_.enqueue(pending);
    scheduleUnicodePump();
    return true;
}

void InputManager::flushPendingInput()
{
    // 断线/析构：队列里没注入的输入已经没有意义，直接作废；再还原借用的
    // 键码 —— 否则断开后本机物理键盘按那颗键会继续打出最后一个汉字。
    // restoreUnicodeKeycode 会等满保持期再还原（一次性收尾，阻塞 ≤60ms 可接受）。
    const bool hadPending = !unicodePending_.isEmpty();
    unicodePending_.clear();
    unicodePumpScheduled_ = false;
    restoreUnicodeKeycode();
    // 注意：这里**不做**全量 restoreAutoRepeatKeycodes —— 还有别的客户端可能正
    // 按着键，放开重复等于让 X 替它多打字符。该连接的按键由 releasePressedKeys
    // 逐键补发 keyup 时恢复；"没有客户端了"的兜底由 RDPServer 单独调用。
    if (hadPending)
        qInfo() << "InputManager: 断线/退出，已丢弃未注入的中文字符并还原借用键码";
}

void InputManager::scheduleUnicodePump()
{
    if (unicodePumpScheduled_ || unicodePending_.isEmpty())
        return;

    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    qint64 readyMs = now;
    if (unicodePending_.head().isChar) {
        const unsigned long want = unicodeWantKeySym(unicodePending_.head().codePoint);
        if (want != unicodeBound_) {
            // 改映射前必须等够上一枚字符的保持期，否则接收端会把上一个按键解成新字符
            readyMs = unicodeBoundMs_ + unicodeHoldMs();
        } else {
            // 映射已绑定：同一字符的 down/up 复用，只需等映射稳定期
            readyMs = unicodeMappingReadyMs_;
        }
    }
    // 普通键码（ASCII/Enter/修饰键）不涉及映射，无需等待，排到队首就执行。

    qint64 delayMs = readyMs - now;
    if (delayMs < 0)
        delayMs = 0;
    unicodePumpScheduled_ = true;
    QTimer::singleShot(static_cast<int>(qMin<qint64>(delayMs, 60000)),
                       this, &InputManager::pumpUnicodeQueue);
}

void InputManager::pumpUnicodeQueue()
{
    unicodePumpScheduled_ = false;
    if (unicodePending_.isEmpty())
        return;
    if (!xDisplay_) {
        // X 连接已断：队列无法执行，直接作废（等下次 ensureXDisplay 重新借用键码）
        unicodePending_.clear();
        return;
    }
    Display *dpy = xdisp(xDisplay_);
    const PendingUnicodeKey head = unicodePending_.head();

    if (!head.isChar) {
        // 普通键码：直接注入（与旧路径完全相同的 keycode 语义），不碰映射。
        // 因为排在同一个 FIFO 里，它必然落在前面所有中文字符之后。
        unicodePending_.dequeue();
        injectKeyboard(head.keycode, head.code, head.isDown, head.ctrl, head.alt,
                       head.shift, false, false, head.meta);
        scheduleUnicodePump();
        return;
    }
    if (unicodeKeycode_ == 0 || unicodeRowLen_ <= 0) {
        // 没借到键码（或 X 连接已换）：字符无法注入，作废队列。
        unicodePending_.clear();
        return;
    }

    const unsigned long want = unicodeWantKeySym(head.codePoint);
    const qint64 now = QDateTime::currentMSecsSinceEpoch();

    if (want != unicodeBound_) {
        if (now < unicodeBoundMs_ + unicodeHoldMs()) {
            scheduleUnicodePump();          // 保持期未到，稍后再试
            return;
        }
        // 整行原样写回、只改前两槽：[0]=不按 Shift、[1]=按 Shift。两槽都填同一个
        // keysym，远端恰好按着 Shift 时也能出这个字符；长度用原始 keysyms-per-keycode，
        // 避免把服务器的映射行宽改成 1 而波及整张键盘表。
        QVector<KeySym> row(unicodeRowLen_);
        for (int i = 0; i < unicodeRowLen_; ++i)
            row[i] = static_cast<KeySym>(unicodeOriginalRow_[i]);
        row[0] = static_cast<KeySym>(want);
        if (unicodeRowLen_ > 1)
            row[1] = static_cast<KeySym>(want);
        XChangeKeyboardMapping(dpy, unicodeKeycode_, unicodeRowLen_, row.data(), 1);
        // 同步一次：确保服务器已应用新映射，再安排按键（稳定期后再按键）。
        XFlush(dpy);
        XSync(dpy, False);
        unicodeBound_ = want;
        unicodeBoundMs_ = now;
        unicodeMappingReadyMs_ = now + unicodeSettleMs();
        scheduleUnicodePump();
        return;
    }

    if (now < unicodeMappingReadyMs_) {
        scheduleUnicodePump();              // 稳定期未到，等映射生效再按键
        return;
    }

    unicodePending_.dequeue();

    focusLockScreenWindow(dpy);
    // 逐键 auto-repeat：借用键码也是"远端正按住的键"，按下前关掉 typematic、
    // 抬起后恢复默认。否则长按/连续上屏时 X 会在两次 repeat 之间自行补字符。
    if (head.isDown)
        setKeyAutoRepeat(static_cast<unsigned int>(unicodeKeycode_), true);
    XTestGrabControl(dpy, True);
    const int ok = XTestFakeKeyEvent(dpy, static_cast<KeyCode>(unicodeKeycode_),
                                     head.isDown, CurrentTime);
    XTestGrabControl(dpy, False);
    XFlush(dpy);
    if (!head.isDown)
        setKeyAutoRepeat(static_cast<unsigned int>(unicodeKeycode_), false);
    if (ok == 0) {
        // 旧实现不看返回值，注入失败也报成功，只能靠对端现象猜。把焦点窗口一起
        // 记下来，便于判断是"X 连接失效"还是"焦点窗口被销毁"。
        Window focusWin = None;
        int revert = 0;
        XGetInputFocus(dpy, &focusWin, &revert);
        qWarning() << "InputManager: XTestFakeKeyEvent 失败 (down=" << head.isDown
                   << ") keycode=" << unicodeKeycode_
                   << " codepoint=" << head.codePoint
                   << " focus=0x" << QString::number(focusWin, 16);
    }
    // 下一个字符（或同一字符的抬起）继续排队执行
    scheduleUnicodePump();
}

void InputManager::injectMouseMove(int x, int y) {
    lastInjX_ = x; lastInjY_ = y;
    // Wayland 门户模式：通过 RemoteDesktop D-Bus 注入
    if (waylandPortalMode_ && portalReady_) {
        // NotifyPointerMotionAbsolute(session, options, stream, x, y)
        // 接口要求 x/y 是**流内 0..1 归一化浮点**（旧实现直接传像素，超出范围的
        // 值会被门户钳到角落，表现为鼠标只能贴边）；stream 必须是 Start 应答
        // streams 里给出的真实节点 id（不存在 "0=默认"）。
        const int sw = portalStreamW_ > 0 ? portalStreamW_ : screenW_;
        const int sh = portalStreamH_ > 0 ? portalStreamH_ : screenH_;
        const bool streamOk = !portalStreamNode_.isEmpty();
        if (sw > 0 && sh > 0 && streamOk) {
            // 采集坐标（捕获区相对）→ 桌面绝对 → 流内归一化
            const double nx = double(x + screenOffsetX_ - portalStreamX_) / double(sw);
            const double ny = double(y + screenOffsetY_ - portalStreamY_) / double(sh);
            QDBusMessage msg = QDBusMessage::createMethodCall(
                PORTAL_SERVICE, PORTAL_PATH, PORTAL_IFACE,
                QStringLiteral("NotifyPointerMotionAbsolute"));
            msg << QVariant::fromValue(QDBusObjectPath(portalSessionPath_));
            msg << QVariantMap();
            msg << portalStreamNode_.toUInt();
            msg << qBound(0.0, nx, 1.0);
            msg << qBound(0.0, ny, 1.0);
            QDBusConnection::sessionBus().asyncCall(msg);
            return;
        }
    }
    // 首次注入时惰性打开 X display（env 已就绪）：X11 机器上此后走 XTest；
    // 成功会把 waylandMode_ 改回 false，避免下面误走 uinput。
    ensureXDisplay();
    // Wayland 下无 X：走 uinput 绝对定位
    if (uinputMouseFd_ < 0 && waylandMode_ && shouldRetryUinputInit())
        initUinputMouse();
    if (uinputMouseFd_ >= 0) {
        sendUinputMouseMove(x, y);
        return;
    }
    if (!xDisplay_) return;
    Display* d = xdisp(xDisplay_);
    // XTest 真实设备语义（与按钮/键盘路径统一）+ XFlush 异步刷出：
    // 原实现 XWarpPointer（程序传送语义，不经指针处理管线）+ XSync（同步往返，
    // 60Hz mousemove 时主线程每秒阻塞等待 60 次）。
    // 坐标补上捕获区在根窗口的原点偏移：多屏时主输出不一定在 (0,0)，
    // 不补偏移会让所有点击系统性偏移 (offsetX,offsetY)。
    XTestFakeMotionEvent(d, -1, x + screenOffsetX_, y + screenOffsetY_, CurrentTime);
    XFlush(d);
}

QPoint InputManager::cursorPosition() const
{
    // Wayland 桌面：XQueryPointer 只能看到 XWayland 自己的光标（恒定不动），
    // 真实光标由本进程的 uinput 虚拟指针驱动 —— 直接回报最近一次注入坐标。
    if (waylandMode_ && uinputMouseFd_ >= 0)
        return QPoint(lastInjX_, lastInjY_);
    // 惰性打开 X display：光标泵在采集启动后很快就开始跑，若不等首个输入事件
    // 才打开，queryPointer 必然失败并回落到 QCursor::pos()（服务模式下恒 (0,0)），
    // 前端光标 overlay 会一直钉在左上角。ensureXDisplay 带失败节流，可安全频繁调用。
    if (!xDisplay_)
        const_cast<InputManager*>(this)->ensureXDisplay();
    int x = -1, y = -1;
    if (queryPointer(xDisplay_, &x, &y) && x >= 0 && y >= 0)
        return QPoint(x - screenOffsetX_, y - screenOffsetY_);
    return QCursor::pos();
}

void InputManager::injectMouseButton(int x, int y, int button, bool isDown) {
    // Wayland 门户模式
    if (waylandPortalMode_ && portalReady_) {
        injectMouseMove(x, y);
        // NotifyPointerButton(session, options, button_code, state)
        // button_code: Linux evdev button codes (0x110=LEFT, 0x111=RIGHT, 0x112=MIDDLE)
        // button 编号遵循前端 DOM 约定：0=左、1=中、2=右（与下方 X11 Button1/2/3 映射一致）
        uint btnCode = (button == 0) ? 0x110 : (button == 1) ? 0x112 : 0x111;
        uint state = isDown ? 1u : 0u;
        QDBusMessage msg = QDBusMessage::createMethodCall(
            PORTAL_SERVICE, PORTAL_PATH, PORTAL_IFACE,
            QStringLiteral("NotifyPointerButton"));
        msg << QVariant::fromValue(QDBusObjectPath(portalSessionPath_));
        msg << QVariantMap();
        msg << static_cast<int>(btnCode);
        msg << state;
        QDBusConnection::sessionBus().asyncCall(msg);
        return;
    }
    injectMouseMove(x, y);
    if (uinputMouseFd_ >= 0) {
        sendUinputMouseButton(button, isDown);
        return;
    }
    if (!xDisplay_) return;
    focusLockScreenWindow(xdisp(xDisplay_));
    unsigned int xButton = (button == 0 ? Button1 : button == 1 ? Button2 : Button3);
    // XTest 真实设备事件：由 X 服务器按当前指针位置投递给光标下的**应用窗口**。
    //
    // 绝不能再改回 XSendEvent(root, propagate=True)：合成事件发到 root 后，root 自己
    // 能收到（所以 `xev -root` 会显示 BP/BR>0，极易误判为"注入成功"），但 X 并不会
    // 把它 propagate 到光标下的应用窗口 —— 实测用 xev 自建窗口收不到任何 ButtonPress，
    // 表现就是"点击无效"。
    // 推论：`xev -root` 不能用来判定按钮注入是否生效（根窗口收不到发给子窗口的
    // 真实按钮事件，XTest 在那里恒为 0）。判定必须用会实际接收按钮的窗口。
    XTestFakeButtonEvent(xdisp(xDisplay_), xButton, isDown, CurrentTime);
    XFlush(xdisp(xDisplay_));
}

void InputManager::injectWheel(int delta) {
    // Wayland 门户模式：NotifyPointerAxisDiscrete(session, options, axis, steps)
    if (waylandPortalMode_ && portalReady_) {
        QDBusMessage msg = QDBusMessage::createMethodCall(
            PORTAL_SERVICE, PORTAL_PATH, PORTAL_IFACE,
            QStringLiteral("NotifyPointerAxisDiscrete"));
        msg << QVariant::fromValue(QDBusObjectPath(portalSessionPath_));
        msg << QVariantMap();
        msg << 0u;  // axis: 0=vertical
        msg << static_cast<int>(delta > 0 ? 1 : -1);
        QDBusConnection::sessionBus().asyncCall(msg);
        return;
    }
    // 首次注入时惰性打开 X display（env 已就绪）
    ensureXDisplay();
    if (uinputWheelFd_ < 0 && waylandMode_ && shouldRetryUinputInit())
        initUinputMouse();
    if (uinputWheelFd_ >= 0) {
        sendUinputWheel(delta);
        return;
    }
    if (!xDisplay_) return;
    // wheel 消息本身不带坐标：先把指针移到最近一次注入的位置，再发按钮事件
    if (lastInjX_ != 0 || lastInjY_ != 0)
        injectMouseMove(lastInjX_, lastInjY_);
    // [I-2] 按幅度注入：X11 Button4/5 每对 press/release = 1 格，循环 |delta| 次
    unsigned int button = delta > 0 ? Button4 : Button5;
    Display* d = xdisp(xDisplay_);
    const int steps = qBound(1, qAbs(delta), 5);
    for (int i = 0; i < steps; ++i) {
        XTestFakeButtonEvent(d, button, True, CurrentTime);
        XTestFakeButtonEvent(d, button, False, CurrentTime);
    }
    XFlush(d);
}

void InputManager::primeFocusWindow() {
    // 惰性 X 连接在"首次注入"才打开；但启动卫生检查（恢复上一进程 kill -9
    // 遗留的卡键/重复位）挂在 ensureXDisplay 上，若等到首个按键才执行，卡住的键
    // 会在整个空闲窗口里一直按着。采集启动时主动建连，让卫生检查随启动落地。
    if (!ensureXDisplay())
        return;
    if (xDisplay_)
        focusLockScreenWindow(xdisp(xDisplay_));
}

void InputManager::focusLockScreenWindow(void* dpy)
{
    Display* display = static_cast<Display*>(dpy);

    // 仅在锁屏/安全输入会话下方可抢焦点。普通桌面下，X11 会把 XTest 注入的
    // 键盘/鼠标事件派发给“当前活动窗口”，无需也不应该 XSetInputFocus 改焦点。
    // 否则会扫到任意“全屏或置顶(ABOVE)”窗口并将其强设为焦点，抢走记事本等
    // 正在输入的应用的焦点（键盘输入几秒后失焦）。
    if (!screenLocked_)
        return;

    // 焦点管理带 2 秒节流：已成功处理过且短时间内无需再校验，
    // 后续按键直接走 XTest（无 X 同步往返，QEMU/软渲染下首次按键不再明显卡顿）。
    // 窗口变化（锁屏/解锁）时通过 lockScreenWindow_ 置 0 强制重新处理。
    qint64 now = QDateTime::currentMSecsSinceEpoch();
    if (lockScreenWindow_ != None && (now - focusCheckedMs_) < 2000)
        return;

    Window curFocus = None; int revert = 0;
    XGetInputFocus(display, &curFocus, &revert);

    // 焦点未变化：直接复用缓存窗口，避免整棵窗口树遍历
    // + 多次同步 X 往返（QEMU/无 GPU 环境下按键延迟明显）。
    if (lockScreenWindow_ != None && curFocus == lockScreenWindow_) {
        focusCheckedMs_ = now;
        return;
    }

    Window found = findLockScreenWindow(display);
    if (found == None) {
        // Try a simpler approach: just try the current focus window
        if (curFocus != None && curFocus != DefaultRootWindow(display)) {
            found = curFocus;
        }
    }

    if (found != None) {
        lockScreenWindow_ = found;
        Atom netActiveWindow = XInternAtom(display, "_NET_ACTIVE_WINDOW", False);
        if (netActiveWindow != None) {
            XEvent ev = {};
            ev.type = ClientMessage;
            ev.xclient.window = found;
            ev.xclient.message_type = netActiveWindow;
            ev.xclient.format = 32;
            ev.xclient.data.l[0] = 1;
            ev.xclient.data.l[1] = CurrentTime;
            ev.xclient.data.l[2] = 0;
            XSendEvent(display, DefaultRootWindow(display), False,
                       SubstructureNotifyMask | SubstructureRedirectMask, &ev);
        }
        XSetInputFocus(display, found, RevertToPointerRoot, CurrentTime);
        XFlush(display);
    }
    focusCheckedMs_ = now;
}

bool InputManager::sendXModifier(X11KeySym ks, bool isDown) {
    // Wayland 门户优先
    if (waylandPortalMode_ && portalReady_) {
        unsigned short lkc = keysymToLinuxKeycode(static_cast<unsigned long>(ks));
        if (lkc != 0) {
            sendPortalKey(lkc, isDown);
            return true;
        }
        return false;
    }
    // uinput 优先（Wayland 会话无门户时也自动初始化）
    if (uinputFd_ >= 0 || waylandMode_) {
        if (uinputFd_ < 0 && !initUinput())
            return false;
        unsigned short lkc = keysymToLinuxKeycode(static_cast<unsigned long>(ks));
        if (lkc != 0) {
            sendUinputKey(lkc, isDown);
            return true;
        }
        return false;
    }
    if (!xDisplay_) return false;
    KeyCode kc = XKeysymToKeycode(xdisp(xDisplay_), static_cast<KeySym>(ks));
    if (kc != 0) {
        focusLockScreenWindow(xdisp(xDisplay_));
        XTestGrabControl(xdisp(xDisplay_), True);
        XTestFakeKeyEvent(xdisp(xDisplay_), kc, isDown, CurrentTime);
        XTestGrabControl(xdisp(xDisplay_), False);
        return true;
    }
    return false;
}

bool InputManager::initUinput()
{
    if (uinputFd_ >= 0)
        return true;

    // 锁屏切换：清空缓存的窗口，切回 XTest 时重新查找
    lockScreenWindow_ = 0;

    int fd = open("/dev/uinput", O_WRONLY | O_NONBLOCK);
    if (fd < 0) {
        fd = open("/dev/input/uinput", O_WRONLY | O_NONBLOCK);
        if (fd < 0) {
            qWarning() << "InputManager: Cannot open uinput device (need /dev/uinput access)";
            return false;
        }
    }

    ioctl(fd, UI_SET_EVBIT, EV_KEY);
    ioctl(fd, UI_SET_EVBIT, EV_SYN);

    // 注册到 248（KEY_MIN_INTERESTING 内安全范围）：Menu 键映射 KEY_COMPOSE(127)，
    // 原上限 126 会导致 UI_SET_KEYBIT 对 127 失败 → uinput 注入 Menu 键无效。
    for (int i = 1; i <= 248; i++)
        ioctl(fd, UI_SET_KEYBIT, i);

#ifdef UI_DEV_SETUP
    // 新版 uinput API (内核 ≥ 4.x)
    struct uinput_setup usetup = {};
    usetup.id.bustype = BUS_USB;
    usetup.id.vendor = 0x1234;
    usetup.id.product = 0x5678;
    snprintf(usetup.name, sizeof(usetup.name), "QtRemoteDesktop Virtual Keyboard");

    if (ioctl(fd, UI_DEV_SETUP, &usetup) < 0) {
        qWarning() << "InputManager: uinput UI_DEV_SETUP failed";
        close(fd);
        return false;
    }
#else
    // 旧版 uinput API (RHEL 7 / CentOS 7)
    struct uinput_user_dev usetup;
    memset(&usetup, 0, sizeof(usetup));
    usetup.id.bustype = BUS_USB;
    usetup.id.vendor = 0x1234;
    usetup.id.product = 0x5678;
    snprintf(usetup.name, sizeof(usetup.name), "QtRemoteDesktop Virtual Keyboard");

    if (write(fd, &usetup, sizeof(usetup)) < 0) {
        qWarning() << "InputManager: uinput write failed";
        close(fd);
        return false;
    }
#endif

    if (ioctl(fd, UI_DEV_CREATE) < 0) {
        qWarning() << "InputManager: uinput UI_DEV_CREATE failed";
        close(fd);
        return false;
    }

    uinputFd_ = fd;
    // udev/libinput 热插拔延迟：UI_DEV_CREATE 后立即写首事件会被合成器吞掉
    // （mutter 需处理 udev add 事件才能看到新设备，实测约百毫秒级）。
    // 设备整个进程生命周期只创建一次，这里的等待只影响首个按键，代价可忽略。
    // （外部独立注入器等 1.2s 实验证明：等待后首个事件即可达桌面）
    usleep(200000);
    qInfo() << "InputManager: uinput device created";

    // 键盘设备就绪后，一并创建鼠标（绝对定位）与滚轮设备（Wayland 需要）
    initUinputMouse();
    return true;
}

bool InputManager::shouldRetryUinputInit()
{
    // 节流：uinput 不可用（无 /dev/uinput 权限）时，指针/滚轮事件每次都调
    // initUinputMouse() 会 open() 两次 + 打警告（60Hz ≈ 120 次/秒的 open 风暴
    // 与日志淹没）。这里限制为最多每 5s 尝试一次。
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    if (lastUinputInitMs_ != 0 && (now - lastUinputInitMs_) < 5000)
        return false;
    lastUinputInitMs_ = now;
    return true;
}

bool InputManager::initUinputMouse()
{
    if (uinputMouseFd_ >= 0 && uinputWheelFd_ >= 0)
        return true;

    // [B-3] 修复泄漏：指针设备已在而滚轮设备缺失（如首次滚轮创建失败）时，
    // 只补建滚轮设备。原实现重入会再开一个指针设备且旧 fd 未 close 即被覆盖，
    // 每次滚轮事件泄漏一个设备 + fd 直至耗尽。
    if (uinputMouseFd_ >= 0)
        return initUinputWheelDevice();

    int fd = open("/dev/uinput", O_WRONLY | O_NONBLOCK);
    if (fd < 0) {
        fd = open("/dev/input/uinput", O_WRONLY | O_NONBLOCK);
        if (fd < 0) {
            qWarning() << "InputManager: cannot open uinput for mouse (need /dev/uinput access)";
            return false;
        }
    }

    // ---- 绝对定位指针设备（ABS_X/ABS_Y 0..65535，mutter/libinput 识别为绝对指针）----
    ioctl(fd, UI_SET_EVBIT, EV_KEY);
    ioctl(fd, UI_SET_EVBIT, EV_ABS);
    ioctl(fd, UI_SET_EVBIT, EV_SYN);
    ioctl(fd, UI_SET_KEYBIT, BTN_LEFT);
    ioctl(fd, UI_SET_KEYBIT, BTN_RIGHT);
    ioctl(fd, UI_SET_KEYBIT, BTN_MIDDLE);
    ioctl(fd, UI_SET_ABSBIT, ABS_X);
    ioctl(fd, UI_SET_ABSBIT, ABS_Y);

#ifdef UI_DEV_SETUP
    struct uinput_setup usetup = {};
    usetup.id.bustype = BUS_USB;
    usetup.id.vendor = 0x1234;
    usetup.id.product = 0x5679;
    snprintf(usetup.name, sizeof(usetup.name), "QtRemoteDesktop Virtual Pointer");
    if (ioctl(fd, UI_DEV_SETUP, &usetup) < 0) {
        // 旧内核（未实现 UI_DEV_SETUP，ioctl 返回 EINVAL）：回退到旧 API，
        // 否则指针设备建不出来 / 绝对轴沿用内核默认范围，定位会被钳边。
        qWarning() << "InputManager: pointer UI_DEV_SETUP failed, using legacy uinput_user_dev";
        struct uinput_user_dev legacy;
        memset(&legacy, 0, sizeof(legacy));
        legacy.id.bustype = BUS_USB;
        legacy.id.vendor = 0x1234;
        legacy.id.product = 0x5679;
        snprintf(legacy.name, sizeof(legacy.name), "QtRemoteDesktop Virtual Pointer");
        legacy.absmin[ABS_X] = 0;
        legacy.absmax[ABS_X] = 65535;
        legacy.absmin[ABS_Y] = 0;
        legacy.absmax[ABS_Y] = 65535;
        if (write(fd, &legacy, sizeof(legacy)) < 0) {
            qWarning() << "InputManager: pointer uinput write (legacy) failed";
            close(fd);
            return false;
        }
    }
#else
    // 旧版 uinput API (RHEL 7 / CentOS 7 等旧内核头)：没有 UI_DEV_SETUP/UI_ABS_SETUP，
    // 设备信息与绝对轴范围必须在 uinput_user_dev 里一次性写下去 —— 缺 absmin/absmax
    // 时内核按默认范围处理，绝对定位会贴边/错位。参照键盘设备的旧 API 分支。
    struct uinput_user_dev usetup;
    memset(&usetup, 0, sizeof(usetup));
    usetup.id.bustype = BUS_USB;
    usetup.id.vendor = 0x1234;
    usetup.id.product = 0x5679;
    snprintf(usetup.name, sizeof(usetup.name), "QtRemoteDesktop Virtual Pointer");
    usetup.absmin[ABS_X] = 0;
    usetup.absmax[ABS_X] = 65535;
    usetup.absmin[ABS_Y] = 0;
    usetup.absmax[ABS_Y] = 65535;
    if (write(fd, &usetup, sizeof(usetup)) < 0) {
        qWarning() << "InputManager: pointer uinput write failed";
        close(fd);
        return false;
    }
#endif

#ifdef UI_ABS_SETUP
    struct uinput_abs_setup absx = {};
    absx.code = ABS_X;
    absx.absinfo.minimum = 0;
    absx.absinfo.maximum = 65535;
    absx.absinfo.fuzz = 0;
    absx.absinfo.flat = 0;
    ioctl(fd, UI_ABS_SETUP, &absx);
    struct uinput_abs_setup absy = {};
    absy.code = ABS_Y;
    absy.absinfo.minimum = 0;
    absy.absinfo.maximum = 65535;
    absy.absinfo.fuzz = 0;
    absy.absinfo.flat = 0;
    ioctl(fd, UI_ABS_SETUP, &absy);
#endif

    if (ioctl(fd, UI_DEV_CREATE) < 0) {
        qWarning() << "InputManager: pointer UI_DEV_CREATE failed";
        close(fd);
        return false;
    }
    uinputMouseFd_ = fd;
    // 同 uinput 键盘：给 udev/libinput 热插拔留时间，防止首个绝对定位事件丢失
    usleep(200000);

    // ---- 滚轮设备（EV_REL，独立设备避免与绝对定位冲突）----
    initUinputWheelDevice();

    qInfo() << "InputManager: uinput mouse devices created (pointer" << uinputMouseFd_
            << "wheel" << uinputWheelFd_ << ")";
    return true;
}

bool InputManager::initUinputWheelDevice()
{
    if (uinputWheelFd_ >= 0)
        return true;
    int wfd = open("/dev/uinput", O_WRONLY | O_NONBLOCK);
    if (wfd < 0)
        wfd = open("/dev/input/uinput", O_WRONLY | O_NONBLOCK);
    if (wfd < 0) {
        qWarning() << "InputManager: cannot open uinput for wheel device";
        return false;
    }
    ioctl(wfd, UI_SET_EVBIT, EV_REL);
    ioctl(wfd, UI_SET_EVBIT, EV_SYN);
    ioctl(wfd, UI_SET_RELBIT, REL_WHEEL);
    ioctl(wfd, UI_SET_RELBIT, REL_HWHEEL);
#ifdef UI_DEV_SETUP
    struct uinput_setup wsetup = {};
    wsetup.id.bustype = BUS_USB;
    wsetup.id.vendor = 0x1234;
    wsetup.id.product = 0x567A;
    snprintf(wsetup.name, sizeof(wsetup.name), "QtRemoteDesktop Virtual Wheel");
    ioctl(wfd, UI_DEV_SETUP, &wsetup);
#endif
    if (ioctl(wfd, UI_DEV_CREATE) < 0) {
        close(wfd);
        qWarning() << "InputManager: wheel UI_DEV_CREATE failed";
        return false;
    }
    uinputWheelFd_ = wfd;
    usleep(200000);  // 滚轮设备热插拔延迟，防首个滚轮事件丢失
    return true;
}

bool InputManager::sendUinputMouseMove(int x, int y)
{
    if (uinputMouseFd_ < 0)
        return false;
    // offscreen 平台下 primaryScreen 尺寸不可靠：优先用 WaylandCapturer 共享的真实分辨率
    int sw = qEnvironmentVariableIntValue("QTRD_WAYLAND_WIDTH");
    int sh = qEnvironmentVariableIntValue("QTRD_WAYLAND_HEIGHT");
    if (sw <= 0 || sh <= 0) {
        QSize sz = QGuiApplication::primaryScreen()
            ? QGuiApplication::primaryScreen()->size() : QSize(1920, 1080);
        sw = sz.width();
        sh = sz.height();
    }
    // 采集坐标（捕获区相对）→ 桌面绝对：与 XTest/portal 路径一致，多屏时补上
    // 捕获区在虚拟桌面中的原点偏移，否则副屏点击系统性偏移 (offsetX,offsetY)。
    int ax = sw > 0 ? qBound(0, (x + screenOffsetX_) * 65535 / sw, 65535) : 0;
    int ay = sh > 0 ? qBound(0, (y + screenOffsetY_) * 65535 / sh, 65535) : 0;

    // 每个事件单独 write：同一 struct 覆写再写只发最后一个 SYN，指针不会移动
    struct input_event ev = {};
    ev.type = EV_ABS; ev.code = ABS_X; ev.value = ax;
    if (write(uinputMouseFd_, &ev, sizeof(ev)) != static_cast<ssize_t>(sizeof(ev)))
        return false;
    ev.type = EV_ABS; ev.code = ABS_Y; ev.value = ay;
    if (write(uinputMouseFd_, &ev, sizeof(ev)) != static_cast<ssize_t>(sizeof(ev)))
        return false;
    ev.type = EV_SYN; ev.code = SYN_REPORT; ev.value = 0;
    return write(uinputMouseFd_, &ev, sizeof(ev)) == static_cast<ssize_t>(sizeof(ev));
}

bool InputManager::sendUinputMouseButton(int button, bool isDown)
{
    if (uinputMouseFd_ < 0)
        return false;
    // 前端约定 button: 0=左 1=中 2=右（e.button）。旧映射把 1→BTN_RIGHT、
    // 2→BTN_MIDDLE，Wayland/uinput 路径中右键互换；X11 路径
    //（Button1/2/3=左/中/右）本来就对，这里对齐为相同语义。
    unsigned short code = (button == 0) ? BTN_LEFT
                        : (button == 1) ? BTN_MIDDLE : BTN_RIGHT;
    struct input_event ev = {};
    ev.type = EV_KEY; ev.code = code; ev.value = isDown ? 1 : 0;
    if (write(uinputMouseFd_, &ev, sizeof(ev)) != static_cast<ssize_t>(sizeof(ev)))
        return false;
    ev.type = EV_SYN; ev.code = SYN_REPORT; ev.value = 0;
    return write(uinputMouseFd_, &ev, sizeof(ev)) == static_cast<ssize_t>(sizeof(ev));
}

bool InputManager::sendUinputWheel(int delta)
{
    if (uinputWheelFd_ < 0)
        return false;
    // [I-2] 保留滚动幅度：REL_WHEEL 的 value 即格数（正=上/负=下），
    // 原实现 ±1 丢失幅度（触控板/高速滚动被压成单格）。钳制 ±5 防事件风暴。
    if (delta > 5) delta = 5;
    if (delta < -5) delta = -5;
    struct input_event ev = {};
    ev.type = EV_REL; ev.code = REL_WHEEL; ev.value = delta;
    if (write(uinputWheelFd_, &ev, sizeof(ev)) != static_cast<ssize_t>(sizeof(ev)))
        return false;
    ev.type = EV_SYN; ev.code = SYN_REPORT; ev.value = 0;
    return write(uinputWheelFd_, &ev, sizeof(ev)) == static_cast<ssize_t>(sizeof(ev));
}

void InputManager::destroyUinput()
{
    if (uinputFd_ < 0 && uinputMouseFd_ < 0 && uinputWheelFd_ < 0)
        return;
    if (uinputFd_ >= 0) {
        ioctl(uinputFd_, UI_DEV_DESTROY);
        close(uinputFd_);
        uinputFd_ = -1;
    }
    if (uinputMouseFd_ >= 0) {
        ioctl(uinputMouseFd_, UI_DEV_DESTROY);
        close(uinputMouseFd_);
        uinputMouseFd_ = -1;
    }
    if (uinputWheelFd_ >= 0) {
        ioctl(uinputWheelFd_, UI_DEV_DESTROY);
        close(uinputWheelFd_);
        uinputWheelFd_ = -1;
    }
    lockScreenWindow_ = 0; // 解锁：清空缓存，下次按键重新查找当前焦点窗口
    qInfo() << "InputManager: uinput devices destroyed";
}

bool InputManager::sendUinputKey(unsigned short linuxKeycode, bool isDown)
{
    if (uinputFd_ < 0)
        return false;

    struct input_event ev = {};
    ev.type = EV_KEY;
    ev.code = linuxKeycode;
    ev.value = isDown ? 1 : 0;
    ev.time.tv_sec = 0;
    ev.time.tv_usec = 0;

    if (write(uinputFd_, &ev, sizeof(ev)) < 0)
        return false;

    ev.type = EV_SYN;
    ev.code = SYN_REPORT;
    ev.value = 0;
    if (write(uinputFd_, &ev, sizeof(ev)) < 0)
        return false;

    return true;
}

unsigned short InputManager::keysymToLinuxKeycode(unsigned long ks)
{
    static const unsigned short letterKeys[] = {
        30, 48, 46, 32, 18, 33, 34, 35, 23, 36, 37, 38, 50,
        49, 24, 25, 16, 19, 31, 20, 22, 47, 17, 45, 21, 44
    };
    static const unsigned short digitKeys[] = {
        11, 2, 3, 4, 5, 6, 7, 8, 9, 10
    };

    if (ks >= XK_A && ks <= XK_Z)
        return letterKeys[ks - XK_A];
    if (ks >= XK_a && ks <= XK_z)
        return letterKeys[ks - XK_a];
    if (ks >= XK_0 && ks <= XK_9)
        return digitKeys[ks - XK_0];

    switch (ks) {
        case XK_Return: case XK_KP_Enter: return KEY_ENTER;
        case XK_BackSpace: return KEY_BACKSPACE;
        case XK_Tab: return KEY_TAB;
        case XK_Escape: return KEY_ESC;
        case XK_space: case XK_KP_Space: return KEY_SPACE;
        case XK_Delete: return KEY_DELETE;
        case XK_Insert: return KEY_INSERT;
        case XK_Home: return KEY_HOME;
        case XK_End: return KEY_END;
        case XK_Page_Up: return KEY_PAGEUP;
        case XK_Page_Down: return KEY_PAGEDOWN;
        case XK_Up: return KEY_UP;
        case XK_Down: return KEY_DOWN;
        case XK_Left: return KEY_LEFT;
        case XK_Right: return KEY_RIGHT;
        case XK_Shift_L: return KEY_LEFTSHIFT;
        case XK_Shift_R: return KEY_RIGHTSHIFT;
        case XK_Control_L: return KEY_LEFTCTRL;
        case XK_Control_R: return KEY_RIGHTCTRL;
        case XK_Alt_L: return KEY_LEFTALT;
        case XK_Alt_R: return KEY_RIGHTALT;
        case XK_Meta_L: return KEY_LEFTMETA;
        case XK_Meta_R: return KEY_RIGHTMETA;
        case XK_Caps_Lock: return KEY_CAPSLOCK;
        case XK_Num_Lock: return KEY_NUMLOCK;
        case XK_Scroll_Lock: return KEY_SCROLLLOCK;
        case XK_F1: return KEY_F1;
        case XK_F2: return KEY_F2;
        case XK_F3: return KEY_F3;
        case XK_F4: return KEY_F4;
        case XK_F5: return KEY_F5;
        case XK_F6: return KEY_F6;
        case XK_F7: return KEY_F7;
        case XK_F8: return KEY_F8;
        case XK_F9: return KEY_F9;
        case XK_F10: return KEY_F10;
        case XK_F11: return KEY_F11;
        case XK_F12: return KEY_F12;
        case XK_minus: return KEY_MINUS;
        case XK_equal: return KEY_EQUAL;
        case XK_bracketleft: return KEY_LEFTBRACE;
        case XK_bracketright: return KEY_RIGHTBRACE;
        case XK_semicolon: return KEY_SEMICOLON;
        case XK_apostrophe: return KEY_APOSTROPHE;
        case XK_grave: return KEY_GRAVE;
        case XK_comma: return KEY_COMMA;
        case XK_period: return KEY_DOT;
        case XK_slash: return KEY_SLASH;
        case XK_backslash: return KEY_BACKSLASH;
        case XK_KP_Multiply: return KEY_KPASTERISK;
        case XK_KP_Add: return KEY_KPPLUS;
        case XK_KP_Subtract: return KEY_KPMINUS;
        case XK_KP_Divide: return KEY_KPSLASH;
        case XK_KP_Decimal: return KEY_KPDOT;
        case XK_KP_0: return KEY_KP0;
        case XK_KP_1: return KEY_KP1;
        case XK_KP_2: return KEY_KP2;
        case XK_KP_3: return KEY_KP3;
        case XK_KP_4: return KEY_KP4;
        case XK_KP_5: return KEY_KP5;
        case XK_KP_6: return KEY_KP6;
        case XK_KP_7: return KEY_KP7;
        case XK_KP_8: return KEY_KP8;
        case XK_KP_9: return KEY_KP9;
        case XK_Pause: return KEY_PAUSE;
        case XK_Print: return KEY_PRINT;
        case XK_Menu: return KEY_COMPOSE;
        case XK_Super_L: return KEY_LEFTMETA;
        case XK_Super_R: return KEY_RIGHTMETA;
        default: return 0;
    }
}

void InputManager::injectKeyboard(int keycode, const QString& code, bool isDown, bool ctrl, bool alt, bool shift, bool useVkFallback, bool isChar, bool meta) {
    Q_UNUSED(useVkFallback);
    unsigned int unicodeCp = 0;   // 非 ASCII 上屏字符的码点（0 = 没走 Unicode 路）

    // 惰性对账：keyup 丢失的键不会永久停在"不重复"（详见 reconcileAutoRepeatKeycodes）
    reconcileAutoRepeatKeycodes();

    // Wayland 门户模式：通过 D-Bus 注入键盘
    if (waylandPortalMode_ && portalReady_) {
        // 先同步修饰键状态（Ctrl/Alt/Shift），保证组合键（Ctrl+C 等）生效
        updateModifiers(ctrl, alt, shift);
        // Meta(Super) 修饰键同步：sendXModifier 内部按 portal 优先路由
        // 注入失败不提交记账（与 Ctrl/Alt/Shift 一致），否则 metaDown_ 与真实
        // 修饰键状态失同步，后续 Super 组合键永久丢失。
        if (meta != metaDown_) {
            if (sendXModifier(static_cast<X11KeySym>(XK_Super_L), meta))
                metaDown_ = meta;
        }
        // 修饰键本身已由 updateModifiers 注入
        if (code == "ControlLeft" || code == "ControlRight" ||
            code == "ShiftLeft" || code == "ShiftRight" ||
            code == "AltLeft" || code == "AltRight" ||
            code == "MetaLeft" || code == "MetaRight") {
            return;
        }
        // 通过 code 字符串映射到 Linux evdev keycode
        unsigned int lk = keysymToLinuxKeycode(
            [code]() -> unsigned long {
                if (code == "Backspace") return XK_BackSpace;
                if (code == "Enter")    return XK_Return;
                if (code == "Tab")      return XK_Tab;
                if (code == "Escape")   return XK_Escape;
                if (code == "Space")    return XK_space;
                if (code == "Delete")   return XK_Delete;
                if (code == "ArrowUp")  return XK_Up;
                if (code == "ArrowDown") return XK_Down;
                if (code == "ArrowLeft") return XK_Left;
                if (code == "ArrowRight") return XK_Right;
                if (code.startsWith("Key") && code.length() == 4) {
                    QByteArray str(1, code[3].toLower().toLatin1());
                    return XStringToKeysym(str.constData());
                }
                if (code.startsWith("Digit") && code.length() == 6) {
                    QByteArray str(1, code[5].toLatin1());
                    return XStringToKeysym(str.constData());
                }
                if (code == "ControlLeft")  return XK_Control_L;
                if (code == "ControlRight") return XK_Control_R;
                if (code == "ShiftLeft")    return XK_Shift_L;
                if (code == "ShiftRight")   return XK_Shift_R;
                if (code == "AltLeft")      return XK_Alt_L;
                if (code == "AltRight")     return XK_Alt_R;
                if (code == "MetaLeft")     return XK_Meta_L;
                if (code == "MetaRight")    return XK_Meta_R;
                return 0;
            }());
        // 兜底：code 为空且 isChar 时，把 JS keycode 视作 ASCII 码点映射
        if (lk == 0 && isChar && keycode > 0) {
            unsigned long ks = 0;
            if (keycode >= 'A' && keycode <= 'Z') ks = XK_A + (keycode - 'A');
            else if (keycode >= 'a' && keycode <= 'z') ks = XK_a + (keycode - 'a');
            else if (keycode >= '0' && keycode <= '9') ks = XK_0 + (keycode - '0');
            if (ks) lk = keysymToLinuxKeycode(ks);
        }
        if (lk != 0)
            sendPortalKey(lk, isDown);
        return;
    }
    bool charShift = false;
    KeySym keySym = NoSymbol;

    if (isChar && code.isEmpty() && keycode > 0) {
        // 锁屏密码逐字符注入：前端只给 Unicode 码点（code 为空、isChar=true）。
        // 此前这里被 Q_UNUSED 丢弃，导致密码字符全部丢失、只有末尾回车生效。
        keySym = static_cast<KeySym>(asciiToKeysym(keycode, &charShift));
        if (keySym == NoSymbol) {
            // 不是 ASCII：输入法上屏的汉字/其它非拉丁字符。记下码点，等修饰键
            // 同步完成后走"借 keycode + Unicode keysym"的注入路。
            // ASCII 永远进不到这里，英文仍逐键注入字母，远端装了中文输入法时
            // 由远端自己组词。码点来自客户端，超出 Unicode 范围的不绑（绑了也
            // 解不出字符，只会白占保持期）。
            if (keycode <= 0x10FFFF)
                unicodeCp = static_cast<unsigned int>(keycode);
        }
    }
    else if (code == "Delete")      keySym = XK_Delete;
    else if (code == "Backspace") keySym = XK_BackSpace;
    else if (code == "Enter")  keySym = XK_Return;
    else if (code == "Tab")    keySym = XK_Tab;
    else if (code == "Escape") keySym = XK_Escape;
    else if (code == "Space")  keySym = XK_space;
    else if (code == "ArrowUp")    keySym = XK_Up;
    else if (code == "ArrowDown")  keySym = XK_Down;
    else if (code == "ArrowLeft")  keySym = XK_Left;
    else if (code == "ArrowRight") keySym = XK_Right;
    else if (code == "Home")   keySym = XK_Home;
    else if (code == "End")    keySym = XK_End;
    else if (code == "PageUp") keySym = XK_Page_Up;
    else if (code == "PageDown") keySym = XK_Page_Down;
    else if (code == "Insert") keySym = XK_Insert;
    else if (code.startsWith("Key") && code.length() == 4) {
        QChar c = code[3];
        QByteArray str = QByteArray(1, c.toLower().toLatin1());
        keySym = XStringToKeysym(str.constData());
    }     else if (code.startsWith("Digit") && code.length() == 6) {
        QChar c = code[5];
        QByteArray str = QByteArray(1, c.toLatin1());
        keySym = XStringToKeysym(str.constData());
    }
    else if (code == "Period")       keySym = XK_period;
    else if (code == "Comma")       keySym = XK_comma;
    else if (code == "Slash")       keySym = XK_slash;
    else if (code == "Semicolon")   keySym = XK_semicolon;
    else if (code == "Quote")       keySym = XK_apostrophe;
    else if (code == "BracketLeft") keySym = XK_bracketleft;
    else if (code == "BracketRight") keySym = XK_bracketright;
    else if (code == "Backslash")   keySym = XK_backslash;
    else if (code == "Minus")       keySym = XK_minus;
    else if (code == "Equal")       keySym = XK_equal;
    else if (code == "Backquote")   keySym = XK_grave;
    else if (code == "F1")  keySym = XK_F1;
    else if (code == "F2")  keySym = XK_F2;
    else if (code == "F3")  keySym = XK_F3;
    else if (code == "F4")  keySym = XK_F4;
    else if (code == "F5")  keySym = XK_F5;
    else if (code == "F6")  keySym = XK_F6;
    else if (code == "F7")  keySym = XK_F7;
    else if (code == "F8")  keySym = XK_F8;
    else if (code == "F9")  keySym = XK_F9;
    else if (code == "F10") keySym = XK_F10;
    else if (code == "F11") keySym = XK_F11;
    else if (code == "F12") keySym = XK_F12;
    else if (code == "ControlLeft")  keySym = XK_Control_L;
    else if (code == "ControlRight") keySym = XK_Control_R;
    else if (code == "ShiftLeft")    keySym = XK_Shift_L;
    else if (code == "ShiftRight")   keySym = XK_Shift_R;
    else if (code == "AltLeft")      keySym = XK_Alt_L;
    else if (code == "AltRight")     keySym = XK_Alt_R;
    else if (code.startsWith("Numpad")) {
        if (code == "NumpadEnter")     keySym = XK_Return;
        else if (code == "NumpadAdd")       keySym = XK_KP_Add;
        else if (code == "NumpadSubtract")  keySym = XK_KP_Subtract;
        else if (code == "NumpadMultiply")  keySym = XK_KP_Multiply;
        else if (code == "NumpadDivide")    keySym = XK_KP_Divide;
        else if (code == "NumpadDecimal")   keySym = XK_KP_Decimal;
        else if (code == "Numpad0") keySym = XK_KP_0;
        else if (code == "Numpad1") keySym = XK_KP_1;
        else if (code == "Numpad2") keySym = XK_KP_2;
        else if (code == "Numpad3") keySym = XK_KP_3;
        else if (code == "Numpad4") keySym = XK_KP_4;
        else if (code == "Numpad5") keySym = XK_KP_5;
        else if (code == "Numpad6") keySym = XK_KP_6;
        else if (code == "Numpad7") keySym = XK_KP_7;
        else if (code == "Numpad8") keySym = XK_KP_8;
        else if (code == "Numpad9") keySym = XK_KP_9;
    }
    else {
        keySym = XStringToKeysym(code.toLatin1().constData());
        if (keySym == NoSymbol) {
            qWarning() << "InputManager: unmapped key code:" << code << "keycode:" << keycode;
        }
    }

    updateModifiers(ctrl, alt, shift);

    // Meta(Super/Win 键)修饰键同步：updateModifiers 只覆盖 Ctrl/Alt/Shift。
    // 前端把 e.metaKey 随每个 keydown 上报（keyup 消息不含修饰键字段，缺省视为
    // 释放，与 Ctrl/Alt/Shift 现有语义一致）。sendXModifier 内部按
    // portal/uinput/XTest 三路路由 —— Wayland/X11 均可注入 KEY_LEFTMETA。
    if (meta != metaDown_) {
        // 注入失败不提交记账（与 Ctrl/Alt/Shift 一致）：否则 metaDown_ 与真实
        // 修饰键状态失同步，后续 Super 组合键永久丢失（远程端"Win 失灵"）。
        if (sendXModifier(static_cast<X11KeySym>(XK_Super_L), meta))
            metaDown_ = meta;
    }

    if (code == "ControlLeft" || code == "ControlRight" ||
        code == "ShiftLeft" || code == "ShiftRight" ||
        code == "AltLeft" || code == "AltRight" ||
        code == "MetaLeft" || code == "MetaRight") {
        return;
    }

    // 非 ASCII 上屏字符：必须在上面的修饰键同步之后注入 —— isChar 消息里
    // ctrl/alt/shift 恒为 false，先跑一遍才能把远端残留的 Shift/Ctrl 抬起来，
    // 否则同一枚键会按上档层解析（实测会丢字或出错的字符）。
    if (unicodeCp != 0) {
        // 只有 XTest 通道能注入字符：内核 uinput 送的是按键，没有"送一个字符"的
        // 入口；Wayland 合成器也看不到 XWayland 键盘映射的改动。判据用"当前实际
        // 在用的通道"（uinput 已建立）而不是 waylandMode_ —— 后者会被残留的
        // wayland socket 误判，而那种机器上 XTest 才是真正在工作的路。
        if (uinputFd_ >= 0) {
            qWarning() << "InputManager: uinput 通道无法注入非 ASCII 字符, 码点:" << unicodeCp;
            return;
        }
        if (!sendUnicodeChar(unicodeCp, isDown))
            qWarning() << "InputManager: 非 ASCII 字符注入失败, 码点:" << unicodeCp;
        return;
    }

    // 单次按键注入：uinput（Wayland/锁屏）优先，否则 XTest。
    auto injectSym = [this](unsigned long ks, bool down) {
        // 首次注入时惰性打开 X display（env 已就绪）：X11 机器上把 waylandMode_
        // 改回 false，避免上面误走 uinput，并让下面 XTest 路径可用。
        ensureXDisplay();
        if (uinputFd_ >= 0 || waylandMode_) {
            // Wayland 会话（无门户授权时）优先走 uinput：无 X 也可注入
            if (uinputFd_ < 0 && !initUinput())
                qWarning() << "InputManager: uinput unavailable, falling back to XTest";
            if (uinputFd_ >= 0) {
                unsigned short lkc = keysymToLinuxKeycode(ks);
                if (lkc != 0)
                    sendUinputKey(lkc, down);
                return;
            }
        }
        if (!xDisplay_)
            return;
        focusLockScreenWindow(xdisp(xDisplay_));
        KeyCode xKeyCode = XKeysymToKeycode(xdisp(xDisplay_), static_cast<KeySym>(ks));
        if (xKeyCode != 0) {
            // 逐键 auto-repeat：抑制请求必须排在 keydown 之前（同一次 Flush 内
            // X 按请求顺序处理），恢复排在 keyup 之后。这样 X 永远不会对"正被
            // 远端按住的这颗键"产生 typematic，重复只来自客户端 repeat 翻译。
            if (down)
                setKeyAutoRepeat(static_cast<unsigned int>(xKeyCode), true);
            XTestGrabControl(xdisp(xDisplay_), True);
            XTestFakeKeyEvent(xdisp(xDisplay_), xKeyCode, down, CurrentTime);
            XTestGrabControl(xdisp(xDisplay_), False);
            XFlush(xdisp(xDisplay_));
            if (!down)
                setKeyAutoRepeat(static_cast<unsigned int>(xKeyCode), false);
        }
    };

    // 字符注入：上档字符（大写字母、!@#$ 等）按需临时按下 Shift，注入后立即抬起；
    // 不写进 shiftDown_ 持久状态，避免逐字符注入时 Shift 卡住。
    if (charShift)
        sendXModifier(static_cast<X11KeySym>(XK_Shift_L), true);
    injectSym(static_cast<unsigned long>(keySym), isDown);
    if (charShift)
        sendXModifier(static_cast<X11KeySym>(XK_Shift_L), false);
}

void InputManager::updateModifiers(bool ctrl, bool alt, bool shift) {
    // [B14] 首个按键的修饰键会被丢弃：sendXModifier 的 XTest 路径依赖 xDisplay_，
    // 旧代码在 injectSym 内才 ensureXDisplay，首个 keydown 走到这里时 display
    // 尚未打开 → 修饰键注入空转，但 ctrlDown_ 已更新（状态失同步，后续不再补发）。
    // ensureXDisplay 幂等且轻量，统一提前。
    ensureXDisplay();
    bool needFlush = false;
    if (ctrl != ctrlDown_) {
        // 注入失败不提交记账：否则下一次按键的 diff 认为 Ctrl 已在按下状态，
        // 组合键前缀永久丢失（远程端"Ctrl 失灵"）。
        if (sendXModifier(static_cast<X11KeySym>(XK_Control_L), ctrl)) {
            ctrlDown_ = ctrl;
            needFlush = true;
        }
    }
    if (alt != altDown_) {
        if (sendXModifier(static_cast<X11KeySym>(XK_Alt_L), alt)) {
            altDown_ = alt;
            needFlush = true;
        }
    }
    if (shift != shiftDown_) {
        if (sendXModifier(static_cast<X11KeySym>(XK_Shift_L), shift)) {
            shiftDown_ = shift;
            needFlush = true;
        }
    }
    if (needFlush && xDisplay_) {
        XFlush(xdisp(xDisplay_));
    }
}

// ── Wayland RemoteDesktop 门户输入 ──────────────────────────────────────────
// GNOME Wayland 通过 org.freedesktop.portal.RemoteDesktop 提供输入注入：
//   CreateSession → SelectDevices → Start（显示 consent 对话框）
//   → NotifyPointerMotionAbsolute / NotifyKeyboardKeycode
// 这是 GNOME 官方的远程桌面输入路径（rustdesk 也用类似方案）。

void InputManager::sendPortalKey(unsigned int linuxKeycode, bool isDown)
{
    if (!waylandPortalMode_ || !portalReady_ || linuxKeycode == 0)
        return;
    QDBusMessage msg = QDBusMessage::createMethodCall(
        PORTAL_SERVICE, PORTAL_PATH, PORTAL_IFACE,
        QStringLiteral("NotifyKeyboardKeycode"));
    msg << QVariant::fromValue(QDBusObjectPath(portalSessionPath_));
    msg << QVariantMap();
    msg << static_cast<int>(linuxKeycode);
    msg << (isDown ? 1u : 0u);
    QDBusConnection::sessionBus().asyncCall(msg);
}

void InputManager::initWaylandPortal()
{
    if (waylandPortalMode_)
        return;

    QDBusConnection bus = QDBusConnection::sessionBus();
    if (!bus.isConnected()) {
        qWarning() << "InputManager: Cannot connect to session D-Bus for Wayland portal";
        return;
    }

    qInfo() << "InputManager: Initializing Wayland RemoteDesktop portal...";
    waylandPortalMode_ = true;

    // 唯一 token（防重连/重启时 session/request 路径冲突）。
    // 注意：rdpserver 的 startCapture() 会重建 InputManager，两个实例可能在同一
    // 毫秒内构造——若只用 pid+ms 会生成相同 token，导致两个 CreateSession 用同一个
    // session_handle_token 而互相顶掉（SelectDevices 报 "Invalid session"）。
    // 因此叠加一个进程内递增序号，保证跨实例唯一。
    static int s_portalInitSeq = 0;
    const QString uniqueTok = QStringLiteral("qtrd_%1_%2_%3")
        .arg(QCoreApplication::applicationPid())
        .arg(s_portalInitSeq++)
        .arg(QDateTime::currentMSecsSinceEpoch());

    // portal 1.12.6 (Anolis) 的 CreateSession 检查 session_handle_token 与
    // handle_token（缺则报 "Missing token"）。
    QVariantMap opts;
    opts[QStringLiteral("handle_token")] = uniqueTok + QStringLiteral("_c");
    opts[QStringLiteral("session_handle_token")] = uniqueTok;

    // 预连接 CreateSession 的 Request.Response（基于 handle_token 预测路径，
    // 避免 portal 立即返回的定向 Response 因晚 connect 而丢失）。
    portalRequestPath_ = portalConnectResponse(uniqueTok + QStringLiteral("_c"));
    portalStage_ = 0;

    QDBusMessage msg = QDBusMessage::createMethodCall(
        PORTAL_SERVICE, PORTAL_PATH, PORTAL_IFACE,
        QStringLiteral("CreateSession"));
    msg << opts;

    QDBusPendingCall pending = bus.asyncCall(msg);
    auto *watcher = new QDBusPendingCallWatcher(pending, this);
    connect(watcher, &QDBusPendingCallWatcher::finished,
            this, &InputManager::onPortalCreateSessionReply);
}

// 基于 handle_token 预测 portal Request 对象路径并预连接 Response 信号。
// 返回预测路径（用于 onPortalXXXReply 校验）；baseService 不可用时返回空。
QString InputManager::portalConnectResponse(const QString &token)
{
    QDBusConnection bus = QDBusConnection::sessionBus();
    QString sender = bus.baseService();
    QString path;
    if (!sender.isEmpty()) {
        sender.remove(QLatin1Char(':'));
        sender.replace(QLatin1Char('.'), QLatin1Char('_'));
        path = QStringLiteral("/org/freedesktop/portal/desktop/request/%1/%2").arg(sender, token);
        bool ok = bus.connect(PORTAL_SERVICE, path, REQUEST_IFACE,
                              QStringLiteral("Response"), this,
                              SLOT(onPortalResponseSignal(QDBusMessage)));
        qInfo() << "InputManager: portalConnectResponse" << path << "connect=" << ok;
    }
    return path;
}

void InputManager::onPortalCreateSessionReply(QDBusPendingCallWatcher *call)
{
    call->deleteLater();
    QDBusPendingReply<QDBusObjectPath> reply = *call;
    if (reply.isError()) {
        qWarning() << "InputManager: Portal CreateSession failed:" << reply.error().message();
        waylandPortalMode_ = false;
        return;
    }

    // CreateSession 返回的是 Request 对象路径（非 session 路径）。
    // 真正的 session_handle 通过该 Request 的 Response 信号返回。
    if (portalRequestPath_.isEmpty()) {
        // 预连接失败（baseService 为空等），此处补连
        portalRequestPath_ = reply.value().path();
        QDBusConnection bus = QDBusConnection::sessionBus();
        bus.connect(PORTAL_SERVICE, portalRequestPath_, REQUEST_IFACE,
                    QStringLiteral("Response"), this,
                    SLOT(onPortalResponseSignal(QDBusMessage)));
    }
    qInfo() << "InputManager: Portal CreateSession request:" << portalRequestPath_;
}

void InputManager::onPortalSelectDevicesReply(QDBusPendingCallWatcher *call)
{
    call->deleteLater();
    QDBusPendingReply<QDBusObjectPath> reply = *call;
    if (reply.isError()) {
        qWarning() << "InputManager: Portal SelectDevices failed:" << reply.error().message();
        waylandPortalMode_ = false;
        return;
    }
    // SelectDevices 返回的是本次请求的 Request 对象路径，监听其 Response
    if (portalRequestPath_.isEmpty()) {
        portalRequestPath_ = reply.value().path();
        QDBusConnection bus = QDBusConnection::sessionBus();
        bus.connect(PORTAL_SERVICE, portalRequestPath_, REQUEST_IFACE,
                    QStringLiteral("Response"), this,
                    SLOT(onPortalResponseSignal(QDBusMessage)));
    }
    qInfo() << "InputManager: Portal SelectDevices request:" << portalRequestPath_;
}

void InputManager::onPortalStartReply(QDBusPendingCallWatcher *call)
{
    call->deleteLater();
    QDBusPendingReply<QDBusObjectPath> reply = *call;
    if (reply.isError()) {
        qWarning() << "InputManager: Portal Start failed:" << reply.error().message();
        waylandPortalMode_ = false;
        return;
    }
    if (portalRequestPath_.isEmpty()) {
        portalRequestPath_ = reply.value().path();
        QDBusConnection bus = QDBusConnection::sessionBus();
        bus.connect(PORTAL_SERVICE, portalRequestPath_, REQUEST_IFACE,
                    QStringLiteral("Response"), this,
                    SLOT(onPortalResponseSignal(QDBusMessage)));
    }
    qInfo() << "InputManager: Portal Start sent, waiting for user consent...";
}

void InputManager::onPortalResponseSignal(const QDBusMessage &msg)
{
    // Response(uint response_code, a{sv} results)
    //   response_code == 0 表示成功/用户同意
    if (msg.arguments().size() < 2) {
        qWarning() << "InputManager: Portal Response signal malformed";
        return;
    }
    uint code = msg.arguments().at(0).toUInt();
    QVariant resultsVar = msg.arguments().at(1);
    QVariantMap results = qdbus_cast<QVariantMap>(resultsVar);
    qInfo() << "InputManager: Portal Response results type:" << resultsVar.typeName()
            << "keys:" << results.keys();
    if (code != 0) {
        qWarning() << "InputManager: Portal consent denied (code" << code << ") stage" << portalStage_;
        waylandPortalMode_ = false;
        portalStage_ = -1;
        return;
    }

    if (portalStage_ == 0) {
        // CreateSession Response：提取 session_handle。
        // 实测该 portal 版本 results 里 session_handle 是 string 类型
        // （a{sv} 的值经 QtDBus 解包后也可能是 QDBusObjectPath），两种都兼容。
        QVariant sh = results.value(QStringLiteral("session_handle"));
        QString sessionPath;
        if (sh.userType() == qMetaTypeId<QDBusObjectPath>())
            sessionPath = sh.value<QDBusObjectPath>().path();
        else if (sh.isValid())
            sessionPath = sh.toString();
        qInfo() << "InputManager: Portal session_handle variant:" << sh.typeName()
                << "valid=" << sh.isValid() << "value=\"" << sh.toString() << "\"";
        portalSessionPath_ = sessionPath;
        qInfo() << "InputManager: Portal session established:" << portalSessionPath_;

        // Step 2: SelectDevices —— 键盘(1) + 鼠标(2) = 3
        const QString selTok = QStringLiteral("qtrd_s_%1_%2")
            .arg(QCoreApplication::applicationPid())
            .arg(QDateTime::currentMSecsSinceEpoch());
        QVariantMap selOpts;
        selOpts[QStringLiteral("handle_token")] = selTok;
        selOpts[QStringLiteral("types")] = 3u;  // KEYBOARD | POINTER
        // 预连接 SelectDevices 的 Response
        portalRequestPath_ = portalConnectResponse(selTok);
        QDBusMessage m = QDBusMessage::createMethodCall(
            PORTAL_SERVICE, PORTAL_PATH, PORTAL_IFACE, QStringLiteral("SelectDevices"));
        m << QVariant::fromValue(QDBusObjectPath(portalSessionPath_));
        m << selOpts;
        QDBusPendingCall pending = QDBusConnection::sessionBus().asyncCall(m);
        auto *watcher = new QDBusPendingCallWatcher(pending, this);
        connect(watcher, &QDBusPendingCallWatcher::finished,
                this, &InputManager::onPortalSelectDevicesReply);
        portalStage_ = 1;
    } else if (portalStage_ == 1) {
        // SelectDevices Response：发起 Start（弹出 consent 对话框）
        const QString startTok = QStringLiteral("qtrd_t_%1_%2")
            .arg(QCoreApplication::applicationPid())
            .arg(QDateTime::currentMSecsSinceEpoch());
        QVariantMap startOpts;
        startOpts[QStringLiteral("handle_token")] = startTok;
        portalRequestPath_ = portalConnectResponse(startTok);
        QDBusMessage m = QDBusMessage::createMethodCall(
            PORTAL_SERVICE, PORTAL_PATH, PORTAL_IFACE, QStringLiteral("Start"));
        m << QVariant::fromValue(QDBusObjectPath(portalSessionPath_));
        m << QString();  // parent_window
        m << startOpts;
        QDBusPendingCall pending = QDBusConnection::sessionBus().asyncCall(m);
        auto *watcher = new QDBusPendingCallWatcher(pending, this);
        connect(watcher, &QDBusPendingCallWatcher::finished,
                this, &InputManager::onPortalStartReply);
        portalStage_ = 2;
    } else if (portalStage_ == 2) {
        // Start Response：门户输入就绪
        // results["streams"] = a(ua{sv})：元素为 (stream_node_id, 属性表)，
        // 属性表含 position(ii)/size(ii)。像素坐标无法直接用于
        // NotifyPointerMotionAbsolute（要求流内 0..1），必须在这里取几何。
        QVariant streamsVar = results.value(QStringLiteral("streams"));
        if (streamsVar.isValid() && streamsVar.canConvert<QDBusArgument>()) {
            QDBusArgument arr = streamsVar.value<QDBusArgument>();
            arr.beginArray();
            while (!arr.atEnd()) {
                arr.beginStructure();
                uint nodeId = 0;
                // a{sv} 必须读成 QDBusVariant 再转 QVariantMap：QDBusArgument
                // 没有嵌套自读的 operator>>，QDBusVariant 是官方提供的嵌套承载类型。
                QDBusVariant propsV;
                arr >> nodeId;
                arr >> propsV;
                arr.endStructure();
                if (portalStreamNode_.isEmpty()) {
                    portalStreamNode_ = QString::number(nodeId);
                    const QVariantMap pm = qdbus_cast<QVariantMap>(propsV.variant());
                    const QVariant pos = pm.value(QStringLiteral("position"));
                    const QVariant size = pm.value(QStringLiteral("size"));
                    if (pos.isValid() && size.isValid()) {
                        // position/size 均为 (ii)，用 QPoint 直接承接
                        const QPoint p = qdbus_cast<QPoint>(pos);
                        const QPoint s = qdbus_cast<QPoint>(size);
                        portalStreamX_ = p.x();
                        portalStreamY_ = p.y();
                        portalStreamW_ = s.x();
                        portalStreamH_ = s.y();
                    }
                    qInfo() << "InputManager: portal stream" << portalStreamNode_
                            << "geometry" << portalStreamX_ << portalStreamY_
                            << portalStreamW_ << "x" << portalStreamH_;
                }
            }
            arr.endArray();
        }
        portalReady_ = true;
        qInfo() << "InputManager: Wayland portal READY — input injection active!";
        portalStage_ = -1;
    }
}