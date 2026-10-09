// AimTargetTask.hpp
#pragma once
#include <cstdint>
#include <vector>
#include "common/Types.hpp"
namespace ttbox::core::aim {
// 瞄准点（帧坐标，像素）
struct AimPoint { float x = 0.0f; float y = 0.0f; };
// 单帧瞄准任务快照：Worker 产出的检测/瞄准结果，供 AimThread 只读消费
struct AimTargetTask {
    uint64_t frame_number = 0;
    uint64_t timestamp_us = 0;
    int worker_id = -1;
    uint32_t frame_width = 0;
    uint32_t frame_height = 0;
    bool has_target = false;
    DetectionBox target{};
    AimPoint aim_point{};
    float target_width = 0.0f;
    float target_height = 0.0f;
    bool crosshair_detected = false;
    AimPoint crosshair{};
    // 准星找色（急停检测）结果：由**有帧的一侧**（推理 worker）算好带过来。
    // AimThread 拿不到像素，只能消费这个 bool。未开启时恒 false（= 没检测到要停手）。
    bool stop_detect_hit = false;
    std::vector<DetectionBox> detections;
};
}
