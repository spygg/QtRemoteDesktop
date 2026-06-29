#include "inputmanager.h"
#include <QCursor>
#include <QDebug>

#include <X11/Xlib.h>
#include <X11/Xatom.h>
#include <X11/extensions/XTest.h>
#include <X11/keysym.h>

#include <linux/uinput.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>

namespace {
    Display* xdisp(void* p) { return static_cast<Display*>(p); }

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
    if (!xDisplay_) return;
    XWarpPointer(xdisp(xDisplay_), None, DefaultRootWindow(xdisp(xDisplay_)), 0, 0, 0, 0, x, y);
    XFlush(xdisp(xDisplay_));
}

void InputManager::injectMouseButton(int x, int y, int button, bool isDown) {
    injectMouseMove(x, y);
    if (!xDisplay_) return;
    focusLockScreenWindow(xdisp(xDisplay_));
    int xButton = (button == 0 ? 1 : button == 1 ? 2 : 3);
    XTestGrabControl(xdisp(xDisplay_), True);
    XTestFakeButtonEvent(xdisp(xDisplay_), xButton, isDown, CurrentTime);
    XTestGrabControl(xdisp(xDisplay_), False);
    XFlush(xdisp(xDisplay_));
}

void InputManager::injectWheel(int delta) {
    if (!xDisplay_) return;
    int button = delta > 0 ? 4 : 5;
    XTestGrabControl(xdisp(xDisplay_), True);
    XTestFakeButtonEvent(xdisp(xDisplay_), button, True, CurrentTime);
    XTestFakeButtonEvent(xdisp(xDisplay_), button, False, CurrentTime);
    XTestGrabControl(xdisp(xDisplay_), False);
    XFlush(xdisp(xDisplay_));
}

void InputManager::focusLockScreenWindow(void* dpy)
{
    Display* display = static_cast<Display*>(dpy);

    // Debug: dump current focus and root properties
    Window curFocus = None; int revert = 0;
    XGetInputFocus(display, &curFocus, &revert);
    //qInfo() << "InputManager: current focus window" << curFocus;

    Window found = findLockScreenWindow(display);
    if (found == None) {
        // Try a simpler approach: just try the current focus window
        if (curFocus != None && curFocus != DefaultRootWindow(display)) {
            found = curFocus;
            //qInfo() << "InputManager: using current focus window as fallback";
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
    } else {
        // Fallback: try the current focus window
        if (curFocus != None && curFocus != DefaultRootWindow(display)) {
            found = curFocus;
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
    }
}

void InputManager::sendXModifier(X11KeySym ks, bool isDown) {
    if (!xDisplay_) return;
    if (uinputFd_ >= 0) {
        unsigned short lkc = keysymToLinuxKeycode(static_cast<unsigned long>(ks));
        if (lkc != 0)
            sendUinputKey(lkc, isDown);
        return;
    }
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
    return true;
}

void InputManager::destroyUinput()
{
    if (uinputFd_ < 0)
        return;
    ioctl(uinputFd_, UI_DEV_DESTROY);
    close(uinputFd_);
    uinputFd_ = -1;
    qInfo() << "InputManager: uinput device destroyed";
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
    Q_UNUSED(isChar);
    if (!xDisplay_) return;

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

    if (uinputFd_ >= 0) {
        unsigned short lkc = keysymToLinuxKeycode(static_cast<unsigned long>(keySym));
        if (lkc != 0)
            sendUinputKey(lkc, isDown);
    } else {
        focusLockScreenWindow(xdisp(xDisplay_));
        KeyCode xKeyCode = XKeysymToKeycode(xdisp(xDisplay_), keySym);
        if (xKeyCode != 0) {
            XTestGrabControl(xdisp(xDisplay_), True);
            XTestFakeKeyEvent(xdisp(xDisplay_), xKeyCode, isDown, CurrentTime);
            XTestGrabControl(xdisp(xDisplay_), False);
            XFlush(xdisp(xDisplay_));
        }
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
    if (needFlush) {
        XFlush(xdisp(xDisplay_));
    }
}
