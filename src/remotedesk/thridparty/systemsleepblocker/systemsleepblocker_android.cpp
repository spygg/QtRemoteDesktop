#include "systemsleepblocker.h"

class AndroidPlatformPrivate : public SystemSleepBlocker::PlatformPrivate
{
public:
    bool inhibit() override { return true; }
    void release() override {}
};

std::unique_ptr<SystemSleepBlocker::PlatformPrivate> createPlatformPrivate()
{
    return std::unique_ptr<SystemSleepBlocker::PlatformPrivate>(new AndroidPlatformPrivate());
}
