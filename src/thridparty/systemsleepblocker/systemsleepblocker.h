#ifndef SYSTEMSLEEPBLOCKER_H
#define SYSTEMSLEEPBLOCKER_H

#include <QObject>
#include <memory>

class SystemSleepBlocker : public QObject {
    Q_OBJECT

public:
    // 内部抽象接口（平台实现需继承此类）
    // 请勿在用户代码中直接使用
    class PlatformPrivate {
    public:
        virtual ~PlatformPrivate() = default;
        virtual bool inhibit() = 0; // 获取休眠锁
        virtual void release() = 0; // 释放休眠锁
        virtual void refresh() { } // 刷新锁（Windows 需要）
    };

    explicit SystemSleepBlocker(QObject* parent = nullptr);
    ~SystemSleepBlocker() override;

    bool start(int refreshIntervalMs = 30000);
    void stop();
    bool isActive() const;

protected:
    void timerEvent(QTimerEvent* event) override;

private:
    std::unique_ptr<PlatformPrivate> d;
    int m_timerId = 0;
    bool m_active = false;
};

#endif // SYSTEMSLEEPBLOCKER_H
