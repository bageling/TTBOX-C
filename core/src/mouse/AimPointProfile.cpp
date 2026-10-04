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

bool resolve_head_box(const DetectionBox& ref, const std::vector<DetectionBox>& dets,
                      DetectionBox* head) {
    if (!head) return false;
    const float rw = ref.x2 - ref.x1;
    const float rh = ref.y2 - ref.y1;
    if (rw <= 0.0f || rh <= 0.0f) return false;
    const float r_area = rw * rh;
    // 头部小框面积上限（相对身体）：头远小于身体，取 0.5 已很宽松。
    constexpr float kAreaMaxRatio = 0.5f;
    // 头框中心必须落在身体框上半部（头在身体上端，不是腿/躯干下部）。
    constexpr float kHeadUpperFraction = 0.5f;
    // 越出身体框的部分占自身宽/高的比例上限（容忍检测框抖动/边缘越界）。
    constexpr float kContainSlack = 0.4f;

    DetectionBox best;
    float best_area = 0.0f;
    bool found = false;
    for (const auto& d : dets) {
        const float dw = d.x2 - d.x1;
        const float dh = d.y2 - d.y1;
        if (dw <= 0.0f || dh <= 0.0f) continue;
        // 跳过 ref 本身（坐标完全一致；detections 里 ref 也在）
        if (d.x1 == ref.x1 && d.y1 == ref.y1 && d.x2 == ref.x2 && d.y2 == ref.y2) continue;
        const float d_area = dw * dh;
        if (d_area >= r_area * kAreaMaxRatio) continue;  // 不是"明显更小"
        const float dcx = (d.x1 + d.x2) * 0.5f;
        const float dcy = (d.y1 + d.y2) * 0.5f;
        if (dcx < ref.x1 || dcx > ref.x2) continue;      // 头框中心在身体框水平范围外
        if (dcy < ref.y1 || dcy > ref.y1 + rh * kHeadUpperFraction) continue;  // 不在上半部
        const float ox = std::max(0.0f, ref.x1 - d.x1) + std::max(0.0f, d.x2 - ref.x2);
        const float oy = std::max(0.0f, ref.y1 - d.y1) + std::max(0.0f, d.y2 - ref.y2);
        if (ox > dw * kContainSlack || oy > dh * kContainSlack) continue;  // 没被包住
        if (!found || d_area < best_area) {
            best = d;
            best_area = d_area;
            found = true;
        }
    }
    if (found) *head = best;
    return found;
}

float upper_body_shrink_ratio(const AimPointProfile& prof) {
    // 有效域 (0.05, 1.0]；越界（含 0/负/>1）一律当 1.0（不收缩）—— fail-closed。
    if (!prof.upper_body_enabled) return 1.0f;
    const float k = prof.upper_body_ratio;
    if (!(k > 0.05f && k <= 1.0f)) return 1.0f;
    return k;
}

bool shrink_to_upper_body(const DetectionBox& box, const AimPointProfile& prof,
                          DetectionBox* out_box, AimPointProfile* out_prof) {
    if (!out_box || !out_prof) return false;
    *out_box = box;
    *out_prof = prof;
    const float w = box.x2 - box.x1;
    const float h = box.y2 - box.y1;
    if (w <= 0.0f || h <= 0.0f) return false;  // 框无效：不收缩（调用方走原框）
    const float k = upper_body_shrink_ratio(prof);
    if (k >= 1.0f) return false;               // 关闭 / ratio 无效：原样返回
    // ① 框：x 不动（肩宽不受腿部影响），y2 收到 y1 + k·h。
    out_box->y2 = box.y1 + k * h;
    // ② 落点等效换算：所有"相对框高"的比例量统一除以 k，
    //    使 ty = y1 + (oy/k)·(k·h) = y1 + oy·h —— 与不收缩时同一像素。
    out_prof->offset_y /= k;
    for (auto& c : out_prof->class_offsets) c.offset_y /= k;
    out_prof->head_aim.head_offset_top_fraction /= k;
    out_prof->head_aim.head_height_fraction /= k;
    // ③ 外推兜底：body_w_over_h 是「宽/全身高」。收缩域要推「宽/上半身高」
    //    = body_w_over_h / k（上半身矮、同宽 ⇒ 比值变大）。
    out_prof->body_w_over_h /= k;
    return true;
}

}  // namespace ttbox::core::aim
