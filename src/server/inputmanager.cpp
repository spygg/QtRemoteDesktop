#include "inputmanager.h"
#include <QCursor>
#include <QDebug>

#ifdef Q_OS_WIN
#define WIN32_LEAN_AND_MEAN
#include <Windows.h>

#define SERVICE_PIPE_NAME L"\\\\.\\pipe\\QtRemoteDesktopKeyboardSvc"
#endif

#ifdef Q_OS_LINUX
#include <X11/Xlib.h>
#include <X11/extensions/XTest.h>
#include <X11/keysym.h>
#endif

InputManager::InputManager(QObject *parent) : QObject(parent)
{
#ifdef Q_OS_LINUX
    xDisplay_ = XOpenDisplay(nullptr);
    if (!xDisplay_) {
        qCritical() << "InputManager: Failed to open X Display";
    }
#endif
}

namespace {
#ifdef Q_OS_LINUX
    Display* xdisp(void* p) { return static_cast<Display*>(p); }
#endif
}

InputManager::~InputManager()
{
#ifdef Q_OS_WIN
    disconnectKeyboardService();
#endif
#ifdef Q_OS_LINUX
    if (xDisplay_) {
        XCloseDisplay(xdisp(xDisplay_));
        xDisplay_ = nullptr;
    }
#endif
}

void InputManager::injectMouseMove(int x, int y) {
#ifdef Q_OS_WIN
    INPUT input = {};
    input.type = INPUT_MOUSE;
    input.mi.dx = x * 65535 / GetSystemMetrics(SM_CXSCREEN);
    input.mi.dy = y * 65535 / GetSystemMetrics(SM_CYSCREEN);
    input.mi.dwFlags = MOUSEEVENTF_MOVE | MOUSEEVENTF_ABSOLUTE;
    SendInput(1, &input, sizeof(INPUT));
#elif defined(Q_OS_LINUX)
    if (!xDisplay_) return;
    XWarpPointer(xdisp(xDisplay_), None, DefaultRootWindow(xdisp(xDisplay_)), 0, 0, 0, 0, x, y);
    XFlush(xdisp(xDisplay_));
#else
    QCursor::setPos(x, y);
#endif
}

void InputManager::injectMouseButton(int x, int y, int button, bool isDown) {
    injectMouseMove(x, y);

#ifdef Q_OS_WIN
    DWORD flags = 0;
    switch (button) {
        case 0: flags = isDown ? MOUSEEVENTF_LEFTDOWN   : MOUSEEVENTF_LEFTUP;   break;
        case 1: flags = isDown ? MOUSEEVENTF_MIDDLEDOWN : MOUSEEVENTF_MIDDLEUP; break;
        case 2: flags = isDown ? MOUSEEVENTF_RIGHTDOWN  : MOUSEEVENTF_RIGHTUP;  break;
        default: return;
    }
    INPUT input = {};
    input.type = INPUT_MOUSE;
    input.mi.dwFlags = flags;
    SendInput(1, &input, sizeof(INPUT));
#elif defined(Q_OS_LINUX)
    if (!xDisplay_) return;
    int xButton = (button == 0 ? 1 : button == 1 ? 2 : 3);
    XTestFakeButtonEvent(xdisp(xDisplay_), xButton, isDown, CurrentTime);
    XFlush(xdisp(xDisplay_));
#endif
}

void InputManager::injectWheel(int delta) {
#ifdef Q_OS_WIN
    INPUT input = {};
    input.type = INPUT_MOUSE;
    input.mi.mouseData = delta * WHEEL_DELTA;
    input.mi.dwFlags = MOUSEEVENTF_WHEEL;
    SendInput(1, &input, sizeof(INPUT));
#elif defined(Q_OS_LINUX)
    if (!xDisplay_) return;
    int button = delta > 0 ? 4 : 5;
    XTestFakeButtonEvent(xdisp(xDisplay_), button, True, CurrentTime);
    XTestFakeButtonEvent(xdisp(xDisplay_), button, False, CurrentTime);
    XFlush(xdisp(xDisplay_));
#endif
}

#ifdef Q_OS_WIN
void InputManager::sendModifierEvent(int vk, bool isDown) {
    INPUT input = {};
    input.type = INPUT_KEYBOARD;
    input.ki.wVk = vk;
    if (!isDown) {
        input.ki.dwFlags = KEYEVENTF_KEYUP;
    }
    SendInput(1, &input, sizeof(INPUT));
}

bool InputManager::sendToService(BYTE type, const BYTE* data, DWORD dataLen) {
    if (servicePipe_ == INVALID_HANDLE_VALUE) return false;

    BYTE buffer[16];
    if (1 + dataLen > sizeof(buffer)) return false;
    buffer[0] = type;
    if (dataLen > 0) memcpy(buffer + 1, data, dataLen);

    DWORD written = 0;
    BOOL ok = WriteFile(servicePipe_, buffer, 1 + dataLen, &written, NULL);
    return ok && written == 1 + dataLen;
}

bool InputManager::connectKeyboardService() {
    if (servicePipe_ != INVALID_HANDLE_VALUE) return true;

    // Wait up to 3 seconds for the service to be ready
    if (!WaitNamedPipeW(SERVICE_PIPE_NAME, 3000)) {
        qWarning() << "InputManager: Keyboard service not available";
        return false;
    }

    servicePipe_ = CreateFileW(
        SERVICE_PIPE_NAME,
        GENERIC_WRITE,
        0, NULL,
        OPEN_EXISTING,
        0, NULL
    );

    if (servicePipe_ == INVALID_HANDLE_VALUE) {
        qWarning() << "InputManager: Failed to connect to keyboard service, error:"
                   << GetLastError();
        return false;
    }

    qInfo() << "InputManager: Connected to keyboard service";
    return true;
}

void InputManager::disconnectKeyboardService() {
    if (servicePipe_ != INVALID_HANDLE_VALUE) {
        CloseHandle(servicePipe_);
        servicePipe_ = INVALID_HANDLE_VALUE;
        qInfo() << "InputManager: Disconnected from keyboard service";
    }
}
#endif

#ifdef Q_OS_LINUX
void InputManager::sendXModifier(X11KeySym ks, bool isDown) {
    if (!xDisplay_) return;
    KeyCode kc = XKeysymToKeycode(xdisp(xDisplay_), static_cast<KeySym>(ks));
    if (kc != 0) {
        XTestFakeKeyEvent(xdisp(xDisplay_), kc, isDown, CurrentTime);
    }
}
#endif

void InputManager::injectKeyboard(int keycode, const QString& code, bool isDown, bool ctrl, bool alt, bool shift) {
#ifdef Q_OS_WIN
    // ===== Service path (SYSTEM-level keyboard injector) =====
    if (servicePipe_ != INVALID_HANDLE_VALUE) {
        // Character-producing keys → send via Unicode (works on secure desktop)
        int vkForUnicode = (keycode == VK_RETURN) ? VK_RETURN :
                           (keycode == VK_BACK) ? VK_BACK :
                           (keycode >= 0x20 && keycode <= 0xFE) ? keycode : 0;
        if (isDown && vkForUnicode) {
            BYTE kbdState[256] = {0};
            if (shift) kbdState[VK_SHIFT] = 0x80;
            if (ctrl)  kbdState[VK_CONTROL] = 0x80;
            if (alt)   kbdState[VK_MENU] = 0x80;

            wchar_t chars[4] = {0};
            int ret = ToUnicode(static_cast<UINT>(keycode), 0, kbdState, chars, 4, 1);
            if (ret >= 1 && chars[0] > 0x07) {
                sendToService(0x02, reinterpret_cast<const BYTE*>(chars), 2);
                return;
            }
        }
        // Non-char or keyup → VK event
        BYTE vkBuf[5] = {0};
        *reinterpret_cast<UINT*>(vkBuf) = static_cast<UINT>(keycode);
        vkBuf[4] = isDown ? 1 : 0;
        sendToService(0x01, vkBuf, 5);
        return;
    }

    // ===== Normal path (SendInput directly) =====
    // 安全桌面: SendInput 被 UIPI 阻止, 优先用 Unicode 尝试
    int vkForUnicode = (keycode == VK_RETURN) ? VK_RETURN :
                       (keycode == VK_BACK) ? VK_BACK :
                       (keycode >= 0x20 && keycode <= 0xFE) ? keycode : 0;
    if (isDown && vkForUnicode) {
        BYTE keyboardState[256] = {0};
        if (shift) keyboardState[VK_SHIFT] = 0x80;
        if (ctrl)  keyboardState[VK_CONTROL] = 0x80;
        if (alt)   keyboardState[VK_MENU] = 0x80;

        wchar_t chars[4] = {0};
        int ret = ToUnicode(static_cast<UINT>(keycode), 0, keyboardState, chars, 4, 1);
        if (ret >= 1 && chars[0] > 0x07) {
            INPUT inpDown = {};
            inpDown.type = INPUT_KEYBOARD;
            inpDown.ki.dwFlags = KEYEVENTF_UNICODE;
            inpDown.ki.wScan = chars[0];
            SendInput(1, &inpDown, sizeof(INPUT));

            INPUT inpUp = {};
            inpUp.type = INPUT_KEYBOARD;
            inpUp.ki.dwFlags = KEYEVENTF_UNICODE | KEYEVENTF_KEYUP;
            inpUp.ki.wScan = chars[0];
            SendInput(1, &inpUp, sizeof(INPUT));
            return;
        }
    }

    updateModifiers(ctrl, alt, shift);

    INPUT input = {};
    input.type = INPUT_KEYBOARD;
    input.ki.wVk = keycode;
    if (!isDown) {
        input.ki.dwFlags = KEYEVENTF_KEYUP;
    }
    SendInput(1, &input, sizeof(INPUT));
#elif defined(Q_OS_LINUX)
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

    KeyCode xKeyCode = XKeysymToKeycode(xdisp(xDisplay_), keySym);
    if (xKeyCode != 0) {
        XTestFakeKeyEvent(xdisp(xDisplay_), xKeyCode, isDown, CurrentTime);
        XFlush(xdisp(xDisplay_));
    }
#endif
}

void InputManager::updateModifiers(bool ctrl, bool alt, bool shift) {
#ifdef Q_OS_WIN
    if (ctrl != ctrlDown_) {
        sendModifierEvent(VK_CONTROL, ctrl);
        ctrlDown_ = ctrl;
    }
    if (alt != altDown_) {
        sendModifierEvent(VK_MENU, alt);
        altDown_ = alt;
    }
    if (shift != shiftDown_) {
        sendModifierEvent(VK_SHIFT, shift);
        shiftDown_ = shift;
    }
#elif defined(Q_OS_LINUX)
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
#endif
}
