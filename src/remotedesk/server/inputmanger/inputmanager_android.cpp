#include "inputmanager.h"
#include <QDebug>
#include <QCursor>

void InputManager::injectMouseMove(int, int)
{
    qWarning() << "InputManager: mouse injection not supported on Android (requires root)";
}

void InputManager::injectMouseButton(int, int, int, bool)
{
    qWarning() << "InputManager: mouse injection not supported on Android (requires root)";
}

void InputManager::injectWheel(int)
{
    qWarning() << "InputManager: scroll injection not supported on Android";
}

void InputManager::injectKeyboard(int, const QString&, bool, bool, bool, bool, bool, bool, bool)
{
    qWarning() << "InputManager: keyboard injection not supported on Android (requires root)";
}

void InputManager::updateModifiers(bool, bool, bool)
{
    // Not supported on Android
}

void InputManager::sendModifierEvent(int, bool)
{
}

QPoint InputManager::cursorPosition() const
{
    // 与其它平台统一：返回捕获区相对坐标。
    return QCursor::pos() - QPoint(screenOffsetX_, screenOffsetY_);
}
