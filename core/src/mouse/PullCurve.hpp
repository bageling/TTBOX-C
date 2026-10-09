// PullCurve.hpp — 拉枪曲线插件（自瞄时附加弧线）
//
// 设计：目标误差距离 ≥ min_distance 时激活，
// 在拉枪方向（X 主导）附加垂直弧线。
// 由 AimThread 在 deadzone 之前调用（输出链顺序）。
//
// ★ 2026-09-29 删除自研抖动：原实现在此叠加一个 ~2Hz 固定正弦抖动
//   （`v += sin(phase) * jitter_px * 0.5f`，默认 jitter_px=3.0px）。
//   固定频率正弦在画面上就是「有规律的上下抖」，且它与全局正弦、拟人化噪声
//   等其余 6 套抖动机制重复 ⇒ 按业主口径删除。
//   ★ 2026-10-07 清理：`PullCurveConfig::jitter_px` 字段已随批 C 一并删除（此前仅序列化层搬运的死字段）。
#pragma once

#include <cmath>

#include "mouse/MouseTypes.hpp"

namespace ttbox::core::aim {

class PullCurve {
public:
    // err_x/err_y：当前像素误差；out_x/out_y：当前缩放后输出（count）。
    // 返回附加的 Y 弧线量（count）；直接修改 out_y 亦可由调用方处理。
    float apply(float err_x, float err_y, float out_x, [[maybe_unused]] float out_y,
                const PullCurveConfig& cfg, [[maybe_unused]] float dt_ms) {
        if (!cfg.enabled) return 0.0f;
        const float dist = std::hypot(err_x, err_y);
        if (dist < cfg.min_distance) return 0.0f;
        // 弧线：沿拉枪方向附加垂直分量（对齐 C 桥 arc = strength × |dx| × 0.08）
        float arc = cfg.strength * std::fabs(out_x) * 0.08f;
        if (arc > 24.0f) arc = 24.0f;
        // 方向：X 拉枪方向（正/负）决定弧线方向（C 桥语义：dx>=0 加正 Y）
        const float dir = (out_x >= 0.0f) ? 1.0f : -1.0f;
        return arc * dir;
    }
    // 无内部状态（抖动已删）；保留接口，调用方不必改。
    void reset() {}
};

}  // namespace ttbox::core::aim
