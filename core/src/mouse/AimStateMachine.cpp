// AimStateMachine.cpp — A10 瞄准状态机实现
#include "mouse/AimStateMachine.hpp"

namespace ttbox::core::aim {

const char* AimStateMachine::state_name() const {
    switch (state_) {
        case AimState::kSelecting: return "SELECTING";
        case AimState::kAiming: return "AIMING";
        case AimState::kLostGrace: return "LOST_GRACE";
        default: return "IDLE";
    }
}

bool AimStateMachine::update(const AimStateEvent& e, float lost_grace_ms,
                             const LockConfirmConfig& confirm) {
    const bool was_aiming = state_ == AimState::kAiming;
    bool need_reset = false;

    // ---- 目标锁定确认（ENTER/HOLD + instant-enter）----
    // 判定本帧目标是否通过"锁定确认"：
    //   - 已是 HOLD 锁定态 → 用较低的 hold_conf 保持
    //   - 未锁定 → 用较高的 enter_conf（且需连续 confirmation_frames 帧）
    //   - instant-enter：近距离 + 高置信直接锁定
    bool confirm_pass = false;
    if (e.has_target) {
        if (locked_) {
            confirm_pass = e.target_confidence >= hold_conf_;
        } else if (confirm.instant_enter_enabled &&
                   e.target_distance < confirm.instant_enter_dist &&
                   e.target_confidence >= confirm.instant_enter_conf) {
            confirm_pass = true;  // 近距离高置信：跳过确认窗
        } else {
            if (e.target_confidence >= confirm.enter_conf) {
                confirm_count_++;
                confirm_pass = confirm_count_ >= std::max(1, confirm.confirmation_frames);
            } else {
                confirm_count_ = 0;
                confirm_pass = false;
            }
        }
    }

    if (state_ == AimState::kIdle || state_ == AimState::kSelecting) {
        if (e.has_target && confirm_pass) {
            if (e.hotkey_active) {
                state_ = AimState::kAiming;
                hold_conf_ = confirm.hold_conf;
                locked_ = true;
                need_reset = true;  // 进入瞄准：清理历史状态
            } else {
                state_ = AimState::kSelecting;
            }
        } else {
            // 目标存在但未过确认 / 无目标
            state_ = e.has_target ? AimState::kSelecting : AimState::kIdle;
        }
    } else if (state_ == AimState::kAiming) {
        if (e.has_target && confirm_pass && e.hotkey_active) {
            // 持续瞄准（用 hold_conf 保持）
        } else if (!e.has_target) {
            state_ = AimState::kLostGrace;
            lost_since_ms_ = e.now_ms;
        } else if (!e.hotkey_active) {
            state_ = AimState::kIdle;
            locked_ = false;
            confirm_count_ = 0;
            need_reset = true;
        } else {
            // 目标置信度跌破 hold_conf（但仍有目标）：保持瞄准不闪烁
            // （置信度略降不打断锁定；真正丢失才进 LOST_GRACE）
        }
    } else {  // LOST_GRACE
        if (e.has_target && e.hotkey_active) {
            if (confirm_pass) {
                state_ = AimState::kAiming;  // 找回（需热键仍有效 + 过确认）
                locked_ = true;
            } else {
                state_ = AimState::kSelecting;
            }
        } else if (e.has_target) {
            // 目标找回但热键已松开：直接回 IDLE 并 reset。
            state_ = AimState::kIdle;
            locked_ = false;
            confirm_count_ = 0;
            need_reset = true;
        } else if (e.now_ms - lost_since_ms_ >= static_cast<uint64_t>(lost_grace_ms)) {
            state_ = AimState::kIdle;
            locked_ = false;
            need_reset = true;
        }
    }

    // 离开 AIMING（非过渡到 LOST_GRACE/仍瞄准）也要求 reset
    if (was_aiming && state_ != AimState::kAiming && state_ != AimState::kLostGrace) {
        need_reset = true;
    }
    return need_reset;
}

}  // namespace ttbox::core::aim