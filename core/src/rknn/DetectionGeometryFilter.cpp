// DetectionGeometryFilter.cpp — 头身几何过滤与置信度分层
#include "rknn/DetectionGeometryFilter.hpp"
#include <algorithm>
#include <cmath>

namespace ttbox::core {
namespace {
// 框几何小工具：宽/高/中心点/带容差的包含判定/长宽比。
float w(const DetectionBox& b) { return std::max(0.0f, b.x2 - b.x1); }
float h(const DetectionBox& b) { return std::max(0.0f, b.y2 - b.y1); }
float cx(const DetectionBox& b) { return (b.x1 + b.x2) * 0.5f; }
float cy(const DetectionBox& b) { return (b.y1 + b.y2) * 0.5f; }
bool contains(const DetectionBox& b, float x, float y, float ex, float ey) {
    return x >= b.x1 - w(b) * ex && x <= b.x2 + w(b) * ex &&
           y >= b.y1 - h(b) * ey && y <= b.y2 + h(b) * ey;
}
float aspect(const DetectionBox& b) { return h(b) > 0.0f ? w(b) / h(b) : 999.0f; }
}

// 头/身融合与几何过滤：配对成功保留头+身双框，否则按策略单留头或身。
std::vector<DetectionBox> DetectionGeometryFilter::filter(const std::vector<DetectionBox>& boxes,
                                                           float center_x, float center_y) {
    stats_ = {};
    stats_.input = static_cast<uint32_t>(boxes.size());
    if (!config_.enabled) { stats_.output = stats_.input; return boxes; }
    const float body_conf = std::max(config_.min_confidence, config_.min_body_conf);
    const float head_conf = std::max(config_.min_confidence, config_.min_head_conf);
    const float paired_conf = std::max(config_.min_confidence * 0.8f, config_.paired_head_min_conf);
    const float single_head_conf = std::max(config_.min_confidence, config_.head_only_min_conf);
    std::vector<DetectionBox> heads, bodies;
    for (const auto& b : boxes) {
        if (b.class_id == config_.head_class_id && b.score >= head_conf &&
            w(b) >= config_.min_head_width_px && h(b) >= config_.min_head_height_px &&
            aspect(b) <= config_.max_head_aspect) heads.push_back(b);
        if (b.class_id == config_.body_class_id && b.score >= body_conf &&
            w(b) >= config_.min_body_width_px && h(b) >= config_.min_body_height_px &&
            aspect(b) <= config_.max_body_aspect) bodies.push_back(b);
    }
    stats_.heads = static_cast<uint32_t>(heads.size());
    stats_.bodies = static_cast<uint32_t>(bodies.size());
    std::vector<DetectionBox> out;
    std::vector<bool> used_body(bodies.size(), false);
    const auto border_bad = [&](const DetectionBox& b) {
        if (!config_.reject_border) return false;
        const float d = std::hypot(cx(b) - center_x, cy(b) - center_y);
        return (b.x1 <= config_.border_margin_px || b.y1 <= config_.border_margin_px) &&
               d > config_.border_center_max_px;
    };
    for (const auto& head : heads) {
        if (border_bad(head)) { ++stats_.rejected; continue; }
        int match = -1;
        for (size_t i = 0; i < bodies.size(); ++i) {
            if (contains(bodies[i], cx(head), cy(head), config_.pair_expand_x, config_.pair_expand_y)) {
                if (match < 0 || bodies[i].score > bodies[match].score) match = static_cast<int>(i);
            }
        }
        if (match >= 0 && head.score >= paired_conf) {
            // 保留原始框，但成对输出；后续目标层用人物框计算用户瞄准部位。
            out.push_back(head); out.push_back(bodies[match]); used_body[match] = true; ++stats_.paired;
        } else if (config_.allow_head_only && head.score >= single_head_conf &&
                   std::hypot(cx(head) - center_x, cy(head) - center_y) <= config_.head_only_center_max_px) {
            out.push_back(head); ++stats_.head_only;
        } else { ++stats_.rejected; }
    }
    if (config_.allow_body_only) {
        for (size_t i = 0; i < bodies.size(); ++i) {
            if (!used_body[i]) { out.push_back(bodies[i]); ++stats_.body_only; }
        }
    }
    stats_.output = static_cast<uint32_t>(out.size());
    return out;
}
} // namespace ttbox::core
