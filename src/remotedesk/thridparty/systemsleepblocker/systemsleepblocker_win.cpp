#include "systemsleepblocker.h"
#include <windows.h>

class WinPlatformPrivate : public SystemSleepBlocker::PlatformPrivate {
public:
    bool inhibit() override
    {
        // [B19] 必须加 ES_DISPLAY_REQUIRED：远程桌面场景屏幕常被设为
        // 分钟级自动关闭，显示器一关大多数采集后端（GDI/DXGI）只能采到黑帧，
        // 远控画面变黑屏。ES_SYSTEM_REQUIRED 只阻止睡眠，不管显示器。
        EXECUTION_STATE prev = SetThreadExecutionState(ES_CONTINUOUS | ES_SYSTEM_REQUIRED | ES_DISPLAY_REQUIRED);
        return prev != 0;
    }

    void release() override
    {
        SetThreadExecutionState(ES_CONTINUOUS);
    }

    void refresh() override
    {
        SetThreadExecutionState(ES_CONTINUOUS | ES_SYSTEM_REQUIRED | ES_DISPLAY_REQUIRED);
    }
};

std::unique_ptr<SystemSleepBlocker::PlatformPrivate> createPlatformPrivate()
{
    return std::unique_ptr<SystemSleepBlocker::PlatformPrivate>(new WinPlatformPrivate());
}
