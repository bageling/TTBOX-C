// HidForwarder.hpp — A9 HID 透传转发器（RAW 优先）
//
// 链路（独立于 AI 推理）：
//   USB Host hidraw → [RX 线程] → SPSC 队列 → [TX 线程] → USB HID Gadget (hidg)
//
// 设计：
//   - 两个独立线程（RX/TX），不等待 NPU/RGA/V4L2/Decode/Web/JSON
//   - lock-free SPSC 队列（固定容量，无 malloc/free）
//   - 每个 report 带 monotonic timestamp（RX 接收时刻）
//   - 默认关闭逐事件日志；只累计统计
//   - CPU affinity 可选（pthread_setaffinity_np），A9 实验矩阵用
#pragma once

#include <atomic>
#include <cstdint>
#include <memory>
#include <string>
#include <thread>

#include "common/Stats.hpp"
#include "hid/HidTypes.hpp"
#include "hid/SpscQueue.hpp"

namespace ttbox::core {

// 转发统计（原子，跨线程安全）
struct HidForwardStats {
    std::atomic<uint64_t> rx_reports{0};        // RX 读取成功
    std::atomic<uint64_t> tx_reports{0};        // TX 写入成功
    std::atomic<uint64_t> rx_errors{0};         // RX read 错误
    std::atomic<uint64_t> tx_errors{0};         // TX write 错误（非背压）
    std::atomic<uint64_t> tx_backpressure{0};   // TX write 背压丢弃（EAGAIN/ENODEV）
    std::atomic<uint64_t> push_drops{0};        // 队列满丢弃（生产者）
    std::atomic<uint64_t> bad_size{0};          // 非法 report 长度
    StatsCollector latency_us;                  // 队列驻留+TX 耗时（us）
    StatsCollector rx_interval_us;              // RX 相邻 report 间隔（us，估算回报率）
    std::atomic<uint64_t> max_queue_depth{0};   // 队列峰值深度
};

class HidForwarder {
public:
    struct Params {
        std::string hidraw_path;   // 输入 /dev/hidrawX（真实键鼠）
        std::string hidg_path;     // 输出 /dev/hidg0（键盘）或 /dev/hidg1（鼠标）
        HidKind kind = HidKind::kUnknown;  // 用于日志/分类（RAW 透传不重编码）
        int cpu = -1;              // RX/TX 线程绑核（-1=默认调度）
        size_t queue_capacity = 1024;  // 队列容量（2 的幂）
        bool raw_pass = true;      // RAW 直接透传（默认）；false 时按 kind 重编码
    };

    HidForwarder() = default;
    ~HidForwarder();

    // 启动 RX+TX 线程。失败返回 false（如 hidraw/hidg 无法打开）。
    bool start(const Params& params, std::string* error = nullptr);
    void stop();

    bool running() const { return running_.load(); }
    // 只读访问转发统计（各计数为原子，StatsCollector 内部加锁）
    const HidForwardStats& stats() const { return stats_; }

private:
    // RX 线程主体：读 hidraw 并入队
    void rx_loop();
    // TX 线程主体：出队并写 hidg
    void tx_loop();
    // 当前单调时钟（微秒）
    uint64_t now_us() const;
    // 把线程绑定到 cpu（cpu<0 跳过）
    static bool set_affinity(std::thread& t, int cpu);
    // 重编码为 gadget 期望的固定格式（返回 false 表示应丢弃，如 consumer/system 报告）
    bool reencode(const HidReport& rep, uint8_t* out, size_t* out_size);

    Params params_;
    std::atomic<bool> running_{false};
    std::thread rx_thread_;
    std::thread tx_thread_;
    std::unique_ptr<SpscQueue<HidReport, 1024>> queue_;  // 容量由 Params 归一化为 1024
    HidForwardStats stats_;
    uint64_t last_rx_us_ = 0;  // 上一份 report 的 RX 时刻（仅 RX 线程访问），用于估算回报率
};

}  // namespace ttbox::core
