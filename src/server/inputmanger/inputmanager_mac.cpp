#include "inputmanager.h"
#include <QCursor>
#include <QDebug>
#include <QPoint>

#include <CoreGraphics/CoreGraphics.h>

static CGKeyCode domCodeToMacKeycode(const QString& code)
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
        QChar c = code[3];
        if (c >= 'A' && c <= 'Z')
            return static_cast<CGKeyCode>(c.toLatin1() - 'A' + 0x00);
        return 0;
    }
    if (code.startsWith("Digit") && code.length() == 6) {
        QChar c = code[5];
        if (c >= '0' && c <= '9')
            return static_cast<CGKeyCode>(c.toLatin1() - '0' + 0x1D);
        return 0;
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

    return 0;
}

void InputManager::injectMouseMove(int x, int y)
{
    CGDirectDisplayID display = CGMainDisplayID();
    CGFloat h = CGDisplayPixelsHigh(display);
    CGEventRef event = CGEventCreateMouseEvent(
        nullptr, kCGEventMouseMoved,
        CGPointMake(x, h - y), kCGMouseButtonLeft);
    CGEventPost(kCGHIDEventTap, event);
    CFRelease(event);
}

void InputManager::injectMouseButton(int x, int y, int button, bool isDown)
{
    CGDirectDisplayID display = CGMainDisplayID();
    CGFloat h = CGDisplayPixelsHigh(display);

    CGMouseButton btn;
    switch (button) {
    case 0: btn = kCGMouseButtonLeft;   break;
    case 1: btn = kCGMouseButtonCenter; break;
    case 2: btn = kCGMouseButtonRight;  break;
    default: return;
    }

    CGEventType type = isDown ? kCGEventLeftMouseDown + btn
                              : kCGEventLeftMouseUp + btn;
    CGEventRef event = CGEventCreateMouseEvent(
        nullptr, type, CGPointMake(x, h - y), btn);
    CGEventPost(kCGHIDEventTap, event);
    CFRelease(event);
}

void InputManager::injectWheel(int delta)
{
    CGEventRef event = CGEventCreateScrollWheelEvent(
        nullptr, kCGScrollEventUnitLine, 1, delta);
    CGEventPost(kCGHIDEventTap, event);
    CFRelease(event);
}

void InputManager::injectKeyboard(int keycode, const QString& code, bool isDown,
                                   bool ctrl, bool alt, bool shift)
{
    updateModifiers(ctrl, alt, shift);

    CGKeyCode macKey = domCodeToMacKeycode(code);
    if (macKey == 0 && !code.isEmpty()) {
        CGKeyCode fromKeycode = static_cast<CGKeyCode>(keycode);
        macKey = fromKeycode;
    }
    if (macKey == 0)
        return;

    if (code == "ControlLeft" || code == "ControlRight" ||
        code == "ShiftLeft" || code == "ShiftRight" ||
        code == "AltLeft" || code == "AltRight" ||
        code == "MetaLeft" || code == "MetaRight") {
        return;
    }

    CGEventRef event = CGEventCreateKeyboardEvent(nullptr, macKey, isDown);
    CGEventPost(kCGHIDEventTap, event);
    CFRelease(event);
}

void InputManager::updateModifiers(bool ctrl, bool alt, bool shift)
{
    if (ctrl != ctrlDown_) {
        CGEventRef e = CGEventCreateKeyboardEvent(nullptr, 0x3B, ctrl);
        CGEventPost(kCGHIDEventTap, e);
        CFRelease(e);
        ctrlDown_ = ctrl;
    }
    if (alt != altDown_) {
        CGEventRef e = CGEventCreateKeyboardEvent(nullptr, 0x3A, alt);
        CGEventPost(kCGHIDEventTap, e);
        CFRelease(e);
        altDown_ = alt;
    }
    if (shift != shiftDown_) {
        CGEventRef e = CGEventCreateKeyboardEvent(nullptr, 0x38, shift);
        CGEventPost(kCGHIDEventTap, e);
        CFRelease(e);
        shiftDown_ = shift;
    }
}

void InputManager::sendModifierEvent(int, bool)
{
}
