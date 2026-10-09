// FrameRateMeter.hpp — 滚动窗口**瞬时**帧率计（header-only）
//
// 为什么需要它（2026-09-23 板端现象驱动）：
//   CoreRuntime 曾用 `published 帧数 ÷ 链路启动至今的秒数` 当 fps。那是**累计平均**，
//   分母里永久含着启动期的一次性开销（3 个 worker 各 ~70ms 加载 + 采集起流 + 预热），
//   于是面板上的帧率会「从 140 出头一点一点往上涨」，越涨越慢地渐近真实值 ——
//   看起来像"性能在爬坡"，其实是**统计口径**问题。
//   数学上 fps(T) = real × (1 - T_startup / T)，永远到不了 real。
//
//   采集侧（V4L2Capture）早就是滚动 1s 窗口，所以 capture_fps 显示 144.0 是准的；
//   本类把同一口径复用到推理侧，使 `fps()` 在第 2 帧就能给出真实瞬时值
//   （"打开就是满的"），而不是等几十秒渐近。
//
// 线程模型：tick() 在 3 个 worker 线程里调用（合计 = 实际推理帧率），
//   fps() 在 IPC/metrics 采样线程调用。窗口 512 帧 ⇒ 144 fps 下覆盖约 3.5 s，
//   锁竞争量级 = 144 次/秒，可忽略。
#pragma once

#include <array>
#include <chrono>
#include <cstdint>
#include <mutex>

namespace ttbox::core {

// 滑动窗口帧率统计（保留最近 kWindow 帧的到达时刻，线程安全）。
class FrameRateMeter {
public:
    // 144 fps 下约 3.5 s 样本；够平滑，又能跟上一两次掉帧。
    static constexpr size_t kWindow = 512;

    // 清空窗口，重新开始统计（如链路重启时）。
    void reset() {
        std::lock_guard<std::mutex> lk(mutex_);
        head_ = 0;
        count_ = 0;
        ts_.fill(0);
    }

    // 每完成一帧推理调用一次（任意一个 worker 发布结果时）。
    void tick() {
        const int64_t now = steady_ms();
        std::lock_guard<std::mutex> lk(mutex_);
        ts_[head_] = now;
        head_ = (head_ + 1) % kWindow;
        if (count_ < kWindow) ++count_;
    }

    // 最近窗口内的瞬时帧率。样本不足 2 帧时返回 0（调用方回退到累计平均）。
    double fps() const {
        std::lock_guard<std::mutex> lk(mutex_);
        if (count_ < 2) return 0.0;
        const size_t newest = (head_ + kWindow - 1) % kWindow;
        const size_t oldest = (head_ + kWindow - count_) % kWindow;
        const int64_t span = ts_[newest] - ts_[oldest];
        if (span <= 0) return 0.0;
        return static_cast<double>(count_ - 1) * 1000.0 / static_cast<double>(span);
    }

    // 当前窗口内的样本数（供调用方判断 fps() 是否已有足够样本）。
    size_t sample_count() const {
        std::lock_guard<std::mutex> lk(mutex_);
        return count_;
    }

private:
    static int64_t steady_ms() {
        return std::chrono::duration_cast<std::chrono::milliseconds>(
                   std::chrono::steady_clock::now().time_since_epoch())
            .count();
    }

    mutable std::mutex mutex_;
    std::array<int64_t, kWindow> ts_{};
    size_t head_ = 0;
    size_t count_ = 0;
};

}  // namespace ttbox::core
