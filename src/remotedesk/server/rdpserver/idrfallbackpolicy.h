#ifndef IDRFALLBACKPOLICY_H
#define IDRFALLBACKPOLICY_H

// [PLI] IDR 兜底重建的纯决策逻辑。
//
// 抽成无状态函数有两个原因：
// 1) 这些分支只有在"编码器不认 requestKeyframe"（部分软编 flush 后仍出 P 帧）
//    或编码线程卡死时才真实触发，端到端难以稳定复现；纯函数可以直接覆盖
//    "冷却重挂 / 等当前帧跑完 / 超过硬上限强制重建 / 立即重建"四条路径。
// 2) 时间参数由调用方传入（单调时钟毫秒），便于在测试里构造时钟跳变等边界。
//
// 所有时刻都是**单调时钟**毫秒：调用方不得混入墙钟（QDateTime），否则 NTP 校时
// 会让 now - lastReinitMs 突变，导致误判冷却已过（连续重建）或永不重建。

#include <QtGlobal>

struct IdrFallbackDecision {
    enum Action {
        Rebuild, // 重建编码器（重建后首帧必为 IDR）
        Retry,   // 条件未满足：按 retryMs 重挂单次定时器，绝不能直接放弃
    };

    Action action = Rebuild;
    int retryMs = 0;                 // action == Retry 时的重挂间隔
    bool busyOverHardLimit = false;  // Rebuild 且编码线程已超硬上限（卡死/追不上）
};

// 判断 IDR 兜底定时器到点后该做什么。
//   nowMs            当前单调时钟
//   requestAtMs      发起新 IDR 请求的时刻（>0）
//   lastReinitMs     上次重建编码器的时刻（冷却起点）
//   busyMs           编码线程在当前帧上的已耗时（0 = 空闲）
//   lastFrameCostMs  最近一帧完整耗时（0 = 尚无样本）
//   cooldownMs       两次重建之间的最小间隔（MPP 反复 init 有固件崩溃历史）
inline IdrFallbackDecision decideIdrFallback(qint64 nowMs, qint64 requestAtMs,
                                             qint64 lastReinitMs, qint64 busyMs,
                                             qint64 lastFrameCostMs,
                                             qint64 cooldownMs = 1000)
{
    IdrFallbackDecision d;

    // 冷却未到：重挂到冷却结束。单次定时器一旦停掉，idrRequestMs_ 就成了永远
    // 没人处理的悬空请求（hasIdr_ 为真时周期定时器的兜底分支也不会介入）。
    const qint64 sinceReinit = nowMs - lastReinitMs;
    if (sinceReinit < cooldownMs) {
        d.action = IdrFallbackDecision::Retry;
        d.retryMs = static_cast<int>(qBound<qint64>(50, cooldownMs - sinceReinit + 50, 60000));
        return d;
    }

    // 编码线程在飞时不能重建：shutdownRetire() 会置 abort_ 并在主线程等它跑完
    // （最多 8s），initialize() 还会清空待编码队列。固定 1.2s 超时在单帧超过
    // 1.2s 的机器上等于把每帧都掐断，永远产不出 IDR（QEMU TCG 1080p 实测活锁）。
    // 按实测单帧耗时放宽等待后重试。
    //
    // 但 busy 永不归零也可能是线程卡死在驱动调用里（或单帧耗时已经大到永远追不上），
    // 无限推迟等于永久黑屏：超过硬上限强制重建，由 shutdownRetire() 8s 后放弃
    // 等待、把旧实例退役泄漏并新建，服务本身不受连坐。
    const qint64 hardLimit = qMax<qint64>(30000, 10 * lastFrameCostMs);
    if (busyMs > 0 && nowMs - requestAtMs < hardLimit) {
        const qint64 cost = qMax<qint64>(1000, lastFrameCostMs);
        d.action = IdrFallbackDecision::Retry;
        d.retryMs = static_cast<int>(qBound<qint64>(200, 3 * cost - busyMs, 60000));
        return d;
    }

    d.action = IdrFallbackDecision::Rebuild;
    d.busyOverHardLimit = (busyMs > 0);
    return d;
}

#endif // IDRFALLBACKPOLICY_H
