// server/input_manager.h
#ifndef INPUT_MANAGER_H
#define INPUT_MANAGER_H

#include <QObject>

#ifdef Q_OS_WIN
#include <windows.h>
#endif

class InputManager : public QObject {
    Q_OBJECT

public:
    explicit InputManager(QObject *parent = nullptr);

    void injectMouseMove(int x, int y);
    void injectMouseButton(int x, int y, int button, bool isDown);
    void injectWheel(int delta);
    void injectKeyboard(int keycode, bool isDown, bool ctrl = false,
                        bool alt = false, bool shift = false);

    void injectKeyboard(int keycode, const QString &code, bool isDown, bool ctrl, bool alt, bool shift);
private:
    void updateModifiers(bool ctrl, bool alt, bool shift);

#ifdef Q_OS_WIN
    void sendInput(INPUT &input);
#endif
};

#endif
