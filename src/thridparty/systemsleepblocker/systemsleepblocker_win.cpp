#include "systemsleepblocker.h"
#include <windows.h>

class WinPlatformPrivate : public SystemSleepBlocker::PlatformPrivate {
public:
    bool inhibit() override
    {
        EXECUTION_STATE prev = SetThreadExecutionState(ES_CONTINUOUS | ES_SYSTEM_REQUIRED);
        return prev != 0;
    }

    void release() override
    {
        SetThreadExecutionState(ES_CONTINUOUS);
    }

    void refresh() override
    {
        SetThreadExecutionState(ES_CONTINUOUS | ES_SYSTEM_REQUIRED);
    }
};

std::unique_ptr<SystemSleepBlocker::PlatformPrivate> createPlatformPrivate()
{
    return std::unique_ptr<SystemSleepBlocker::PlatformPrivate>(new WinPlatformPrivate());
}
