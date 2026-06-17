#include "systemsleepblocker.h"
#import <IOKit/pwr_mgt/IOPMLib.h>
#import <CoreFoundation/CoreFoundation.h>

class MacPlatformPrivate : public SystemSleepBlocker::PlatformPrivate
{
public:
    MacPlatformPrivate() : assertionID(0) {}

    bool inhibit() override
    {
        CFStringRef reason = CFSTR("Application requires system wake.");
        IOReturn res = IOPMAssertionCreateWithName(
                            kIOPMAssertionTypePreventSystemSleep,
                            kIOPMAssertionLevelOn,
                            reason,
                            &assertionID);
        return (res == kIOReturnSuccess);
    }

    void release() override
    {
        if (assertionID)
            IOPMAssertionRelease(assertionID);
        assertionID = 0;
    }

private:
    IOPMAssertionID assertionID;
};

std::unique_ptr<SystemSleepBlocker::PlatformPrivate> createPlatformPrivate()
{
    return std::unique_ptr<SystemSleepBlocker::PlatformPrivate>(new MacPlatformPrivate());
}