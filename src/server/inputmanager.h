// server/input_manager.h
#ifndef INPUT_MANAGER_H
#define INPUT_MANAGER_H

#include <QObject>
#include <QString>

#ifdef Q_OS_WIN
#include <windows.h>
#endif

#ifdef Q_OS_LINUX
// 不在头文件中 include X11 头文件，避免 Status 类型与 Qt 冲突
// X11 的 Display* 用 void* 代替，KeySym 用 unsigned long 代替
// 实际类型转换在 .cpp 中完成
typedef unsigned long X11KeySym; // 等价于 X11 KeySym
#endif

class InputManager : public QObject {
    Q_OBJECT

public:
    explicit InputManager(QObject *parent = nullptr);
    ~InputManager();

    void injectMouseMove(int x, int y);
    void injectMouseButton(int x, int y, int button, bool isDown);
    void injectWheel(int delta);
    void injectKeyboard(int keycode, bool isDown, bool ctrl = false,
                        bool alt = false, bool shift = false);

    void injectKeyboard(int keycode, const QString &code, bool isDown, bool ctrl, bool alt, bool shift);
private:
    void updateModifiers(bool ctrl, bool alt, bool shift);
    void sendModifierEvent(int vk, bool isDown);

#ifdef Q_OS_WIN
    void sendInput(INPUT &input);
#endif

#ifdef Q_OS_LINUX
    void* xDisplay_ = nullptr; // 实际为 Display*，在 .cpp 中转换
    void sendXModifier(X11KeySym ks, bool isDown);
#endif

    // 修饰键状态跟踪（避免重复注入）
    bool ctrlDown_  = false;
    bool altDown_   = false;
    bool shiftDown_ = false;
};

#endif
