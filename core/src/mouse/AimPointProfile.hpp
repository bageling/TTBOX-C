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
//   clipped_h_over_w：调用方按目标自校准的「身高/框宽」比（>0 才用）。<=0 = 用配置兜底。
//   输出 tx/ty = 目标框内瞄准点（像素，crop 系）。
// 返回 false 仅当 box 无效。
// V1.0.08/09：框底贴到裁剪区下边界时（近身目标下半身在 crop 之外），可见框高被截断，
//   按它算的落点会相对人体上飘 ⇒ 此时用框宽 × 身高宽比反推完整身高。
//   比值优先取调用方自校准值（同一目标最近一次未截断帧的 h/w，见 AimThread），
//   没有时退回 prof.body_w_over_h（兜底，默认偏保守）。
bool aim_point_at(const DetectionBox& box, int class_id, const AimPointProfile& prof,
                  float* tx, float* ty, float crop_bottom_px = -1.0f,
                  float clipped_h_over_w = 0.0f);

// 获取 class_id 命中的类偏移（按 priority 最高）；无命中返回默认。
void class_offset_for(const AimPointProfile& prof, int class_id,
                      float* offset_x, float* offset_y);

// 头部瞄准约束（第3项）：把瞄准点 (tx,ty) 约束到头区内部安全区（若启用且瞄头）。
//   box：目标框（crop 系）；prof：瞄准点配置（含 head_aim）
//   返回是否发生了约束（false = 未启用/瞄身体/无需约束）。
bool constrain_aim_point_to_head(const DetectionBox& box, const AimPointProfile& prof,
                                 float* tx, float* ty);

}  // namespace ttbox::core::aim
