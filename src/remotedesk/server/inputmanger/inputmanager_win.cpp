#include "inputmanager.h"
#include <QDebug>
#include <QCursor>

#define WIN32_LEAN_AND_MEAN
#include <Windows.h>

QPoint InputManager::cursorPosition() const
{
    // QCursor::pos() 是虚拟桌面绝对坐标；统一换算成"捕获区相对坐标"，
    // 与 X11/Wayland 实现保持一致（调用方不再二次减偏移）。
    // 多屏时副屏在主屏左/上会让绝对值/相对值为负，这是合法值。
    return QCursor::pos() - QPoint(screenOffsetX_, screenOffsetY_);
}

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
    // 高 DPI 缩放下 GetSystemMetrics 返回物理像素，前端坐标基于逻辑像素，
    // 若未设置 screenW_/screenH_（与前端一致），回退物理尺寸。
    int sw = screenW_ > 0 ? screenW_ : GetSystemMetrics(SM_CXSCREEN);
    int sh = screenH_ > 0 ? screenH_ : GetSystemMetrics(SM_CYSCREEN);
    // 无头/RDP 边缘会话下 SM_CXSCREEN 可能为 0，裸除会直接整除零崩溃进程；
    // 分母兜底为 1（坐标会被系统钳到边界，但至少不崩）。
    if (sw <= 0 || sh <= 0) {
        static bool logged = false;
        if (!logged) {
            logged = true;
            qWarning() << "injectMouseMove: screen size unavailable ("
                       << sw << sh << "), clamping denominator to 1";
        }
        sw = qMax(1, sw);
        sh = qMax(1, sh);
    }
    input.mi.dx = x * 65535 / sw;
    input.mi.dy = y * 65535 / sh;
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

void InputManager::injectKeyboard(int keycode, const QString& code, bool isDown, bool ctrl, bool alt, bool shift, bool useVkFallback, bool isChar, bool meta)
{
    // 优先处理 isChar 模式：直接发送 Unicode 字符（中文输入法提交的最终字符）
    if (isChar) {
        // keydown 已按 KEYEVENTF_UNICODE 成对发送 down+up，不存在"按住"状态；
        // 对应的 keyup 若继续走下面的 VK 分支，会把 Unicode 码点当虚拟键码再发
        // 一次 keyup（如 'A'=65 → VK 'A'），可能错误释放用户正按住的键。
        if (!isDown || keycode <= 0)
            return;
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

    // Meta(Win 键)修饰键同步：与 Linux 端同语义，前端 e.metaKey 随 keydown
    // 上报、keyup 缺省视为释放。VK_LWIN 0x5B / VK_RWIN 0x5C。
    if (meta != metaDown_) {
        sendModifierEvent(VK_LWIN, meta);
        metaDown_ = meta;
    }

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
