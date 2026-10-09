// NpuMonitor.hpp — NPU Core 利用率采集（/sys/kernel/debug/rknpu/load）
#pragma once

#include <atomic>
#include <cstdint>
#include <mutex>
#include <thread>

namespace ttbox::core {

// 单次采样结果（%）
struct NpuLoadSample {
    float core0 = 0.0f;
    float core1 = 0.0f;
    float core2 = 0.0f;
    bool ok = false;
};

// 采样汇总（平均）
struct NpuLoadSummary {
    float core0 = 0.0f;
    float core1 = 0.0f;
    float core2 = 0.0f;
    size_t samples = 0;
};

// NPU 三核利用率监测器：单次读取 + 后台周期采样并累计平均。
class NpuMonitor {
public:
    NpuMonitor() = default;
    ~NpuMonitor() { stop(); }
    NpuMonitor(const NpuMonitor&) = delete;
    NpuMonitor& operator=(const NpuMonitor&) = delete;

    // 单次读取 /sys/kernel/debug/rknpu/load
    static NpuLoadSample read_once();

    // 后台采样线程（周期 interval_ms）
    bool start(int interval_ms = 200, std::string* error = nullptr);
    void stop();
    bool running() const { return running_.load(); }

    NpuLoadSummary summary() const;

private:
    void sample_loop();

    std::atomic<bool> running_{false};
    std::thread thread_;
    mutable std::mutex mutex_;
    float sum0_ = 0.0f, sum1_ = 0.0f, sum2_ = 0.0f;
    size_t samples_ = 0;
};

}  // namespace ttbox::core
