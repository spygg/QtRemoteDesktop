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
    // 前端发来的是「捕获区相对像素」。MOUSEEVENTF_ABSOLUTE 不带 VIRTUALDESK 时按
    // **主屏**归一化：捕获区是副屏（或主屏不在虚拟桌面原点）时整段坐标偏移，
    // 点哪都差一个屏幕。这里统一换成虚拟桌面坐标：
    //     绝对位置 = 捕获区原点(screenOffsetX_/Y_) + 相对坐标
    // 再用虚拟桌面宽高归一化。单屏时虚拟桌面 ≡ 主屏、原点 ≡ (0,0)，与旧实现
    // 只差"除数用跨度-1"的亚像素修正（65535 对应最后一个像素）。
    // 用系统实时尺寸而不是 setScreenSize 的逻辑尺寸：屏幕变了哪一侧没同步上，
    // 映射都不能跟着错（这也是外部改分辨率后点击错位的一条来源）。
    const int vx = GetSystemMetrics(SM_XVIRTUALSCREEN);
    const int vy = GetSystemMetrics(SM_YVIRTUALSCREEN);
    int vw = GetSystemMetrics(SM_CXVIRTUALSCREEN);
    int vh = GetSystemMetrics(SM_CYVIRTUALSCREEN);
    // 无头/RDP 边缘会话下虚拟桌面尺寸可能为 0，裸除会直接崩溃进程
    if (vw <= 0 || vh <= 0) {
        static bool logged = false;
        if (!logged) {
            logged = true;
            qWarning() << "injectMouseMove: virtual desktop size unavailable ("
                       << vw << vh << "), clamping denominator to 1";
        }
        vw = qMax(1, vw);
        vh = qMax(1, vh);
    }
    // 65535 对应虚拟桌面最后一个像素，所以除数是跨度-1，不是跨度
    const int spanX = qMax(1, vw - 1);
    const int spanY = qMax(1, vh - 1);
    const int absX = qBound(0, x + screenOffsetX_ - vx, spanX);
    const int absY = qBound(0, y + screenOffsetY_ - vy, spanY);
    input.mi.dx = static_cast<LONG>(absX * 65535 / spanX);
    input.mi.dy = static_cast<LONG>(absY * 65535 / spanY);
    input.mi.dwFlags = MOUSEEVENTF_MOVE | MOUSEEVENTF_ABSOLUTE | MOUSEEVENTF_VIRTUALDESK;
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
