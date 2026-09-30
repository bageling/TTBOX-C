// ContinuousLead.hpp — 持续提前量插件（自瞄时同向累计 → X 偏置，渐入渐出）
//
// 设计：AI 输出同向累计距离超 enter 后，附加 X 方向偏置
// （scale × |dx| × 0.5），渐入渐出。C 桥已有近似实现；本模块在
// C++ 侧提供（AimThread 控制链调用），避免重复叠加。
#pragma once

#include <cmath>

#include "mouse/MouseTypes.hpp"

namespace ttbox::core::aim {

class ContinuousLead {
public:
    // ai_dx/ai_dy：当前 AI 输出（count）。返回附加 X 偏置（count）。
    float apply(int32_t ai_dx, int32_t ai_dy, float dt_ms,
                const ContinuousLeadConfig& cfg) {
        if (!cfg.enabled || cfg.scale <= 0.0f) {
            level_ = 0.0f;
            accum_ = 0;
            dir_ = 0;
            idle_ms_ = 0.0f;
            return 0.0f;
        }
        if (ai_dx == 0 && ai_dy == 0) {
            // 无输出：累计空闲时间，超过 300ms 复位累计（对齐 C 桥）
            idle_ms_ += dt_ms;
            if (idle_ms_ > 300.0f) { accum_ = 0; dir_ = 0; }
            return 0.0f;
        }
        idle_ms_ = 0.0f;
        const int dir = (ai_dx >= 0) ? 1 : -1;
        if (dir_ != 0 && dir_ != dir) accum_ = 0;  // 换向复位
        dir_ = dir;
        accum_ += std::abs(ai_dx);
        if (accum_ < static_cast<int32_t>(cfg.enter_distance)) return 0.0f;
        const float target = cfg.scale * static_cast<float>(std::abs(ai_dx)) * 0.5f;
        level_ += (target - level_) * 0.2f;  // 渐入（一阶平滑）
        if (level_ > target) level_ = target;
        return level_ * static_cast<float>(dir);
    }
    void reset() { level_ = 0.0f; accum_ = 0; dir_ = 0; idle_ms_ = 0.0f; }

private:
    float level_ = 0.0f;
    int32_t accum_ = 0;
    int dir_ = 0;
    float idle_ms_ = 0.0f;
};

}  // namespace ttbox::core::aim
