#include "inputmanager.h"
#include <QDebug>

#ifdef Q_OS_LINUX
#include <X11/Xlib.h>
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

InputManager::~InputManager()
{
#ifdef Q_OS_LINUX
    destroyUinput();
    if (xDisplay_) {
        XCloseDisplay(static_cast<Display*>(xDisplay_));
        xDisplay_ = nullptr;
    }
#endif
}
