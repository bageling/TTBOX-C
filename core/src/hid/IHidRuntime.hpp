// IHidRuntime.hpp — A9-P2 HID Runtime 抽象接口
//
// AI Runtime 与 HID 解耦：AI Runtime 只通过本接口访问 HID 状态，
// 禁止直接访问 /dev/hidraw*、/dev/hidg*、/sys/kernel/config/usb_gadget/。
#pragma once

#include <cstdint>
#include <string>

#include "hid/HidTypes.hpp"

namespace ttbox::core {

// HID Runtime 状态
enum class HidRuntimeStatus : int {
    kStopped = 0,
    kRunning = 1,
    kError = 2,
};

const char* hid_runtime_status_name(HidRuntimeStatus s);

// 运行时指标（供 AI Runtime 只读）
struct HidRuntimeMetrics {
    uint64_t rx_reports = 0;
    uint64_t tx_reports = 0;
    uint64_t drop = 0;
    uint64_t backpressure = 0;
    double latency_avg_us = 0.0;
    uint64_t latency_p50_us = 0;
    uint64_t latency_p95_us = 0;
    uint64_t latency_p99_us = 0;
    double report_rate_hz = 0.0;
    uint64_t max_queue_depth = 0;
    HidRuntimeStatus status = HidRuntimeStatus::kStopped;
};

// HID Runtime 接口（AI Runtime 的唯一边界）
class IHidRuntime {
public:
    virtual ~IHidRuntime() = default;

    // 启动（配置来自 HID Package 独立配置）
    virtual bool start(std::string* error = nullptr) = 0;
    // 停止（释放 hidraw/hidg/线程）
    virtual void stop() = 0;
    // 当前状态
    virtual HidRuntimeStatus status() const = 0;
    // 最近鼠标状态（HidParser 解析；无事件返回空状态）
    virtual MouseState get_mouse_state() const = 0;
    // 最近键盘状态
    virtual KeyboardState get_keyboard_state() const = 0;
    // 运行时指标（只读快照）
    virtual HidRuntimeMetrics get_metrics() const = 0;
};

}  // namespace ttbox::core
