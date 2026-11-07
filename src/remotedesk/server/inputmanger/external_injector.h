// server/inputmanger/external_injector.h
//
// 输入接管接口（header-only，避免让 InputManager 依赖会话代理的实现）。
//
// 语义：InputManager 在每次注入前先问注入器；返回 true 表示本次事件已被
// 外部接管（例如转发给用户会话里的代理），InputManager 必须立即返回、
// 不再走本地 XTest / uinput 路径。isActive() 为 false 时直接回落本地实现。
//
// 坐标约定：x/y 是**采集画面坐标系**（左上角为 0,0）。代理上报的宽高就是
// 坐标映射基准，root 侧不再叠加任何偏移补偿。
#pragma once

#include <QString>

class ExternalInjector
{
public:
    virtual ~ExternalInjector() = default;

    // 是否处于接管状态（代理已握手并出帧）。
    virtual bool isActive() const = 0;

    virtual bool injectMouseMove(int x, int y) = 0;
    virtual bool injectMouseButton(int x, int y, int button, bool isDown) = 0;
    virtual bool injectWheel(int delta) = 0;
    virtual bool injectKeyboard(int keycode, const QString& code, bool isDown,
                                bool ctrl, bool alt, bool shift,
                                bool isChar, bool meta) = 0;
    // 修饰键状态同步（rdpserver 在自动重复恢复后会单独调用）。
    virtual bool updateModifiers(bool ctrl, bool alt, bool shift) = 0;
};
