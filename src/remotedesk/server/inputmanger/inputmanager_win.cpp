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

// [A12] 需要 0xE0 前缀的「扩展键」。
// SendInput 只填 wVk 时，系统按虚拟键码反查 scan code；KEYEVENTF_EXTENDEDKEY
// 决定是否补 0xE0 前缀。缺了这个标志，按 scan code 判键的程序（终端类、部分
// 老控件、游戏）会把主键盘 Delete/Home/方向键当成小键盘等价键，或干脆不响应。
// 注意：小键盘 NumLock 关闭时的方向键也报 VK_LEFT/37 等，但它们**不是**扩展键，
// 由调用方用前端 code("Numpad4") 排除。
static bool isExtendedVk(UINT vk)
{
    switch (vk) {
    case VK_INSERT: case VK_DELETE: case VK_HOME: case VK_END:
    case VK_PRIOR:  case VK_NEXT:
    case VK_LEFT:   case VK_RIGHT:  case VK_UP:  case VK_DOWN:
    case VK_NUMLOCK: case VK_DIVIDE: case VK_SNAPSHOT:
    case VK_RCONTROL: case VK_RMENU:
    case VK_LWIN: case VK_RWIN: case VK_APPS:
        return true;
    default:
        return false;
    }
}

// 小键盘上那几个"与主键盘同名"的键（NumLock 关闭时前端 keyCode 与主键盘
// 完全相同），不能被当作扩展键处理。
static bool isNumpadTwin(UINT vk)
{
    switch (vk) {
    case VK_INSERT: case VK_DELETE: case VK_HOME: case VK_END:
    case VK_PRIOR:  case VK_NEXT:
    case VK_LEFT:   case VK_RIGHT:  case VK_UP:  case VK_DOWN:
        return true;
    default:
        return false;
    }
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

// 发送一次按键/抬起，返回是否真的注入成功。
// [A13] 调用方据此决定要不要提交修饰键记账：注入失败却提交状态，下一次按键
// 的 diff 会认为 Ctrl 已在按下状态，组合键前缀永久丢失（远端"Ctrl 失灵"）。
//
// [A12b] 必须走**扫描码**路径（wScan + KEYEVENTF_SCANCODE，wVk 留 0）。
// 只填 wVk 时 SendInput 照样返回 1（不算失败），但 XP 上实测**完全没有效果**：
// Win 键弹不出开始菜单、cmd 里打字零字符，而同一进程的鼠标注入一切正常。
// 扫描码是硬件层语义，直接进键盘状态机；VK 路径要靠系统把虚拟键翻成字符，
// 这条链在「服务进程以 lpDesktop=winsta0\default 拉起的 helper」里不生效。
//
// 隔离实验（XP 2026-10-29，xpuserrun 把探针放进用户会话跑 keystest.exe）：
//   mode 0  wVk=0x5B + EXTENDED            → 开始菜单不弹
//   mode 2  wScan=0x5B + EXTENDED|SCANCODE → 开始菜单弹出
//   mode21  Win+R 全扫描码                  → 弹出「运行」对话框
// 能拿到扫描码时一律走扫描码；MapVirtualKey 返回 0（媒体键等没有物理键位的
// VK）才退回 VK 路径，保证这类键仍然可注入。
static bool sendVkEvent(UINT vk, bool isDown, bool extended)
{
    INPUT input = {};
    input.type = INPUT_KEYBOARD;
    const UINT scan = MapVirtualKeyW(vk, MAPVK_VK_TO_VSC);
    DWORD flags = 0;
    if (!isDown)
        flags |= KEYEVENTF_KEYUP;
    if (extended)
        flags |= KEYEVENTF_EXTENDEDKEY;
    if (scan != 0) {
        input.ki.wScan = static_cast<WORD>(scan);
        input.ki.wVk = 0;
        flags |= KEYEVENTF_SCANCODE;
    } else {
        input.ki.wVk = static_cast<WORD>(vk);
    }
    input.ki.dwFlags = flags;
    return sendInputChecked(1, &input, sizeof(INPUT));
}

void InputManager::sendModifierEvent(int vk, bool isDown)
{
    sendVkEvent(static_cast<UINT>(vk), isDown,
                isExtendedVk(static_cast<UINT>(vk)));
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
    // [A13] 注入失败不记账，下次仍会重试。
    if (meta != metaDown_ && sendVkEvent(VK_LWIN, meta, /*extended=*/true))
        metaDown_ = meta;

    // [A12] 扩展键标志：主键盘 Delete/Home/方向键等需要 0xE0 前缀；小键盘上
    // 同名的键（前端 code 以 "Numpad" 开头）不需要，否则会多一个前缀。
    const UINT vk = static_cast<UINT>(keycode);
    const bool numpadTwin = code.startsWith(QLatin1String("Numpad"))
                            && isNumpadTwin(vk);
    sendVkEvent(vk, isDown, isExtendedVk(vk) && !numpadTwin);
}

void InputManager::updateModifiers(bool ctrl, bool alt, bool shift)
{
    // [A13] 与 Linux 实现对齐：注入失败不提交记账。
    if (ctrl != ctrlDown_ && sendVkEvent(VK_CONTROL, ctrl, false))
        ctrlDown_ = ctrl;
    if (alt != altDown_ && sendVkEvent(VK_MENU, alt, false))
        altDown_ = alt;
    if (shift != shiftDown_ && sendVkEvent(VK_SHIFT, shift, false))
        shiftDown_ = shift;
}
