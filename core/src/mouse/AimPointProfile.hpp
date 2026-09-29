// AimPointProfile.hpp — A10 瞄准点计算
//
// 目标基础点 = 框中心 + offset × 框尺寸（crop 坐标系）。
// class_offsets 按 class_id + priority 覆盖默认 offset（不同模型允许不同 profile）。
// 禁止把 aim point 写死在模型 Adapter。
#pragma once

#include <cstdint>
#include <vector>

#include "common/Types.hpp"
#include "mouse/MouseTypes.hpp"

namespace ttbox::core::aim {

// 计算瞄准点（crop 坐标系）。
//   box：目标框（crop 系）；class_id：目标类别
//   prof：瞄准点配置（默认 offset + class_offsets）
//   crop_bottom_px：裁剪区下边界（与 box 同一坐标系）。<=0 = 未知 ⇒ 不做截断外推。
//   输出 tx/ty = 目标框内瞄准点（像素，crop 系）。
// 返回 false 仅当 box 无效。
// V1.0.08：框底贴到裁剪区下边界时（近身目标下半身在 crop 之外），可见框高被截断，
//   按它算的落点会相对人体上飘 ⇒ 此时用框宽反推完整身高（见 AimPointProfile 同名注释）。
bool aim_point_at(const DetectionBox& box, int class_id, const AimPointProfile& prof,
                  float* tx, float* ty, float crop_bottom_px = -1.0f);

// 获取 class_id 命中的类偏移（按 priority 最高）；无命中返回默认。
void class_offset_for(const AimPointProfile& prof, int class_id,
                      float* offset_x, float* offset_y);

// 头部瞄准约束（第3项）：把瞄准点 (tx,ty) 约束到头区内部安全区（若启用且瞄头）。
//   box：目标框（crop 系）；prof：瞄准点配置（含 head_aim）
//   返回是否发生了约束（false = 未启用/瞄身体/无需约束）。
bool constrain_aim_point_to_head(const DetectionBox& box, const AimPointProfile& prof,
                                 float* tx, float* ty);

}  // namespace ttbox::core::aim
