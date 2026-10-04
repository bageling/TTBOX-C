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

// 几何配对识别「头部小框」（不依赖 class_id）：
//   在 dets 里找「被 ref（身体框）包住、面积明显更小、中心落在 ref 上半部」的框 = 头。
//   命中返回 true 并写 head；找不到（模型没单独出头框 / 只有一个框）返回 false。
//   判据写死（不进面板，对齐"精细量只遥测不暴露"的口径）：
//     · 面积 ≤ ref 面积 × 0.5（明显更小，头远小于身体）
//     · 中心 x 落在 ref 水平范围、中心 y 落在 ref 上半部（头在身体上端）
//     · 越出 ref 的部分不超过自身宽/高的 40%（大体被包住，容忍框抖动）
//   多个候选时取面积最小者（最像头）。
bool resolve_head_box(const DetectionBox& ref, const std::vector<DetectionBox>& dets,
                      DetectionBox* head);

// V1.0.24：上半身收缩（无条件生效；比例是算法常量，不进配置/面板）。
//   把控制链用的框与瞄准点配置**一起**映射到上半身域：
//     · out_box = (x1, y1, x2, y1 + k·h)      —— x 不动
//     · out_prof.offset_y /= k                 —— 落点物理位置不变（等效换算）
//     · out_prof.class_offsets 每项 offset_y /= k
//     · out_prof.head_aim 两个 fraction /= k
//     · out_prof.body_w_over_h /= k            —— 外推兜底在收缩域推出"完整上半身高"
//   调用方把 out_box/out_prof 喂给整条控制链（clip 判定/冻结/自校准/落点/框高）；
//   显示框直接用 out_box（不再做多框并集 —— 那正是"框看起来是全身加头"的来源）。
//   框无效 ⇒ 原样返回（调用方走原框）。
bool shrink_to_upper_body(const DetectionBox& box, const AimPointProfile& prof,
                          DetectionBox* out_box, AimPointProfile* out_prof);

// 上半身占全身框高的比例（算法常量，见 AimPointProfile.cpp 的取值依据）。
float upper_body_ratio();

}  // namespace ttbox::core::aim
