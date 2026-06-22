#include "inputmanager.h"
#include <QDebug>

#define WIN32_LEAN_AND_MEAN
#include <Windows.h>

static bool sendInputChecked(UINT count, LPINPUT inputs, int cbSize)
{
    UINT sent = SendInput(count, inputs, cbSize);
    if (sent != count)
        qWarning() << "SendInput: sent" << sent << "of" << count << "events, error:" << GetLastError();
    return sent == count;
}

void InputManager::injectMouseMove(int x, int y)
{
    INPUT input = {};
    input.type = INPUT_MOUSE;
    input.mi.dx = x * 65535 / GetSystemMetrics(SM_CXSCREEN);
    input.mi.dy = y * 65535 / GetSystemMetrics(SM_CYSCREEN);
    input.mi.dwFlags = MOUSEEVENTF_MOVE | MOUSEEVENTF_ABSOLUTE;
    sendInputChecked(1, &input, sizeof(INPUT));
}

void InputManager::injectMouseButton(int x, int y, int button, bool isDown)
{
    injectMouseMove(x, y);

    DWORD flags = 0;
    switch (button) {
    case 0:
        flags = isDown ? MOUSEEVENTF_LEFTDOWN : MOUSEEVENTF_LEFTUP;
        break;
    case 1:
        flags = isDown ? MOUSEEVENTF_MIDDLEDOWN : MOUSEEVENTF_MIDDLEUP;
        break;
    case 2:
        flags = isDown ? MOUSEEVENTF_RIGHTDOWN : MOUSEEVENTF_RIGHTUP;
        break;
    default:
        return;
    }
    INPUT input = {};
    input.type = INPUT_MOUSE;
    input.mi.dwFlags = flags;
    sendInputChecked(1, &input, sizeof(INPUT));
}

void InputManager::injectWheel(int delta)
{
    INPUT input = {};
    input.type = INPUT_MOUSE;
    input.mi.mouseData = delta * WHEEL_DELTA;
    input.mi.dwFlags = MOUSEEVENTF_WHEEL;
    sendInputChecked(1, &input, sizeof(INPUT));
}

void InputManager::sendModifierEvent(int vk, bool isDown)
{
    INPUT input = {};
    input.type = INPUT_KEYBOARD;
    input.ki.wVk = vk;
    if (!isDown)
        input.ki.dwFlags = KEYEVENTF_KEYUP;
    sendInputChecked(1, &input, sizeof(INPUT));
}

void InputManager::injectKeyboard(int keycode, const QString& code, bool isDown, bool ctrl, bool alt, bool shift, bool useVkFallback, bool isChar)
{
    // 优先处理 isChar 模式：直接发送 Unicode 字符（中文输入法提交的最终字符）
    if (isChar && isDown && keycode > 0) {
        wchar_t ch = static_cast<wchar_t>(keycode);
        INPUT inpDown = {};
        inpDown.type = INPUT_KEYBOARD;
        inpDown.ki.dwFlags = KEYEVENTF_UNICODE;
        inpDown.ki.wScan = ch;
        sendInputChecked(1, &inpDown, sizeof(INPUT));

        INPUT inpUp = {};
        inpUp.type = INPUT_KEYBOARD;
        inpUp.ki.dwFlags = KEYEVENTF_UNICODE | KEYEVENTF_KEYUP;
        inpUp.ki.wScan = ch;
        sendInputChecked(1, &inpUp, sizeof(INPUT));
        return;
    }

    updateModifiers(ctrl, alt, shift);

    INPUT input = {};
    input.type = INPUT_KEYBOARD;
    input.ki.wVk = keycode;
    if (!isDown)
        input.ki.dwFlags = KEYEVENTF_KEYUP;
    sendInputChecked(1, &input, sizeof(INPUT));
}

void InputManager::updateModifiers(bool ctrl, bool alt, bool shift)
{
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
}
