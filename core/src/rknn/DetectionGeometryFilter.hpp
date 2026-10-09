// DetectionGeometryFilter.hpp — 人物/头部双框融合与几何过滤（**行为类**）
//
// 配置与统计值类型已下沉到 common/DetectionGeometryTypes.hpp（纯值类型、干净叶子层）。
// 这样 model/RuntimeProfile.hpp 只需要 include 那个叶子头，不必把 rknn 拖进来
// ⇒ model ↔ rknn 的模块级依赖环在这里被断开（2026-10-06 架构整理）。
#pragma once
#include <cstdint>
#include <vector>
#include "common/Types.hpp"
#include "common/DetectionGeometryTypes.hpp"

namespace ttbox::core {

// 头/身双框融合与几何过滤（人物类目标的成对输出与边界剔除）。
class DetectionGeometryFilter {
public:
    explicit DetectionGeometryFilter(DetectionGeometryFilterConfig config = {}) : config_(config) {}
    void set_config(const DetectionGeometryFilterConfig& config) { config_ = config; }
    const DetectionGeometryFilterConfig& config() const { return config_; }
    const DetectionGeometryFilterStats& stats() const { return stats_; }
    std::vector<DetectionBox> filter(const std::vector<DetectionBox>& boxes, float center_x, float center_y);
private:
    DetectionGeometryFilterConfig config_;
    DetectionGeometryFilterStats stats_;
};

} // namespace ttbox::core
