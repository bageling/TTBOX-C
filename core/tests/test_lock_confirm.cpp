// test_lock_confirm.cpp — 目标锁定确认（第2项：ENTER/HOLD + instant-enter）验证
//
// 覆盖验收场景：
//   Case1 默认配置（confirmation_frames=1, enter_conf=0）→ 首帧即锁（兼容旧行为）
//   Case2 confirmation_frames=3 + enter_conf=0.4 → 需连续3帧置信度>=0.4才锁定
//   Case3 HOLD：已锁目标置信度跌破 enter_conf 但仍>=hold_conf → 保持锁定不闪烁
//   Case4 instant-enter：近距离高置信目标跳过确认窗直接锁
#include <cstdio>

#include "mouse/AimStateMachine.hpp"

using namespace ttbox::core::aim;

namespace {
int failures = 0;
void check(bool cond, const char* msg) {
    if (!cond) { std::printf("  FAIL: %s\n", msg); failures++; }
    else { std::printf("  PASS: %s\n", msg); }
}
}  // namespace

int main() {
    std::printf("===== Case1: default -> lock first frame =====\n");
    {
        AimStateMachine sm;
        LockConfirmConfig cfg;  // 默认 confirmation_frames=1, enter_conf=0
        AimStateEvent e; e.has_target = true; e.hotkey_active = true; e.now_ms = 0;
        e.target_confidence = 0.6f; e.target_distance = 100.0f;
        bool reset = sm.update(e, 78.0f, cfg);
        check(sm.state() == AimState::kAiming, "Case1 默认配置首帧即 AIMING");
        check(reset, "Case1 进入瞄准产生 reset");
    }

    std::printf("===== Case2: confirmation_frames=3 -> need 3 frames =====\n");
    {
        AimStateMachine sm;
        LockConfirmConfig cfg;
        cfg.confirmation_frames = 3;
        cfg.enter_conf = 0.4f;
        cfg.instant_enter_enabled = false;  // 关掉 instant-enter 测纯确认帧
        AimStateEvent e; e.has_target = true; e.hotkey_active = true;
        e.target_confidence = 0.6f; e.target_distance = 200.0f;  // 远距离
        // 帧1：距离 200 > 105，不触发 instant-enter；confirm_count=1 <3 → 不锁定
        e.now_ms = 0; sm.update(e, 78.0f, cfg);
        check(sm.state() != AimState::kAiming, "Case2 帧1不锁定");
        // 帧2
        e.now_ms = 16; sm.update(e, 78.0f, cfg);
        check(sm.state() != AimState::kAiming, "Case2 帧2不锁定");
        // 帧3：confirm_count=3 → 锁定
        e.now_ms = 32; sm.update(e, 78.0f, cfg);
        check(sm.state() == AimState::kAiming, "Case2 帧3连续确认后锁定");
        // 置信度跌破 enter_conf（闪烁）：confirm_count 清零，需重新确认
        e.target_confidence = 0.2f; e.now_ms = 48; sm.update(e, 78.0f, cfg);
        // 已锁（HOLD）→ hold_conf=0 保持锁定（不因置信度跌回 while locked）
        check(sm.state() == AimState::kAiming, "Case2 已锁目标置信度略降仍保持锁定");
    }

    std::printf("===== Case3: HOLD keeps lock under confidence dip =====\n");
    {
        AimStateMachine sm;
        LockConfirmConfig cfg;
        cfg.confirmation_frames = 1;
        cfg.enter_conf = 0.5f;
        cfg.hold_conf = 0.3f;
        cfg.instant_enter_enabled = false;
        AimStateEvent e; e.has_target = true; e.hotkey_active = true;
        e.target_confidence = 0.7f; e.target_distance = 200.0f;
        e.now_ms = 0; sm.update(e, 78.0f, cfg);
        check(sm.state() == AimState::kAiming, "Case3 高置信首帧锁定");
        // 置信度降至 0.35（跌破 enter_conf 0.5 但 >= hold_conf 0.3）→ 保持
        e.target_confidence = 0.35f; e.now_ms = 16; sm.update(e, 78.0f, cfg);
        check(sm.state() == AimState::kAiming, "Case3 HOLD 阈值保持锁定不闪烁");
        // 置信度跌破 hold_conf 0.3 → 仍保持（目标还在,不丢失）
        e.target_confidence = 0.10f; e.now_ms = 32; sm.update(e, 78.0f, cfg);
        check(sm.state() == AimState::kAiming, "Case3 仍保持（置信度变化不断锁,仅 LOST 才进宽限）");
    }

    std::printf("===== Case4: instant-enter skips confirmation =====\n");
    {
        AimStateMachine sm;
        LockConfirmConfig cfg;
        cfg.confirmation_frames = 5;      // 即使要求 5 帧
        cfg.enter_conf = 0.9f;            // 即使 enter_conf 很高
        cfg.instant_enter_enabled = true;
        cfg.instant_enter_dist = 105.0f;
        cfg.instant_enter_conf = 0.50f;
        AimStateEvent e; e.has_target = true; e.hotkey_active = true;
        e.target_confidence = 0.6f; e.target_distance = 50.0f;  // 近距离 + 中等置信
        e.now_ms = 0;
        sm.update(e, 78.0f, cfg);
        check(sm.state() == AimState::kAiming, "Case4 近距离高置信跳过确认窗直接锁");
    }

    std::printf("\n===== RESULT: %s (%d failures) =====\n",
                failures == 0 ? "ALL PASS" : "FAIL", failures);
    return failures == 0 ? 0 : 1;
}