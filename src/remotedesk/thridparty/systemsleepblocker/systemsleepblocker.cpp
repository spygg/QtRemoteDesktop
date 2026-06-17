#include "systemsleepblocker.h"
#include <QTimerEvent>

// 工厂函数声明 —— 由平台文件提供定义
std::unique_ptr<SystemSleepBlocker::PlatformPrivate> createPlatformPrivate();

SystemSleepBlocker::SystemSleepBlocker(QObject* parent)
    : QObject(parent)
    , d(createPlatformPrivate())
{
}

SystemSleepBlocker::~SystemSleepBlocker()
{
    stop();
}

bool SystemSleepBlocker::start(int refreshIntervalMs)
{
    if (m_active)
        return true;

    if (!d->inhibit())
        return false;

    m_active = true;
    m_timerId = startTimer(refreshIntervalMs);
    return true;
}

void SystemSleepBlocker::stop()
{
    if (!m_active)
        return;

    killTimer(m_timerId);
    m_timerId = 0;
    d->release();
    m_active = false;
}

bool SystemSleepBlocker::isActive() const
{
    return m_active;
}

void SystemSleepBlocker::timerEvent(QTimerEvent* event)
{
    Q_UNUSED(event);
    d->refresh();
}
