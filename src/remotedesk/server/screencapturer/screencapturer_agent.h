// server/screencapturer/screencapturer_agent.h
//
// 把"用户会话代理送来的帧"适配成 PlatformCapturer。
//
// 该对象挂在 ScreenCapturer 的 waylandCapturer_ 槽位里（复用 useWayland_ 语义，
// 不新增成员），因此 ScreenCapturer 的宽高/原点/暂停逻辑全部原样生效。
// 代理上报的宽高就是坐标映射基准，originX/Y 恒为 0，root 不再叠加补偿。
#pragma once

#include "screencapturer.h"

class SessionAgentClient;

class AgentCapturer : public PlatformCapturer
{
public:
    AgentCapturer() = default;
    ~AgentCapturer() override = default;

    bool initialize() override;
    bool captureFrame(QImage& outImage, bool* updated = nullptr) override;
    int width() const override;
    int height() const override;
    void setPaused(bool paused) override;
    void requestFullCapture() override;
    void requestForceFrame() override;
    // 每帧都是 root 从 shm 深拷贝出来的独立 QImage，编码线程可以安全直读。
    bool framesAreExclusive() const override { return true; }

private:
    SessionAgentClient* client_ = nullptr;
    bool initLogged_ = false;
};
