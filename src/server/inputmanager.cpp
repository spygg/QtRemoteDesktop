// server/input_manager.cpp
#include "inputmanager.h"
#include <QCursor>
#include <QDebug>

#ifdef Q_OS_WIN
#include <Windows.h>
#endif

#ifdef Q_OS_LINUX
#include <X11/Xlib.h>
#include <X11/extensions/XTest.h>
#include <X11/keysym.h>
#endif

InputManager::InputManager(QObject *parent) : QObject(parent) {}

void InputManager::injectMouseMove(int x, int y) {
#ifdef Q_OS_WIN
    INPUT input = {};
    input.type = INPUT_MOUSE;
    input.mi.dx = x * 65535 / GetSystemMetrics(SM_CXSCREEN);
    input.mi.dy = y * 65535 / GetSystemMetrics(SM_CYSCREEN);
    input.mi.dwFlags = MOUSEEVENTF_MOVE | MOUSEEVENTF_ABSOLUTE;
    SendInput(1, &input, sizeof(INPUT));
#elif defined(Q_OS_LINUX)
    Display* display = XOpenDisplay(nullptr);
    XWarpPointer(display, None, DefaultRootWindow(display), 0, 0, 0, 0, x, y);
    XFlush(display);
    XCloseDisplay(display);
#else
    QCursor::setPos(x, y);
#endif
}

void InputManager::injectMouseButton(int x, int y, int button, bool isDown) {
    // 先移动到位
    injectMouseMove(x, y);

#ifdef Q_OS_WIN
    INPUT input = {};
    input.type = INPUT_MOUSE;
    input.mi.dwFlags = (button == 0 ? (isDown ? MOUSEEVENTF_LEFTDOWN : MOUSEEVENTF_LEFTUP) :
                            button == 1 ? (isDown ? MOUSEEVENTF_RIGHTDOWN : MOUSEEVENTF_RIGHTUP) :
                            button == 2 ? (isDown ? MOUSEEVENTF_MIDDLEDOWN : MOUSEEVENTF_MIDDLEUP) :
                            0);
    SendInput(1, &input, sizeof(INPUT));
#elif defined(Q_OS_LINUX)
    Display* display = XOpenDisplay(nullptr);
    int xButton = (button == 0 ? 1 : button == 1 ? 3 : 2);
    XTestFakeButtonEvent(display, xButton, isDown, CurrentTime);
    XFlush(display);
    XCloseDisplay(display);
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
    Display* display = XOpenDisplay(nullptr);
    int button = delta > 0 ? 4 : 5;
    XTestFakeButtonEvent(display, button, True, CurrentTime);
    XTestFakeButtonEvent(display, button, False, CurrentTime);
    XFlush(display);
    XCloseDisplay(display);
#endif
}

void InputManager::injectKeyboard(int keycode, const QString& code, bool isDown, bool ctrl, bool alt, bool shift) {
    updateModifiers(ctrl, alt, shift);

#ifdef Q_OS_WIN
    // 转换 JS keycode 到 Windows VK
    int vk = keycode;

    INPUT input = {};
    input.type = INPUT_KEYBOARD;
    input.ki.wVk = vk;
    input.ki.dwFlags = isDown ? 0 : KEYEVENTF_KEYUP;
    SendInput(1, &input, sizeof(INPUT));
#elif defined(Q_OS_LINUX)
    Display* display = XOpenDisplay(nullptr);

    // 将 code 字符串转换为 KeySym
    KeySym keySym = NoSymbol;

    // 常用按键映射
    if (code == "Delete") keySym = XK_Delete;
    else if (code == "Backspace") keySym = XK_BackSpace;
    else if (code == "Enter") keySym = XK_Return;
    else if (code == "Tab") keySym = XK_Tab;
    else if (code == "Escape") keySym = XK_Escape;
    else if (code == "Space") keySym = XK_space;
    else if (code == "ArrowUp") keySym = XK_Up;
    else if (code == "ArrowDown") keySym = XK_Down;
    else if (code == "ArrowLeft") keySym = XK_Left;
    else if (code == "ArrowRight") keySym = XK_Right;
    else if (code == "Home") keySym = XK_Home;
    else if (code == "End") keySym = XK_End;
    else if (code == "PageUp") keySym = XK_Page_Up;
    else if (code == "PageDown") keySym = XK_Page_Down;
    else if (code == "Insert") keySym = XK_Insert;
    else if (code.startsWith("Key")) {
        // 字母键 A-Z
        if (code.length() == 4) {
            QChar c = code[3];
            QByteArray str = QByteArray(1, c.toLower().toLatin1());
            keySym = XStringToKeysym(str.constData());
        }
    } else if (code.startsWith("Digit")) {
        // 数字键 0-9
        if (code.length() == 6) {
            QChar c = code[5];
            QByteArray str = QByteArray(1, c.toLatin1());
            keySym = XStringToKeysym(str.constData());
        }
    } else {
        // 使用 keycode 作为后备
        keySym = keycode;
    }

    KeyCode xKeyCode = XKeysymToKeycode(display, keySym);
    if (xKeyCode != 0) {
        XTestFakeKeyEvent(display, xKeyCode, isDown, CurrentTime);
        XFlush(display);
    }

    XCloseDisplay(display);
#endif
}

void InputManager::updateModifiers(bool ctrl, bool alt, bool shift) {
#ifdef Q_OS_WIN
    // 使用 GetAsyncKeyState 检查当前状态，按需发送事件
    // 简化处理：直接发送修饰键事件
    if (ctrl) {
        INPUT input = {};
        input.type = INPUT_KEYBOARD;
        input.ki.wVk = VK_CONTROL;
        SendInput(1, &input, sizeof(INPUT));
    }
#endif
}
