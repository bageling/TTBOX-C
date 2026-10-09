// Stats.hpp — 通用耗时统计（min/avg/p50/p95/p99/max），header-only
//
// S6 内存泄漏修复（2026-09-22）：samples_ 曾是无界 push_back——worker 每帧多路
// add 且 stats_ 从不清空（engine/decoder 的统计有逐帧 reset，worker 自身没有），
// 3 个 worker 各自堆 arena 以 ~112 kB/min/worker 线性累积、填满后随 vector 容量
// 翻倍出现 +60 MB 级台阶（板上 smaps 实证，见 .workbuddy/artifacts/长稳-2h-结论-2026-09-19.md）。
// 现改为**滑动窗口**：上限 kMaxSamples，满后环形覆盖最旧样本。消费方（CoreRuntime
// snapshot）只用 avg/percentile/max，「最近 N 个样本」的语义完全兼容且更有代表性；
// 顺带把 percentile 的全量拷贝+排序从无界降为 O(kMax)。
#pragma once

#include <algorithm>
#include <cstdint>
#include <mutex>
#include <numeric>
#include <vector>

namespace ttbox::core {

// 滑动窗口统计（count/min/avg/max/percentile），线程安全。
class StatsCollector {
public:
    // 窗口上限：30 fps 下约 2 分钟样本量，p95/p99 统计意义充分；
    // 内存上界 = kMaxSamples × 8 B ≈ 32 kB/实例。
    static constexpr size_t kMaxSamples = 4096;

    StatsCollector() = default;

    // ★ 合并专用构造（2026-09-23 全仓审查复核 #11）：多路样本合并时，窗口必须装得下**所有源**，
    //   否则后吸的会把先吸的整段挤掉 —— 3 路各 4096 吸进默认 4096 窗口后，窗口里只剩最后一路
    //   worker 的样本，于是 e2e_p95/p99「名义全局、实际单路」。调用方按 kMaxSamples × worker_count
    //   构造即可。默认构造仍是单源滚动窗口，内存上界不变（S6 的修复意图保持）。
    explicit StatsCollector(size_t capacity) : capacity_(capacity < 1 ? kMaxSamples : capacity) {
        samples_.reserve(capacity_);
    }

    // 追加一个耗时样本（us）；窗口满后环形覆盖最旧样本。
    void add(uint64_t us) {
        std::lock_guard<std::mutex> lock(mutex_);
        append_locked(us);
    }

    // 清空全部样本，窗口游标复位。
    void clear() {
        std::lock_guard<std::mutex> lock(mutex_);
        samples_.clear();
        head_ = 0;
    }

    void absorb(const StatsCollector& other) {
        if (this == &other) return;
        // C++17 scoped_lock 一次取两把锁：内部走 std::lock 的统一算法。
        // 原写法是「先 this 后 other」两个 lock_guard，一旦将来出现反向调用
        // （a.absorb(b) 与 b.absorb(a) 并发）就是经典 AB-BA 死锁；此处零成本根除该隐患。
        std::scoped_lock lock(mutex_, other.mutex_);
        if (head_ == 0 && samples_.size() + other.samples_.size() <= capacity_) {
            // 快路径：本端仍是顺序未满窗口，直接追加不越界。
            samples_.insert(samples_.end(), other.samples_.begin(), other.samples_.end());
            return;
        }
        for (const uint64_t v : other.samples_) append_locked(v);
    }

    // 当前窗口内的有效样本数。
    size_t count() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return samples_.size();
    }

    // 最小样本（us）；无样本返回 0。
    uint64_t min() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return samples_.empty() ? 0 : *std::min_element(samples_.begin(), samples_.end());
    }

    // 样本均值（us）；无样本返回 0。
    double avg() const {
        std::lock_guard<std::mutex> lock(mutex_);
        if (samples_.empty()) return 0.0;
        const uint64_t total = std::accumulate(samples_.begin(), samples_.end(), uint64_t{0});
        return static_cast<double>(total) / static_cast<double>(samples_.size());
    }

    // 最大样本（us）；无样本返回 0。
    uint64_t max() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return samples_.empty() ? 0 : *std::max_element(samples_.begin(), samples_.end());
    }

    // p 分位（0~100，线性插值）；无样本返回 0。
    uint64_t percentile(double p) const {
        std::lock_guard<std::mutex> lock(mutex_);
        if (samples_.empty()) return 0;
        std::vector<uint64_t> sorted = samples_;
        std::sort(sorted.begin(), sorted.end());
        if (p <= 0.0) return sorted.front();
        if (p >= 100.0) return sorted.back();
        const double index = (static_cast<double>(sorted.size()) - 1.0) * (p / 100.0);
        const size_t lower = static_cast<size_t>(index);
        const size_t upper = std::min(lower + 1, sorted.size() - 1);
        const double fraction = index - static_cast<double>(lower);
        return static_cast<uint64_t>(static_cast<double>(sorted[lower]) +
                                     fraction * static_cast<double>(sorted[upper] - sorted[lower]));
    }

private:
    // 调用方必须已持有 mutex_。未满顺序追加；满后环形覆盖最旧样本。
    // 上界用 capacity_（默认 kMaxSamples；合并容器按构造参数放大）。
    void append_locked(uint64_t us) {
        if (samples_.size() < capacity_) {
            samples_.push_back(us);
            return;
        }
        samples_[head_] = us;
        head_ = (head_ + 1) % capacity_;
    }

    // 本实例的窗口容量：单源默认 4096；合并容器由调用方按 kMaxSamples × 源数指定。
    const size_t capacity_ = kMaxSamples;

    mutable std::mutex mutex_;
    std::vector<uint64_t> samples_;
    size_t head_ = 0;  // 窗口满后的写入游标（指向最旧样本）
};

}  // namespace ttbox::core
