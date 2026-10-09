// PipelineDebug.hpp — 第13阶段：链路诊断采样器
//
// 用途：开启后，每 N 帧输出一次完整链路状态（不每帧刷日志，不拖慢实时链路）：
//   [PIPELINE] Frame / Detector / Target / Box / TargetPoint / Controller / MouseCommand / Mouse
//
// 以后出现"识别到了，但鼠标偏了"，直接看这一行就知道是哪一层出了问题。
// 设计：纯 C++ 结构体 + 采样计数，不依赖 Web/IPC；由 AimThread 在每帧更新时喂数据。
#pragma once

#include <cstdint>
#include <cstdio>
#include <vector>

#include "common/CoreContracts.hpp"
#include "pipeline/Target.hpp"

namespace ttbox::core::aim {

// 链路诊断采样器：每 sample_interval 帧输出一次完整链路状态。
// 默认关闭（enabled=false）；开启后不改变任何控制行为，只增加低频打印。
struct PipelineDebug {
    bool enabled = false;          // 总开关（默认关闭）
    uint32_t sample_interval = 60; // 每 N 帧输出一次（默认 60 ≈ 1秒@60fps）
    uint32_t frame_counter = 0;    // 帧计数（内部）

    // 一帧完整链路快照（由 AimThread 填充后调用 sample()）
    struct Snapshot {
        uint64_t frame_number = 0;
        uint32_t frame_w = 0;
        uint32_t frame_h = 0;
        size_t detections = 0;           // 本帧检测框数量
        Target target{};                 // TargetSelector 输出
        TargetPoint point{};             // Coordinate 输出
        float error_x = 0.0f;            // Controller 输入误差（遥测）
        float error_y = 0.0f;
        MouseCommand command{};          // Controller 输出（MouseCommand）
        bool mouse_disabled = true;      // 安全状态（本阶段恒 true）
    };

    // 喂入一帧快照；若到达采样周期则打印并返回 true，否则返回 false。
    // 打印到 stderr（不影响 stdout 数据流）。
    bool sample(const Snapshot& s) {
        if (!enabled) return false;
        ++frame_counter;
        if (frame_counter % sample_interval != 0) return false;
        print(s);
        return true;
    }

private:
    static void print(const Snapshot& s) {
        std::fprintf(stderr,
            "[PIPELINE] frame=%llu size=%ux%u\n"
            "  Detector: detections=%zu\n"
            "  Target: valid=%d class=%d conf=%.3f id=%d center=(%.1f,%.1f)\n"
            "  Box: x1=%.1f y1=%.1f x2=%.1f y2=%.1f\n"
            "  TargetPoint: valid=%d x=%.1f y=%.1f\n"
            "  Controller: error=(%.1f,%.1f) dx=%d dy=%d valid=%d\n"
            "  Mouse: %s\n",
            static_cast<unsigned long long>(s.frame_number), s.frame_w, s.frame_h,
            s.detections,
            s.target.valid ? 1 : 0, s.target.class_id, s.target.confidence,
            s.target.target_id, s.target.center_x, s.target.center_y,
            s.target.box.x1, s.target.box.y1, s.target.box.x2, s.target.box.y2,
            s.point.valid ? 1 : 0, s.point.x, s.point.y,
            s.error_x, s.error_y, s.command.dx, s.command.dy, s.command.valid ? 1 : 0,
            s.mouse_disabled ? "DISABLED" : "ENABLED");
    }
};

}  // namespace ttbox::core::aim
