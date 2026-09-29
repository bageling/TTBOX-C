// LeadPredictor.hpp — 提前量（横向 X 轴），BB 对标移植（2026-09-24）
//
// 小白理解：
//   敌人横向跑动时，自瞄如果永远"照着它现在的位置打"，就会一直落后。
//   提前量 = 在瞄准点上往前加一点偏移，让准星落在敌人"将要去的地方"。
//
// 现状（2026-09-29 业主裁定「提前量只留 2.0」）：
//   · 只剩二代 Lead2（积分累积）：直接对"瞄准点横向误差"做积分，误差一直在同一边
//     就会越积越大，形成偏移；死区内按 decay 衰减；Y 轴输出大时抑制（防斜拉抛物线）。
//   · 一代 Lead1（帧窗口投票）已整段删除。它天然带一帧延迟，且需要"喂上一帧 mx"的
//     两阶段调用；二代同帧生效，行为更可预期。
//
// 调用关系（对齐 bb-port/03 号 §0 数据流）：
//   算误差之前：at.x += lead2 本帧积分输出（同帧生效）
//
// ★ 只改 X 轴。Y 轴不加提前量（垂直受后坐力/跳跃影响，预测帮倒忙）。
// ★ 默认 enabled=false ⇒ 不跑即零输出，输出链与本模块加入前逐字节一致。
#pragma once

#include <cmath>
#include <cstdint>

#include "mouse/MouseTypes.hpp"

namespace ttbox::core::aim {

// ---------------------------------------------------------------------------
// 二代：积分累积法
// ---------------------------------------------------------------------------
class Lead2 {
public:
    struct Input {
        bool has_target = false;
        float target_x = 0.0f;
        float target_y = 0.0f;
        float crosshair_x = 0.0f;
        float crosshair_y = 0.0f;
        float last_move_y = 0.0f;  // 上一帧纵向输出（px，Y 轴抑制用）
        uint32_t now_ms = 0;
    };

    // 返回本帧应叠加到瞄准点的 X 偏移（px，右为正）。同帧生效。
    float update(const Lead2Config& cfg, const Input& in);

    void reset();
    bool active() const { return active_; }
    float offset() const { return current_offset_; }

private:
    float integral_ = 0.0f;        // 积分器（px）
    float current_offset_ = 0.0f;  // 当前输出偏移
    bool active_ = false;
    bool holding_ = false;
    uint32_t hold_start_ = 0;
    uint32_t cooldown_start_ = 0;
    bool cooldown_active_ = false;
};

// ---------------------------------------------------------------------------
// 门面：只剩二代（2026-09-29 一代删除）。门面本身保留，是为了不动 AimThread
// 的调用点（`lead_pred_.prepare_x_offset(...)` / `clear_target(...)` / `reset()`）。
// ---------------------------------------------------------------------------
class LeadPredictor {
public:
    // 跑二代（同帧生效），返回本帧 X 偏置（px）。
    float prepare_x_offset(float crosshair_x, float crosshair_y,
                           float target_x, float target_y, float last_move_y,
                           uint32_t now_ms, const Lead2Config& c2) {
        Lead2::Input in;
        in.has_target = true;
        in.target_x = target_x;
        in.target_y = target_y;
        in.crosshair_x = crosshair_x;
        in.crosshair_y = crosshair_y;
        in.last_move_y = last_move_y;
        in.now_ms = now_ms;
        return l2_.update(c2, in);
    }

    // 无目标时把二代清零
    void clear_target(uint32_t now_ms, const Lead2Config& c2) {
        Lead2::Input in;
        in.has_target = false;
        in.now_ms = now_ms;
        l2_.update(c2, in);
    }

    void reset() { l2_.reset(); }

    Lead2& lead2() { return l2_; }

private:
    Lead2 l2_;
};

// ==================== Lead2 实现 ====================

inline void Lead2::reset() {
    integral_ = 0.0f;
    current_offset_ = 0.0f;
    active_ = false;
    holding_ = false;
    hold_start_ = 0;
    cooldown_start_ = 0;
    cooldown_active_ = false;
}

inline float Lead2::update(const Lead2Config& cfg, const Input& in) {
    // [0] 总开关
    if (!cfg.enabled) {
        reset();
        return 0.0f;
    }

    // [1] Y 轴抑制（垂直输出越大，横向提前量越小；平方衰减）
    float y_scale = 1.0f;
    if (cfg.y_suppress_enabled) {
        const float my_abs = std::fabs(in.last_move_y);
        if (my_abs >= cfg.y_suppress_max) {
            y_scale = 0.0f;
        } else if (my_abs > cfg.y_suppress_min) {
            y_scale = 1.0f - (my_abs - cfg.y_suppress_min) / (cfg.y_suppress_max - cfg.y_suppress_min);
        }
        const float cur_max = cfg.max_offset * y_scale;
        if (integral_ > cur_max) integral_ = cur_max;
        if (integral_ < -cur_max) integral_ = -cur_max;
    }

    // [2] 无目标
    if (!in.has_target) {
        reset();
        return 0.0f;
    }

    // [3] 误差与距离
    const float error_x = (in.target_x + integral_) - in.crosshair_x;
    const float error_y = in.target_y - in.crosshair_y;
    const float dist = std::sqrt(error_x * error_x + error_y * error_y);

    // [4] 保持窗
    if (holding_) {
        if (static_cast<float>(in.now_ms - hold_start_) < cfg.hold_ms) return current_offset_;
        holding_ = false;
    }

    // [5] 超出激活距离：全清
    if (dist > cfg.activation_distance) {
        reset();
        return 0.0f;
    }

    // [6] 进入距离后的冷却
    if (!cooldown_active_) {
        if (cooldown_start_ == 0) cooldown_start_ = in.now_ms;
        if (static_cast<float>(in.now_ms - cooldown_start_) < cfg.cooldown_ms) {
            integral_ = 0.0f;
            current_offset_ = 0.0f;
            return 0.0f;
        }
        cooldown_active_ = true;
    }

    // [7] 积分 / 衰减（注意 yScale 是平方）
    if (std::fabs(error_x) <= cfg.dead_zone) {
        integral_ *= cfg.decay;
    } else {
        integral_ += error_x * cfg.gain * y_scale * y_scale;
    }
    if (integral_ > cfg.max_offset) integral_ = cfg.max_offset;
    if (integral_ < -cfg.max_offset) integral_ = -cfg.max_offset;

    // [8] 输出
    current_offset_ = integral_;
    active_ = true;
    holding_ = true;
    hold_start_ = in.now_ms;
    return current_offset_;
}

}  // namespace ttbox::core::aim
