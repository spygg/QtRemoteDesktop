#include "inputmanager.h"
#include <QCursor>
#include <QDebug>
#include <QPoint>

#include <CoreGraphics/CoreGraphics.h>

// CGEventCreate* 系列在失败时返回 NULL，直接 CGEventPost/CFRelease 会崩溃
// （CFRelease(NULL) 是未定义行为）。统一走这个包装：失败时告警并跳过。
static void postEventAndRelease(CGEventRef event, const char* what)
{
    if (!event) {
        qWarning() << "InputManager(mac): CGEventCreate returned NULL for" << what;
        return;
    }
    CGEventPost(kCGHIDEventTap, event);
    CFRelease(event);
}

// 返回 macOS CGKeyCode；未识别时返回 -1。
// [P0 fix] 禁止用 0 作"未找到"哨兵：kVK_ANSI_A = 0x00 是合法键码，
// 旧逻辑会把 A 键误判为未找到并回退到 JS 码点（'A'=65）→ 敲错物理键。
static int domCodeToMacKeycode(const QString& code)
{
    if (code == "Delete")        return 0x75;
    if (code == "Backspace")     return 0x33;
    if (code == "Enter")         return 0x24;
    if (code == "Tab")           return 0x30;
    if (code == "Escape")        return 0x35;
    if (code == "Space")         return 0x31;
    if (code == "ArrowUp")       return 0x7E;
    if (code == "ArrowDown")     return 0x7D;
    if (code == "ArrowLeft")     return 0x7B;
    if (code == "ArrowRight")    return 0x7C;
    if (code == "Home")          return 0x73;
    if (code == "End")           return 0x77;
    if (code == "PageUp")        return 0x74;
    if (code == "PageDown")      return 0x79;
    if (code == "Insert")        return 0x72;
    if (code == "CapsLock")      return 0x39;
    if (code == "NumLock")       return 0x47;
    if (code == "ScrollLock")    return 0x6B;
    if (code == "Pause")         return 0x71;
    if (code == "PrintScreen")   return 0x69;
    if (code == "ContextMenu")   return 0x6E;

    if (code.startsWith("Key") && code.length() == 4) {
        // macOS CGKeyCode 是 QWERTY 物理布局码，并非 ASCII 连续码（按 Apple kVK_*）：
        // A=0x00 B=0x0B C=0x08 D=0x02 E=0x0E F=0x03 G=0x05 H=0x04 I=0x22 J=0x26
        // K=0x28 L=0x25 M=0x2E N=0x2D O=0x1F P=0x23 Q=0x0C R=0x0F S=0x01 T=0x11
        // U=0x20 V=0x09 W=0x0D X=0x07 Y=0x10 Z=0x06
        static const CGKeyCode kLetters[] = {
            0x00,0x0B,0x08,0x02,0x0E,0x03,0x05,0x04,0x22,0x26,0x28,0x25,0x2E,
            0x2D,0x1F,0x23,0x0C,0x0F,0x01,0x11,0x20,0x09,0x0D,0x07,0x10,0x06
        };
        QChar c = code[3];
        if (c >= 'A' && c <= 'Z')
            return kLetters[c.toLatin1() - 'A'];
        return -1;
    }
    if (code.startsWith("Digit") && code.length() == 6) {
        // 1=0x12,2=0x13,3=0x14,4=0x15,5=0x17,6=0x16,7=0x1A,8=0x1C,9=0x19,0=0x1D
        static const CGKeyCode kDigits[] = { 0x1D,0x12,0x13,0x14,0x15,0x17,0x16,0x1A,0x1C,0x19 };
        QChar c = code[5];
        if (c >= '0' && c <= '9')
            return kDigits[c.toLatin1() - '0'];
        return -1;
    }

    if (code == "Period")        return 0x2F;
    if (code == "Comma")         return 0x2B;
    if (code == "Slash")         return 0x2C;
    if (code == "Semicolon")     return 0x29;
    if (code == "Quote")         return 0x27;
    if (code == "BracketLeft")   return 0x21;
    if (code == "BracketRight")  return 0x1E;
    if (code == "Backslash")     return 0x2A;
    if (code == "Minus")         return 0x1B;
    if (code == "Equal")         return 0x18;
    if (code == "Backquote")     return 0x32;

    if (code == "F1")  return 0x7A;
    if (code == "F2")  return 0x78;
    if (code == "F3")  return 0x63;
    if (code == "F4")  return 0x76;
    if (code == "F5")  return 0x60;
    if (code == "F6")  return 0x61;
    if (code == "F7")  return 0x62;
    if (code == "F8")  return 0x64;
    if (code == "F9")  return 0x65;
    if (code == "F10") return 0x6D;
    if (code == "F11") return 0x67;
    if (code == "F12") return 0x6F;

    if (code == "ControlLeft")  return 0x3B;
    if (code == "ControlRight") return 0x3E;
    if (code == "ShiftLeft")    return 0x38;
    if (code == "ShiftRight")   return 0x3C;
    if (code == "AltLeft")      return 0x3A;
    if (code == "AltRight")     return 0x3D;
    if (code == "MetaLeft")     return 0x37;
    if (code == "MetaRight")    return 0x37;

    if (code.startsWith("Numpad")) {
        if (code == "NumpadEnter")    return 0x4C;
        if (code == "NumpadAdd")      return 0x45;
        if (code == "NumpadSubtract") return 0x4E;
        if (code == "NumpadMultiply") return 0x43;
        if (code == "NumpadDivide")   return 0x4B;
        if (code == "NumpadDecimal")  return 0x41;
        if (code == "Numpad0") return 0x52;
        if (code == "Numpad1") return 0x53;
        if (code == "Numpad2") return 0x54;
        if (code == "Numpad3") return 0x55;
        if (code == "Numpad4") return 0x56;
        if (code == "Numpad5") return 0x57;
        if (code == "Numpad6") return 0x58;
        if (code == "Numpad7") return 0x59;
        if (code == "Numpad8") return 0x5B;
        if (code == "Numpad9") return 0x5C;
    }

    return -1;
}

// 采集端固定只采主屏（MacCapturer::initialize 里 displayID_ = CGMainDisplayID()），
// 因此输入也必须固定映射到主屏 —— 尺寸不能当成屏身份。
// 旧实现先按"点尺寸"遍历所有活动屏：Retina 主屏 1920×1080 点 + 4K@1x 外接屏时，
// 前端上报 3840×2160 会先命中点尺寸恰好为 3840×2160 的外接屏（那其实是主屏的
// 物理像素），鼠标/点击被送到根本没在采集的那块屏。
// 这里直接返回主屏；坐标仍由 capturePointToGlobal 用 bounds(点)/上报尺寸缩放，
// 所以前端按点或按像素上报都能落回主屏的正确位置。
static CGDirectDisplayID captureDisplayForSize(int w, int h)
{
    (void)w;   // 形参保留，调用方语义不变（未来若采集多屏再按尺寸匹配）
    (void)h;
    return CGMainDisplayID();
}

// 把“捕获区相对坐标”换算成 CGEvent 需要的全局点坐标。
// 比例必须用 bounds(点)/前端尺寸，而不是拿像素当点：Retina 下 2x 屏的
// 像素尺寸是点尺寸的两倍，直接传像素会落到一半位置。
static CGPoint capturePointToGlobal(CGDirectDisplayID display, int x, int y,
                                    int srcW, int srcH)
{
    const CGRect b = CGDisplayBounds(display);
    // 尺寸缺失时退回该屏的点尺寸（与 b 同一坐标系），绝不要把像素尺寸与点混用。
    const CGFloat w = srcW > 0 ? srcW : b.size.width;
    const CGFloat h = srcH > 0 ? srcH : b.size.height;
    const CGFloat sx = w > 0 ? b.size.width / w : 1.0;
    const CGFloat sy = h > 0 ? b.size.height / h : 1.0;
    // 前端与 Linux/Windows 注入统一为左上角原点、y 向下；CGDisplayBounds 的全局
    // 坐标同样是左上角原点、y 向下，因此两轴都直接按比例映射（旧实现 h - y 会把
    // 指针上下镜像）。
    return CGPointMake(b.origin.x + x * sx, b.origin.y + y * sy);
}

void InputManager::injectMouseMove(int x, int y)
{
    const CGDirectDisplayID display = captureDisplayForSize(screenW_, screenH_);
    CGEventRef event = CGEventCreateMouseEvent(
        nullptr, kCGEventMouseMoved,
        capturePointToGlobal(display, x, y, screenW_, screenH_),
        kCGMouseButtonLeft);
    postEventAndRelease(event, "mouse");
}

void InputManager::injectMouseButton(int x, int y, int button, bool isDown)
{
    const CGDirectDisplayID display = captureDisplayForSize(screenW_, screenH_);

    CGMouseButton btn;
    switch (button) {
    case 0: btn = kCGMouseButtonLeft;   break;
    case 1: btn = kCGMouseButtonCenter; break;
    case 2: btn = kCGMouseButtonRight;  break;
    default: return;
    }

    // 不能用 kCGEventLeftMouseDown + btn：常量值并不连续
    // （Left=1, Right=3, Other=25），btn=Center 时会算成 2=kCGEventRightMouseDown，
    // 中键被注入成右键按下。必须显式映射。
    CGEventType type;
    if (btn == kCGMouseButtonCenter)
        type = isDown ? kCGEventOtherMouseDown : kCGEventOtherMouseUp;
    else if (btn == kCGMouseButtonRight)
        type = isDown ? kCGEventRightMouseDown : kCGEventRightMouseUp;
    else
        type = isDown ? kCGEventLeftMouseDown : kCGEventLeftMouseUp;
    CGEventRef event = CGEventCreateMouseEvent(
        nullptr, type,
        capturePointToGlobal(display, x, y, screenW_, screenH_), btn);
    postEventAndRelease(event, "mouse");
}

void InputManager::injectWheel(int delta)
{
    CGEventRef event = CGEventCreateScrollWheelEvent(
        nullptr, kCGScrollEventUnitLine, 1, delta);
    postEventAndRelease(event, "mouse");
}

void InputManager::injectKeyboard(int keycode, const QString& code, bool isDown, bool ctrl, bool alt, bool shift, bool useVkFallback, bool isChar, bool meta)
{
    Q_UNUSED(useVkFallback);
    Q_UNUSED(isChar);
    Q_UNUSED(meta);
    updateModifiers(ctrl, alt, shift);

    int macKey = domCodeToMacKeycode(code);
    if (macKey < 0 && !code.isEmpty()) {
        // 未识别 DOM code 时才回退到 JS keycode（合成键），合法键码 0（A 键）直达下方
        macKey = static_cast<int>(keycode);
    }
    if (macKey < 0)
        return;

    if (code == "ControlLeft" || code == "ControlRight" ||
        code == "ShiftLeft" || code == "ShiftRight" ||
        code == "AltLeft" || code == "AltRight" ||
        code == "MetaLeft" || code == "MetaRight") {
        return;
    }

    CGEventRef event = CGEventCreateKeyboardEvent(nullptr, static_cast<CGKeyCode>(macKey), isDown);
    postEventAndRelease(event, "mouse");
}

void InputManager::updateModifiers(bool ctrl, bool alt, bool shift)
{
    if (ctrl != ctrlDown_) {
        CGEventRef e = CGEventCreateKeyboardEvent(nullptr, 0x3B, ctrl);
        postEventAndRelease(e, "modifier");
        ctrlDown_ = ctrl;
    }
    if (alt != altDown_) {
        CGEventRef e = CGEventCreateKeyboardEvent(nullptr, 0x3A, alt);
        postEventAndRelease(e, "modifier");
        altDown_ = alt;
    }
    if (shift != shiftDown_) {
        CGEventRef e = CGEventCreateKeyboardEvent(nullptr, 0x38, shift);
        postEventAndRelease(e, "modifier");
        shiftDown_ = shift;
    }
}

void InputManager::sendModifierEvent(int, bool)
{
}

QPoint InputManager::cursorPosition() const
{
    // 与 X11/Windows 统一：返回捕获区相对坐标（原点由调用方 setScreenOffset 同步）。
    return QCursor::pos() - QPoint(screenOffsetX_, screenOffsetY_);
}
