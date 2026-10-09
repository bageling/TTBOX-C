// test_detection_geometry_filter.cpp — 头身配对、孤立头过滤和关闭兼容
#include <iostream>
#include <vector>
#include "rknn/DetectionGeometryFilter.hpp"

using ttbox::core::DetectionBox;
using ttbox::core::DetectionGeometryFilter;
using ttbox::core::DetectionGeometryFilterConfig;

int main() {
    DetectionGeometryFilterConfig cfg;
    cfg.enabled = true;
    cfg.min_head_conf = 0.2f;
    cfg.min_body_conf = 0.2f;
    cfg.paired_head_min_conf = 0.2f;
    cfg.head_only_min_conf = 0.8f;
    DetectionGeometryFilter filter(cfg);
    std::vector<DetectionBox> boxes = {
        {90, 40, 110, 60, 0.45f, 1},
        {75, 55, 125, 180, 0.70f, 0},
        {300, 300, 304, 304, 0.50f, 1},
    };
    const auto out = filter.filter(boxes, 100.0f, 100.0f);
    if (out.size() != 2 || filter.stats().paired != 1 || filter.stats().head_only != 0) {
        std::cerr << "头身配对或孤立头过滤失败\n"; return 1;
    }
    DetectionGeometryFilter disabled;
    const auto unchanged = disabled.filter(boxes, 100.0f, 100.0f);
    if (unchanged.size() != boxes.size()) {
        std::cerr << "关闭过滤器后未保持兼容\n"; return 1;
    }
    std::cout << "test_detection_geometry_filter: PASS\n";
    return 0;
}
