#include "systemsleepblocker.h"
#include <QDBusInterface>
#include <QDBusReply>
#include <QDBusUnixFileDescriptor>

class LinuxPlatformPrivate : public SystemSleepBlocker::PlatformPrivate
{
public:
    bool inhibit() override
    {
        QDBusInterface manager("org.freedesktop.login1",
                               "/org/freedesktop/login1",
                               "org.freedesktop.login1.Manager",
                               QDBusConnection::systemBus());
        QDBusReply<QDBusUnixFileDescriptor> reply = manager.call(
            "Inhibit", "idle", "MyApp", "Long-running operation");

        if (reply.isValid()) {
            fd = reply.value();
            return fd.isValid();
        }
        return false;
    }

    void release() override
    {
        // 文件描述符关闭即自动解除 inhibit
        fd = QDBusUnixFileDescriptor();
    }

private:
    QDBusUnixFileDescriptor fd;
};

std::unique_ptr<SystemSleepBlocker::PlatformPrivate> createPlatformPrivate()
{
    return std::unique_ptr<SystemSleepBlocker::PlatformPrivate>(new LinuxPlatformPrivate());
}