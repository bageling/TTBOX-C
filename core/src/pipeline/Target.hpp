// Target.hpp — 第13阶段：目标选择与坐标转换的正式数据结构
//
// 模块边界（第13阶段）：
//   Detector ──Detection[]──▶ TargetSelector ──Target──▶ Coordinate ──TargetPoint──▶ Controller
//
// 本文件只定义"层间传递的数据结构"，不含任何逻辑：
//   - Target      ：TargetSelector（目标选择器）的输出，代表"选中的目标"
//   - TargetPoint ：Coordinate（坐标转换）的输出，代表"真正用于控制的目标点"
//
// 依赖规则：本文件不依赖 RKNN/V4L2/RGA/Mouse/HID/Web，任何模块都可安全包含。
// 说明：Detection / MouseCommand 已定义在 common/CoreContracts.hpp，本文件不再重复定义。
#pragma once

#include <cstdint>

#include "common/Types.hpp"

namespace ttbox::core::aim {

// 目标（Target）：TargetSelector 最终选中的目标。
// 由 Detection[]（检测框列表）→ 选择规则（距离最近/类别过滤/连续帧稳定）得出。
struct Target {
    bool valid = false;          // 是否有效（无目标时为 false）
    int class_id = 0;            // 目标类别编号（COCO：0=人 2=汽车 5=公交车）
    float confidence = 0.0f;     // 目标置信度（0~1）
    DetectionBox box{};          // 目标框（crop/帧坐标系，与原 Detection 相同坐标系）
    float center_x = 0.0f;       // 框中心 x（原图/帧坐标）
    float center_y = 0.0f;       // 框中心 y
    int target_id = -1;          // 稳定追踪 id（跨帧跟踪用，-1 = 未分配）
};

// 目标点（TargetPoint）：Coordinate（坐标转换）的输出。
// 这是"真正用于控制的目标点"——由 Target 框 + 瞄准点配置（offset）换算而来。
// Controller（控制器）只消费 TargetPoint，不接触 Detection/RKNN 任何细节。
struct TargetPoint {
    bool valid = false;          // 是否有效
    float x = 0.0f;              // 目标点 x（帧坐标，像素）
    float y = 0.0f;              // 目标点 y
};

}  // namespace ttbox::core::aim
