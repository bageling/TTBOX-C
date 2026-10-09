// DetectionGeometryTypes.hpp — 检测几何过滤的**配置与统计值类型**（叶子头）
//
// ★ 为什么独立成一个头（2026-10-06 架构整理）：
//   下面这两个 struct 是**纯值类型**（bool / int / float / uint32_t），不含任何行为、
//   不依赖 rknn 的任何东西。此前它们住在 rknn/DetectionGeometryFilter.hpp 里，于是：
//     model/RuntimeProfile.hpp  为了拿配置类型 → include 一个 rknn 头
//     rknn/DecodeNMS.hpp         为了拿 InferenceProfile/FovProfile → 反过来 include model 头
//   两边一夹，就形成了 **model ↔ rknn 的模块级依赖环**（两个最大模块 3291 + 3700 行
//   互相拖住，改一个公共头触发两模块全量重编译）。
//   把值类型下沉到 common/（干净叶子层）后，环即断开：model 不再看见 rknn，
//   两个模块可以各自独立编译。
//
// 本头的使用约定：
//   - 只放**值类型**（struct / 枚举 / 常量），不放带行为的类；
//   - 行为类 DetectionGeometryFilter 留在 rknn/DetectionGeometryFilter.hpp，它 include 本头；
//   - 谁只需要"配置长什么样"，就 include 本头，不要 include rknn 那个。
#pragma once

#include <cstdint>

namespace ttbox::core {

// 人物/头部双框的几何过滤门槛（纯配置，运行时热更新）
struct DetectionGeometryFilterConfig {
    bool enabled = false;
    bool allow_body_only = true;
    bool allow_head_only = false;
    bool lower_body_block = true;
    int body_class_id = 0;
    int head_class_id = 1;
    float min_confidence = 0.25f;       // 用户总置信度基线
    float min_head_conf = 0.18f;
    float min_body_conf = 0.26f;
    float paired_head_min_conf = 0.20f;
    float head_only_min_conf = 0.75f;
    float head_only_center_max_px = 175.0f;
    float min_body_width_px = 8.0f;
    float min_body_height_px = 26.0f;
    float min_head_width_px = 1.5f;
    float min_head_height_px = 1.5f;
    float max_head_aspect = 2.55f;
    float max_body_aspect = 1.05f;
    float pair_expand_x = 0.25f;
    float pair_expand_y = 0.12f;
    float border_margin_px = 2.0f;
    bool reject_border = true;
    float border_center_max_px = 105.0f;
};

// 过滤器运行统计（纯计数，供面板/日志展示）
struct DetectionGeometryFilterStats {
    uint32_t input = 0;
    uint32_t output = 0;
    uint32_t heads = 0;
    uint32_t bodies = 0;
    uint32_t paired = 0;
    uint32_t body_only = 0;
    uint32_t head_only = 0;
    uint32_t rejected = 0;
};

}  // namespace ttbox::core
