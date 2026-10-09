// AimPointProfile.cpp — A10 瞄准点计算实现
#include "mouse/AimPointProfile.hpp"

#include <algorithm>

namespace ttbox::core::aim {

namespace {
// V1.0.08：贴裁剪区下边界时，身高最多按可见框高的多少倍外推。
// 取 3.0 是给极端近身留余量；正常情况由框宽约束，这个上限只防异常框。
constexpr float kClipBottomMaxStretch = 3.0f;
// 身高宽比的合理区间（挡异常框 / 退化框，如 w=h 的近方形框外推出 3 倍身高）。
constexpr float kMinHOverW = 0.8f;
constexpr float kMaxHOverW = 12.0f;
}  // namespace

void class_offset_for(const AimPointProfile& prof, int class_id,
                      float* offset_x, float* offset_y) {
    const ClassOffset* best = nullptr;
    for (const auto& c : prof.class_offsets) {
        if (c.class_id != class_id) continue;
        if (!best || c.priority > best->priority) best = &c;
    }
    if (best) {
        *offset_x = best->offset_x;
        *offset_y = best->offset_y;
    } else {
        *offset_x = prof.offset_x;
        *offset_y = prof.offset_y;
    }
}

bool aim_point_at(const DetectionBox& box, int class_id, const AimPointProfile& prof,
                  float* tx, float* ty, float crop_bottom_px, float clipped_h_over_w) {
    const float w = box.x2 - box.x1;
    float h = box.y2 - box.y1;
    if (w <= 0.0f || h <= 0.0f) return false;
    float ox = prof.offset_x;
    float oy = prof.offset_y;
    class_offset_for(prof, class_id, &ox, &oy);
    // V1.0.08/09：框底贴到裁剪区下边界 ⇒ 下半身在 crop 之外，可见框高偏小 ⇒ 落点相对
    // 人体上飘（越近截得越多，所以是「走进目标才飘」而不是「一直偏」）。
    // 定障实测（板端常驻记录器 + 离线复算，2026-09-29）：
    //   · 裁剪区 = 画面中心 416x416 ⇒ 下边界 = 720 + 208 = **928**（cls5 原始框 y2 在
    //     928 出现巨峰 102 帧，且「走近」段选中框 y2 从 832 一路涨到 928 就钉死不动、
    //     而 y1 继续上冒 ⇒ 框底被截）。
    //   · 选中框贴到 928 的帧占 5.8%（351/6069 对齐帧）；这些帧落点相对真实胸口
    //     上飘 p50≈65px、p90≈173px、最大≈240px —— 在 300~400px 高的身体上就是 20%，
    //     0.31 的胸口位直接变成 0.10 的头部（= 业主症状「走进目标就跑到头上」）。
    // 修法：用**框宽**反推身高（宽度不随纵向裁剪失真）。
    //   ★ 比值优先取调用方自校准值 clipped_h_over_w（同一目标最近一次未截断帧的 h/w）——
    //     本模型 cls5 的框宽高比在 0.29~0.52 之间漂（远距离常只框上半身），写死会过度
    //     修正；同一目标的比值按距离等比缩放，最稳。没有自校准值时退回配置兜底。
    //   只放大不缩小，且最多放大 kClipBottomMaxStretch 倍（防蹲姿/异常框把落点推到脚下）。
    if (prof.clip_bottom_extrapolate && crop_bottom_px > 0.0f &&
        box.y2 >= crop_bottom_px - prof.clip_bottom_margin_px) {
        float h_over_w = clipped_h_over_w;
        if (!(h_over_w >= kMinHOverW && h_over_w <= kMaxHOverW)) {
            h_over_w = prof.body_w_over_h > 0.05f ? 1.0f / prof.body_w_over_h : 0.0f;
        }
        if (h_over_w >= kMinHOverW && h_over_w <= kMaxHOverW) {
            const float h_from_w = w * h_over_w;
            const float h_cap = h * kClipBottomMaxStretch;
            h = std::min(std::max(h_from_w, h), h_cap);
        }
    }
    *tx = box.x1 + ox * w;
    *ty = box.y1 + oy * h;
    return true;
}

bool constrain_aim_point_to_head(const DetectionBox& box, const AimPointProfile& prof,
                                 float* tx, float* ty) {
    const HeadAimConfig& cfg = prof.head_aim;
    if (!cfg.enabled) return false;
    // 只对"瞄头"生效（默认 offset_y < 0.5 = 框上半部 = 头部方向）
        float class_oy = prof.offset_y;
        float class_ox_tmp = prof.offset_x;  // class_offset_for 会写 offset_x，需传非空
        class_offset_for(prof, box.class_id, &class_ox_tmp, &class_oy);  // 取类偏移
        if (class_oy >= 0.5f) return false;  // 瞄身体：不做头约束

    const float w = box.x2 - box.x1;
    const float h = box.y2 - box.y1;
    if (w <= 0.0f || h <= 0.0f) return false;

    // 估算头区（body 框顶部一段）
    const float head_top = box.y1 + cfg.head_offset_top_fraction * h;
    const float head_bottom = box.y1 + (cfg.head_offset_top_fraction + cfg.head_height_fraction) * h;
    if (head_bottom <= head_top) return false;

    // 头区安全内缩（safe inset）
    const float inset_y = (head_bottom - head_top) * cfg.safe_inset_fraction;
    const float inset_x = w * cfg.safe_inset_fraction;
    float safe_y1 = head_top + inset_y;
    float safe_y2 = head_bottom - inset_y;
    float safe_x1 = box.x1 + inset_x;
    float safe_x2 = box.x2 - inset_x;
    if (safe_y1 > safe_y2) { const float c = (safe_y1 + safe_y2) * 0.5f; safe_y1 = safe_y2 = c; }
    if (safe_x1 > safe_x2) { const float c = (safe_x1 + safe_x2) * 0.5f; safe_x1 = safe_x2 = c; }

    // 锚点滞后钳制（限制单帧最大移动，防瞄准点大幅跳变出安全区）
    // max_lag = min(max_lag_px, 头高 × max_lag_fraction)
    const float lag_y = std::min(cfg.max_lag_px, (head_bottom - head_top) * cfg.max_lag_fraction);
    const float lag_x = std::min(cfg.max_lag_px, w * cfg.max_lag_fraction);
    const float anchor_x = box.x1 + w * 0.5f;  // 头中心 x（头宽≈框宽）
    const float anchor_y = (head_top + head_bottom) * 0.5f;
    float lo_x = std::max(safe_x1, anchor_x - lag_x);
    float hi_x = std::min(safe_x2, anchor_x + lag_x);
    float lo_y = std::max(safe_y1, anchor_y - lag_y);
    float hi_y = std::min(safe_y2, anchor_y + lag_y);
    if (lo_x > hi_x) { const float c = (lo_x + hi_x) * 0.5f; lo_x = hi_x = c; }
    if (lo_y > hi_y) { const float c = (lo_y + hi_y) * 0.5f; lo_y = hi_y = c; }

    const float x0 = *tx, y0 = *ty;
    *tx = std::max(lo_x, std::min(hi_x, *tx));
    *ty = std::max(lo_y, std::min(hi_y, *ty));
    return (*tx != x0) || (*ty != y0);
}

// ★★ V1.0.31（2026-10-04）：**框裁小这件事整体退役**。
//
// 业主定调：「框完整，只偏移落点」+「框的大小不该成为问题，落点才是目的」。
// 调研对照（GitHub 上流传最广的 sunone_aimbot，类别定义是事实标准）：
//   class 0 = player（人）/ 1,7 = head / 2 = weapon / 4 = dead_body
//   class 5 = 训练场人形靶 / **6 = 训练场的球**
//   它的落点做法是 `body_y_offset` —— **在身体框内做 y 偏移**，不是把框裁小：
//   框画完整（能看到目标全貌、调试时不误导），落点才落在胸口。
//
// 为什么不裁（V1.0.24~V1.0.30 三版裁框踩到的坑）：
//   ① 近身时框底被 640×640 画面切掉 ⇒ 可见框高残缺 ⇒ 肩宽算小 ⇒ 框越收越窄；
//   ② 落点被「等效换算」强行拉住 ⇒ 画面上看着"框不随远近变化"（实测宽高几乎不变）；
//   ③ 框不再等于「目标在哪」⇒ 预览失去调试价值。
//
// 落点公式**不变**：ty = 框顶 + offset_y × 框高，offset_y 默认 0.24（身体框 24% ≈ 胸口）。
// 排除「非人目标」改由**几何 + 类别筛选**在选靶层做（见 TargetSelector 的几何兜底）。

}  // namespace ttbox::core::aim
