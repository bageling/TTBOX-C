// AimStateMachine.hpp — A10 瞄准状态机
//
// IDLE → SELECTING（目标检测命中）→ AIMING（selector 选中 + 热键有效）
//      → LOST_GRACE（目标丢失）→ 找回恢复 AIMING / 超时回 IDLE。
// 默认丢失宽限 78ms。
// 退出 AIMING / 进入新目标时产生 reset 需求（Reset Pid1Controller + Tracker）。
//
// 第2项升级（目标锁定确认：ENTER/HOLD 双阈值门控）：
//   - ENTER：新目标需连续 confirmation_frames 帧通过确认置信度阈值(target_enter_conf)
//             才进入 AIMING（抗检测闪烁/误触发）。
//   - HOLD ：已锁定目标在后续帧可用较低阈值(target_hold_conf)保持（不被置信度小抖动打断）。
//   - instant_enter：近距离(target_distance < instant_enter_dist) + 高置信
//             （>= instant_enter_conf）的目标跳过确认窗直接锁（快瞄）。
//   - 默认 confirmation_frames=1 且 enter==hold==0 → 完全保持原"首帧即锁"行为（兼容既有测试）。
#pragma once

#include <cstdint>

#include "mouse/MouseTypes.hpp"

namespace ttbox::core::aim {

// 状态机事件（补充目标置信度/距离，供 ENTER/HOLD/instant_enter 判定）
struct AimStateEvent {
    bool has_target = false;     // selector 是否有有效目标
    bool hotkey_active = true;   // 瞄准热键是否按下（V1 外部门控；未提供视为 true）
    uint64_t now_ms = 0;
    float target_confidence = 0.0f;   // 当前锁定目标置信度（0~1）
    float target_distance = 0.0f;     // 目标到选择中心的距离（px）
};

class AimStateMachine {
public:
    // 当前状态。
    AimState state() const { return state_; }
    // 状态名（遥测/日志用）。
    const char* state_name() const;

    // 推进状态机。返回 true 表示本次发生了需要 Reset（controller/tracker）的转换。
    bool update(const AimStateEvent& e, float lost_grace_ms,
                const LockConfirmConfig& confirm = LockConfirmConfig{});
    void reset() {
        state_ = AimState::kIdle;
        lost_since_ms_ = 0;
        was_aiming_ = false;
        confirm_count_ = 0;
        hold_conf_ = 0.0f;
        locked_ = false;
    }

private:
    AimState state_ = AimState::kIdle;
    uint64_t lost_since_ms_ = 0;
    bool was_aiming_ = false;  // 上一帧是否在 AIMING（用于离开时产生 reset）
    int confirm_count_ = 0;    // 已连续通过确认置信度的帧数（ENTER 用）
    float hold_conf_ = 0.0f;   // 最近一次进入时记录的 HOLD 阈值（锁定时用）
    bool locked_ = false;      // 是否处于已锁定（HOLD）状态
};

}  // namespace ttbox::core::aim
