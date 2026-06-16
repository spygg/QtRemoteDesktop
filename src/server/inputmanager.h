#ifndef INPUT_MANAGER_H
#define INPUT_MANAGER_H

#include <QObject>
#include <QString>

#ifdef Q_OS_WIN
#include <windows.h>
#endif

#ifdef Q_OS_LINUX
typedef unsigned long X11KeySym;
#endif

class InputManager : public QObject {
    Q_OBJECT

public:
    explicit InputManager(QObject *parent = nullptr);
    ~InputManager();

    void injectMouseMove(int x, int y);
    void injectMouseButton(int x, int y, int button, bool isDown);
    void injectWheel(int delta);
    void injectKeyboard(int keycode, const QString &code, bool isDown, bool ctrl, bool alt, bool shift);

#ifdef Q_OS_WIN
    // Connect/disconnect to the SYSTEM-level keyboard service
    bool connectKeyboardService();
    void disconnectKeyboardService();
    bool isKeyboardServiceConnected() const { return servicePipe_ != INVALID_HANDLE_VALUE; }
#elif defined(Q_OS_LINUX)
    // Switch to kernel-level uinput (works on lock screen / Wayland)
    bool initUinput();
    void destroyUinput();
    bool isUinputActive() const { return uinputFd_ >= 0; }
#endif

private:
    void updateModifiers(bool ctrl, bool alt, bool shift);
    void sendModifierEvent(int vk, bool isDown);

#ifdef Q_OS_WIN
    void sendInput(INPUT &input);
    bool sendToService(BYTE type, const BYTE* data, DWORD dataLen);
    HANDLE servicePipe_ = INVALID_HANDLE_VALUE;
#endif

#ifdef Q_OS_LINUX
    void* xDisplay_ = nullptr;
    void sendXModifier(X11KeySym ks, bool isDown);
    int uinputFd_ = -1;
    bool sendUinputKey(unsigned short linuxKeycode, bool isDown);
    unsigned short keysymToLinuxKeycode(unsigned long ks);
    unsigned long lockScreenWindow_ = 0;
    void focusLockScreenWindow(void* dpy);
#endif

    bool ctrlDown_  = false;
    bool altDown_   = false;
    bool shiftDown_ = false;
};

#endif
