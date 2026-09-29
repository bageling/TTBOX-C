// AimPointProfile.cpp — A10 瞄准点计算实现
#include "mouse/AimPointProfile.hpp"

#include <algorithm>

namespace ttbox::core::aim {

namespace {
// V1.0.08：贴裁剪区下边界时，身高最多按可见框高的多少倍外推。
// 取 3.0 是给极端近身留余量；正常情况由框宽约束，这个上限只防异常框。
constexpr float kClipBottomMaxStretch = 3.0f;
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
                  float* tx, float* ty, float crop_bottom_px) {
    const float w = box.x2 - box.x1;
    float h = box.y2 - box.y1;
    if (w <= 0.0f || h <= 0.0f) return false;
    float ox = prof.offset_x;
    float oy = prof.offset_y;
    class_offset_for(prof, class_id, &ox, &oy);
    // V1.0.08：框底贴到裁剪区下边界 ⇒ 下半身在 crop 之外，可见框高偏小 ⇒ 落点相对人体
    // 上飘（越近越严重）。此时改用**框宽**反推身高：宽度不随纵向裁剪失真，而人体框
    // 宽/高 在板端三批记录里都稳定在 0.32（p10-p90 = 0.30~0.34）。
    // 只放大不缩小，且最多放大 kClipBottomMaxStretch 倍（防蹲姿/异常框把落点推到脚下）。
    if (prof.clip_bottom_extrapolate && crop_bottom_px > 0.0f && prof.body_w_over_h > 0.05f &&
        box.y2 >= crop_bottom_px - prof.clip_bottom_margin_px) {
        const float h_from_w = w / prof.body_w_over_h;
        const float h_cap = h * kClipBottomMaxStretch;
        h = std::min(std::max(h_from_w, h), h_cap);
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

}  // namespace ttbox::core::aim
