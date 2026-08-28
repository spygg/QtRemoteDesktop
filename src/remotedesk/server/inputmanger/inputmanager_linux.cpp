#include "inputmanager.h"
#include <QCursor>
#include <QDebug>
#include <QDateTime>
#include <QGuiApplication>
#include <QScreen>

#include <X11/Xlib.h>
#include <X11/Xatom.h>
#include <X11/extensions/XTest.h>
#include <X11/keysym.h>

#include <linux/uinput.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>

#include <QDBusConnection>
#include <QDBusMessage>
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
                bool skip = (atoms[0] == wmTypeDesktop || atoms[0] == wmTypeDock);
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
}

void InputManager::injectMouseMove(int x, int y) {
    // Wayland 门户模式：通过 RemoteDesktop D-Bus 注入
    if (waylandPortalMode_ && portalReady_) {
        // NotifyPointerMotionAbsolute(session, options, stream, x, y)
        // stream=0 表示使用默认流
        QDBusMessage msg = QDBusMessage::createMethodCall(
            PORTAL_SERVICE, PORTAL_PATH, PORTAL_IFACE,
            QStringLiteral("NotifyPointerMotionAbsolute"));
        msg << QVariant::fromValue(QDBusObjectPath(portalSessionPath_));
        msg << QVariantMap();
        msg << 0u;  // stream node id (0 = default)
        msg << static_cast<double>(x);
        msg << static_cast<double>(y);
        QDBusConnection::sessionBus().asyncCall(msg);
        return;
    }
    // Wayland 下无 X：走 uinput 绝对定位
    if (uinputMouseFd_ < 0 && waylandMode_)
        initUinputMouse();
    if (uinputMouseFd_ >= 0) {
        sendUinputMouseMove(x, y);
        return;
    }
    if (!xDisplay_) return;
    XWarpPointer(xdisp(xDisplay_), None, DefaultRootWindow(xdisp(xDisplay_)), 0, 0, 0, 0, x, y);
    XFlush(xdisp(xDisplay_));
}

QPoint InputManager::cursorPosition() const
{
    int x = -1, y = -1;
    if (queryPointer(xDisplay_, &x, &y) && x >= 0 && y >= 0)
        return QPoint(x, y);
    return QCursor::pos();
}

void InputManager::injectMouseButton(int x, int y, int button, bool isDown) {
    // Wayland 门户模式
    if (waylandPortalMode_ && portalReady_) {
        injectMouseMove(x, y);
        // NotifyPointerButton(session, options, button_code, state)
        // button_code: Linux evdev button codes (0x110=LEFT, 0x111=RIGHT, 0x112=MIDDLE)
        uint btnCode = (button == 0) ? 0x110 : (button == 1) ? 0x111 : 0x112;
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
    int xButton = (button == 0 ? 1 : button == 1 ? 2 : 3);
    XTestGrabControl(xdisp(xDisplay_), True);
    XTestFakeButtonEvent(xdisp(xDisplay_), xButton, isDown, CurrentTime);
    XTestGrabControl(xdisp(xDisplay_), False);
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
    if (uinputWheelFd_ < 0 && waylandMode_)
        initUinputMouse();
    if (uinputWheelFd_ >= 0) {
        sendUinputWheel(delta);
        return;
    }
    if (!xDisplay_) return;
    int button = delta > 0 ? 4 : 5;
    XTestGrabControl(xdisp(xDisplay_), True);
    XTestFakeButtonEvent(xdisp(xDisplay_), button, True, CurrentTime);
    XTestFakeButtonEvent(xdisp(xDisplay_), button, False, CurrentTime);
    XTestGrabControl(xdisp(xDisplay_), False);
    XFlush(xdisp(xDisplay_));
}

void InputManager::primeFocusWindow() {
    if (xDisplay_)
        focusLockScreenWindow(xdisp(xDisplay_));
}

void InputManager::focusLockScreenWindow(void* dpy)
{
    Display* display = static_cast<Display*>(dpy);

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

void InputManager::sendXModifier(X11KeySym ks, bool isDown) {
    // Wayland 门户优先
    if (waylandPortalMode_ && portalReady_) {
        unsigned short lkc = keysymToLinuxKeycode(static_cast<unsigned long>(ks));
        if (lkc != 0)
            sendPortalKey(lkc, isDown);
        return;
    }
    // uinput 优先（Wayland 会话无门户时也自动初始化）
    if (uinputFd_ >= 0 || waylandMode_) {
        if (uinputFd_ < 0 && !initUinput())
            return;
        unsigned short lkc = keysymToLinuxKeycode(static_cast<unsigned long>(ks));
        if (lkc != 0)
            sendUinputKey(lkc, isDown);
        return;
    }
    if (!xDisplay_) return;
    KeyCode kc = XKeysymToKeycode(xdisp(xDisplay_), static_cast<KeySym>(ks));
    if (kc != 0) {
        focusLockScreenWindow(xdisp(xDisplay_));
        XTestGrabControl(xdisp(xDisplay_), True);
        XTestFakeKeyEvent(xdisp(xDisplay_), kc, isDown, CurrentTime);
        XTestGrabControl(xdisp(xDisplay_), False);
    }
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

    for (int i = 1; i <= 126; i++)
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
    qInfo() << "InputManager: uinput device created";

    // 键盘设备就绪后，一并创建鼠标（绝对定位）与滚轮设备（Wayland 需要）
    initUinputMouse();
    return true;
}

bool InputManager::initUinputMouse()
{
    if (uinputMouseFd_ >= 0 && uinputWheelFd_ >= 0)
        return true;

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
        qWarning() << "InputManager: pointer UI_DEV_SETUP failed";
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

    // ---- 滚轮设备（EV_REL，独立设备避免与绝对定位冲突）----
    int wfd = open("/dev/uinput", O_WRONLY | O_NONBLOCK);
    if (wfd < 0)
        wfd = open("/dev/input/uinput", O_WRONLY | O_NONBLOCK);
    if (wfd >= 0) {
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
            wfd = -1;
        }
        uinputWheelFd_ = wfd;
    }

    qInfo() << "InputManager: uinput mouse devices created (pointer" << uinputMouseFd_
            << "wheel" << uinputWheelFd_ << ")";
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
    int ax = sw > 0 ? qBound(0, x * 65535 / sw, 65535) : 0;
    int ay = sh > 0 ? qBound(0, y * 65535 / sh, 65535) : 0;

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
    unsigned short code = (button == 0) ? BTN_LEFT
                        : (button == 1) ? BTN_RIGHT : BTN_MIDDLE;
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
    struct input_event ev = {};
    ev.type = EV_REL; ev.code = REL_WHEEL; ev.value = delta > 0 ? 1 : -1;
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

void InputManager::injectKeyboard(int keycode, const QString& code, bool isDown, bool ctrl, bool alt, bool shift, bool useVkFallback, bool isChar) {
    Q_UNUSED(useVkFallback);

    // Wayland 门户模式：通过 D-Bus 注入键盘
    if (waylandPortalMode_ && portalReady_) {
        // 先同步修饰键状态（Ctrl/Alt/Shift），保证组合键（Ctrl+C 等）生效
        updateModifiers(ctrl, alt, shift);
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
    Q_UNUSED(code);
    Q_UNUSED(isChar);

    KeySym keySym = NoSymbol;

    if (code == "Delete")      keySym = XK_Delete;
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

    if (code == "ControlLeft" || code == "ControlRight" ||
        code == "ShiftLeft" || code == "ShiftRight" ||
        code == "AltLeft" || code == "AltRight" ||
        code == "MetaLeft" || code == "MetaRight") {
        return;
    }

    if (uinputFd_ >= 0 || waylandMode_) {
        // Wayland 会话（无门户授权时）优先走 uinput：无 X 也可注入
        if (uinputFd_ < 0 && !initUinput())
            qWarning() << "InputManager: uinput unavailable, falling back to XTest";
        if (uinputFd_ >= 0) {
            unsigned short lkc = keysymToLinuxKeycode(static_cast<unsigned long>(keySym));
            if (lkc != 0)
                sendUinputKey(lkc, isDown);
            return;
        }
    }
    if (!xDisplay_)
        return;
    focusLockScreenWindow(xdisp(xDisplay_));
    KeyCode xKeyCode = XKeysymToKeycode(xdisp(xDisplay_), keySym);
    if (xKeyCode != 0) {
        XTestGrabControl(xdisp(xDisplay_), True);
        XTestFakeKeyEvent(xdisp(xDisplay_), xKeyCode, isDown, CurrentTime);
        XTestGrabControl(xdisp(xDisplay_), False);
        XFlush(xdisp(xDisplay_));
    }
}

void InputManager::updateModifiers(bool ctrl, bool alt, bool shift) {
    bool needFlush = false;
    if (ctrl != ctrlDown_) {
        sendXModifier(static_cast<X11KeySym>(XK_Control_L), ctrl);
        ctrlDown_ = ctrl;
        needFlush = true;
    }
    if (alt != altDown_) {
        sendXModifier(static_cast<X11KeySym>(XK_Alt_L), alt);
        altDown_ = alt;
        needFlush = true;
    }
    if (shift != shiftDown_) {
        sendXModifier(static_cast<X11KeySym>(XK_Shift_L), shift);
        shiftDown_ = shift;
        needFlush = true;
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
        portalReady_ = true;
        qInfo() << "InputManager: Wayland portal READY — input injection active!";
        if (results.contains(QStringLiteral("streams")))
            qInfo() << "InputManager: Portal streams available";
        portalStage_ = -1;
    }
}