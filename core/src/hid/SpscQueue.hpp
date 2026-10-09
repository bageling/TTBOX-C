// SpscQueue.hpp — A9 lock-free SPSC 环形队列（HID RX → HID TX）
//
// 设计：
//   - 单生产者单消费者（HID RX 线程 push，HID TX 线程 pop）
//   - 无锁（sequence numbers + acquire/release），无 malloc/free
//   - 固定容量 N（容量不足时 try_push 失败 → 生产者计数 drop）
//   - T 必须为平凡可拷贝（HidReport 满足）
#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>

namespace ttbox::core {

template <typename T, size_t N>
class SpscQueue {
    static_assert(N > 0 && (N & (N - 1)) == 0, "N 必须为 2 的幂");

public:
    // 队列固定容量（编译期常量，2 的幂）
    static constexpr size_t capacity() { return N; }

    // 构造：初始化各槽位序列号（Vyukov SPSC 约定）
    SpscQueue() {
        // Vyukov SPSC：每个槽位预填期望序列号 seq[i]=i
        for (size_t i = 0; i < N; ++i) seq_[i].store(static_cast<uint64_t>(i));
    }

    // 生产者：失败返回 false（队列满，调用方计数 drop）
    bool try_push(const T& item) {
        const uint64_t pos = head_.load(std::memory_order_relaxed);
        const size_t idx = static_cast<size_t>(pos & (N - 1));
        const uint64_t cur = seq_[idx].load(std::memory_order_acquire);
        // 槽位可用条件：seq == pos（写入轮次对齐）
        if (cur != pos) return false;
        data_[idx] = item;
        seq_[idx].store(pos + 1, std::memory_order_release);  // 发布数据
        head_.store(pos + 1, std::memory_order_release);
        return true;
    }

    // 消费者：失败返回 false（队列空）
    bool try_pop(T* out) {
        const uint64_t pos = tail_.load(std::memory_order_relaxed);
        const size_t idx = static_cast<size_t>(pos & (N - 1));
        const uint64_t cur = seq_[idx].load(std::memory_order_acquire);
        // 已发布条件：seq == pos+1（上一轮写入完成）
        if (cur != pos + 1) return false;
        *out = data_[idx];
        seq_[idx].store(pos + N, std::memory_order_release);  // 释放槽位给下一轮
        tail_.store(pos + 1, std::memory_order_release);
        return true;
    }

    // 队列是否为空（近似快照，仅供判空/统计，不作同步屏障）
    bool empty() const {
        return head_.load(std::memory_order_relaxed) == tail_.load(std::memory_order_relaxed);
    }
    // 当前元素数（近似快照，供峰值深度统计）
    size_t size() const {
        const uint64_t h = head_.load(std::memory_order_relaxed);
        const uint64_t t = tail_.load(std::memory_order_relaxed);
        return static_cast<size_t>(h - t);
    }

private:
    std::array<std::atomic<uint64_t>, N> seq_;
    std::array<T, N> data_;
    std::atomic<uint64_t> head_{0};  // 生产者
    std::atomic<uint64_t> tail_{0};  // 消费者
};

}  // namespace ttbox::core
