#include "inputmanager.h"
#include <QDebug>

#define WIN32_LEAN_AND_MEAN
#include <Windows.h>

#define SERVICE_PIPE_NAME L"\\\\.\\pipe\\QtRemoteDesktopKeyboardSvc"

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
    if (!isDown) {
        input.ki.dwFlags = KEYEVENTF_KEYUP;
    }
    sendInputChecked(1, &input, sizeof(INPUT));
}

bool InputManager::sendToService(BYTE type, const BYTE* data, DWORD dataLen)
{
    if (servicePipe_ == INVALID_HANDLE_VALUE)
        return false;

    BYTE buffer[16];
    if (1 + dataLen > sizeof(buffer))
        return false;
    buffer[0] = type;
    if (dataLen > 0)
        memcpy(buffer + 1, data, dataLen);

    DWORD written = 0;
    BOOL ok = WriteFile(servicePipe_, buffer, 1 + dataLen, &written, NULL);
    return ok && written == 1 + dataLen;
}

bool InputManager::connectKeyboardService()
{
    if (servicePipe_ != INVALID_HANDLE_VALUE)
        return true;

    if (!WaitNamedPipeW(SERVICE_PIPE_NAME, 3000)) {
        // qWarning() << "InputManager: Keyboard service not available";
        return false;
    }

    servicePipe_ = CreateFileW(
        SERVICE_PIPE_NAME,
        GENERIC_WRITE,
        0, NULL,
        OPEN_EXISTING,
        0, NULL);

    if (servicePipe_ == INVALID_HANDLE_VALUE) {
        // qWarning() << "InputManager: Failed to connect to keyboard service, error:"
        //            << GetLastError();
        return false;
    }

    qInfo() << "InputManager: Connected to keyboard service";
    return true;
}

void InputManager::disconnectKeyboardService()
{
    if (servicePipe_ != INVALID_HANDLE_VALUE) {
        CloseHandle(servicePipe_);
        servicePipe_ = INVALID_HANDLE_VALUE;
        // qInfo() << "InputManager: Disconnected from keyboard service";
    }
}

void InputManager::injectKeyboard(int keycode, const QString& code, bool isDown, bool ctrl, bool alt, bool shift)
{
    bool isCharKey = code.startsWith("Key") || code.startsWith("Digit")
        || code == "Space" || code == "Enter" || code == "Tab"
        || code == "Comma" || code == "Period" || code == "Semicolon" || code == "Quote"
        || code == "BracketLeft" || code == "BracketRight" || code == "Backslash"
        || code == "Minus" || code == "Equal" || code == "Backquote" || code == "Slash";

    // ===== Service connected → forward ALL keys to secure desktop helper =====
    if (servicePipe_ != INVALID_HANDLE_VALUE) {
        if (isDown && isCharKey) {
            BYTE kbdState[256] = { 0 };
            if (shift)
                kbdState[VK_SHIFT] = 0x80;
            if (ctrl)
                kbdState[VK_CONTROL] = 0x80;
            if (alt)
                kbdState[VK_MENU] = 0x80;

            wchar_t chars[4] = { 0 };
            int ret = ToUnicode(static_cast<UINT>(keycode), 0, kbdState, chars, 4, 1);
            if (ret >= 1 && chars[0] > 0x07) {
                sendToService(0x02, reinterpret_cast<const BYTE*>(chars), 2);
                return;
            }
        }
        BYTE vkBuf[5] = { 0 };
        *reinterpret_cast<UINT*>(vkBuf) = static_cast<UINT>(keycode);
        vkBuf[4] = isDown ? 1 : 0;
        sendToService(0x01, vkBuf, 5);
        return;
    }

    // ===== Service NOT connected → SendInput directly =====
    if (isDown && isCharKey) {
        BYTE keyboardState[256] = { 0 };
        if (shift)
            keyboardState[VK_SHIFT] = 0x80;
        if (ctrl)
            keyboardState[VK_CONTROL] = 0x80;
        if (alt)
            keyboardState[VK_MENU] = 0x80;

        wchar_t chars[4] = { 0 };
        int ret = ToUnicode(static_cast<UINT>(keycode), 0, keyboardState, chars, 4, 1);
        if (ret >= 1 && chars[0] > 0x07) {
            INPUT inpDown = {};
            inpDown.type = INPUT_KEYBOARD;
            inpDown.ki.dwFlags = KEYEVENTF_UNICODE;
            inpDown.ki.wScan = chars[0];
            sendInputChecked(1, &inpDown, sizeof(INPUT));

            INPUT inpUp = {};
            inpUp.type = INPUT_KEYBOARD;
            inpUp.ki.dwFlags = KEYEVENTF_UNICODE | KEYEVENTF_KEYUP;
            inpUp.ki.wScan = chars[0];
            sendInputChecked(1, &inpUp, sizeof(INPUT));
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
