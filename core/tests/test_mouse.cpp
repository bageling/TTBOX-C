// test_mouse.cpp — A10 AI 鼠标注入模块单元测试
//
// 覆盖：TargetSelector / AimPointProfile / CoordinateTransform / AimTracker /
//       Deadzone / RateLimit / MotionMerge / AimState /
//       RuntimeProfile(mouse 段) + 关键场景。
#include "test_util.hpp"

#include <cmath>
#include <type_traits>
#include <utility>

#include "mouse/AimPointProfile.hpp"
#include "mouse/AimStateMachine.hpp"
#include "mouse/AimTracker.hpp"
#include "mouse/CoordinateTransform.hpp"
#include "mouse/ContinuousLead.hpp"
#include "mouse/Deadzone.hpp"
#include "mouse/FovAngle.hpp"
#include "mouse/MotionMerge.hpp"
#include "mouse/MouseRouter.hpp"
#include "mouse/MouseTypes.hpp"
#include "mouse/PullCurve.hpp"
#include "mouse/RateLimit.hpp"
#include "mouse/TargetSelector.hpp"
#include "model/RuntimeProfile.hpp"

using namespace ttbox::core;

// ---------------------------------------------------------------------------
// 1. TargetSelector
// ---------------------------------------------------------------------------
TEST(mouse_target_selector_fov_and_confidence) {
    aim::TargetSelector sel;
    aim::TargetSelectorConfig cfg;
    cfg.fov_range = 1.0f;
    cfg.confidence = 0.5f;
    cfg.roi_w = 320;
    cfg.roi_h = 320;

    std::vector<DetectionBox> dets;
    // 中心远（conf 0.9，超出半径）与近（conf 0.6）
    DetectionBox far, near;
    far.x1 = 40; far.y1 = 40; far.x2 = 60; far.y2 = 60; far.score = 0.9f; far.class_id = 0;
    near.x1 = 150; near.y1 = 150; near.x2 = 170; near.y2 = 170; near.score = 0.6f; near.class_id = 0;
    dets.push_back(far);
    dets.push_back(near);

    // fov_range=1：半径=160，两者都在；取最近（near）
    auto s = sel.select(dets, cfg);
    CHECK(s.valid);
    CHECK_EQ(static_cast<int>(s.box.x1), 150);

    // 低置信度被过滤
    cfg.confidence = 0.95f;
    s = sel.select(dets, cfg);
    CHECK(!s.valid);

    // fov_range=0.1：半径=16，far(中心距 155) 被过滤；near(距 0) 仍命中
    cfg.confidence = 0.5f;
    cfg.fov_range = 0.1f;
    std::vector<DetectionBox> only_far = {far};
    s = sel.select(only_far, cfg);
    CHECK(!s.valid);
    s = sel.select(dets, cfg);
    CHECK(s.valid);
    CHECK_EQ(static_cast<int>(s.box.x1), 150);
}

TEST(mouse_target_selector_class_filter) {
    aim::TargetSelector sel;
    aim::TargetSelectorConfig cfg;
    cfg.roi_w = 320; cfg.roi_h = 320;
    std::vector<DetectionBox> dets;
    DetectionBox b;
    b.x1 = 100; b.y1 = 100; b.x2 = 120; b.y2 = 120; b.score = 0.9f; b.class_id = 1;
    dets.push_back(b);
    cfg.class_filter = {0};
    CHECK(!sel.select(dets, cfg).valid);  // class 1 被过滤
    cfg.class_filter = {1};
    CHECK(sel.select(dets, cfg).valid);
}

// 瞄准范围 = 截取尺寸内划最大的圆形（业主口径，2026-09-24）。
// 框坐标是整帧坐标系（AimThread 传 task.frame_width/height），所以整帧尺寸算出来的
// 半径 min(2560,1440)/2 = 720px 已经大于检测区半宽（capture 640 ⇒ 320px）——
// 圆心到框的距离是拿 aim_ratio 算的，故这里把 aim_ratio 设 0.5 让几何直白。
TEST(mouse_target_selector_aim_range_is_capture_inscribed_circle) {
    aim::TargetSelector sel;
    aim::TargetSelectorConfig cfg;
    cfg.roi_w = 2560;  // 整帧（框坐标系）
    cfg.roi_h = 1440;
    cfg.center_x = 0.5f;
    cfg.center_y = 0.5f;   // FOV 中心 = (1280, 720)
    cfg.aim_ratio_x = 0.5f;
    cfg.aim_ratio_y = 0.5f;
    cfg.fov_range = 1.0f;
    cfg.confidence = 0.3f;
    cfg.search_radius_px = 320.0f;   // 截取尺寸 640×640 内划最大圆

    // inner 中心 (1480,920)：距中心 282.8px ⇒ 圆内
    DetectionBox inner;
    inner.x1 = 1470; inner.y1 = 910; inner.x2 = 1490; inner.y2 = 930;
    inner.score = 0.9f; inner.class_id = 0;
    // corner 中心 (1520,960)：距中心 339.4px ⇒ 圆外（但在旧口径 720px 圆内）
    DetectionBox corner;
    corner.x1 = 1510; corner.y1 = 950; corner.x2 = 1530; corner.y2 = 970;
    corner.score = 0.9f; corner.class_id = 0;

    // 只有角上那个 ⇒ 被圆滤掉（这一条就是本次修复的墓碑）
    std::vector<DetectionBox> only_corner = {corner};
    CHECK(!sel.select(only_corner, cfg).valid);

    // 圆内 + 圆外同时在 ⇒ 选圆内那个
    std::vector<DetectionBox> both;
    both.push_back(corner);
    both.push_back(inner);
    auto s = sel.select(both, cfg);
    CHECK(s.valid);
    CHECK_EQ(static_cast<int>(s.box.x1), 1470);

    // 半径随 fov_range 缩放：0.5 ⇒ 160px，连 inner（282.8px）也出圈
    cfg.fov_range = 0.5f;
    std::vector<DetectionBox> only_inner = {inner};
    CHECK(!sel.select(only_inner, cfg).valid);

    // search_radius_px = 0 ⇒ 回退旧口径 min(roi_w,roi_h)/2 = 720px：
    // 角上那个又会被收（保证未接线的调用方行为不变）
    cfg.fov_range = 1.0f;
    cfg.search_radius_px = 0.0f;
    aim::TargetSelector legacy;
    CHECK(legacy.select(only_corner, cfg).valid);
}

// ---------------------------------------------------------------------------
// 2. AimPointProfile
// ---------------------------------------------------------------------------
TEST(mouse_aim_point_profile_default_and_class_override) {
    aim::AimPointProfile prof;
    prof.offset_x = 0.5f;
    prof.offset_y = 0.5f;
    DetectionBox box;
    box.x1 = 100; box.y1 = 200; box.x2 = 200; box.y2 = 400;
    float tx = 0, ty = 0;
    // 默认：框中心
    CHECK(aim::aim_point_at(box, 0, prof, &tx, &ty));
    CHECK_EQ(tx, 150.0f);
    CHECK_EQ(ty, 300.0f);

    // class_offsets 覆盖（class 0：0.25, 0.25）
    aim::ClassOffset co;
    co.class_id = 0;
    co.offset_x = 0.25f;
    co.offset_y = 0.25f;
    co.priority = 0;
    prof.class_offsets.push_back(co);
    CHECK(aim::aim_point_at(box, 0, prof, &tx, &ty));
    CHECK_EQ(tx, 125.0f);
    CHECK_EQ(ty, 250.0f);
    // class 1 未覆盖 → 默认
    CHECK(aim::aim_point_at(box, 1, prof, &tx, &ty));
    CHECK_EQ(tx, 150.0f);
}

// ---------------------------------------------------------------------------
// 3. CoordinateTransform（ROI/crop 系）
// ---------------------------------------------------------------------------
TEST(mouse_coord_transform_pixel_error) {
    aim::AimPointProfile prof;  // 默认 0.5/0.5
    DetectionBox box;
    box.x1 = 190; box.y1 = 190; box.x2 = 210; box.y2 = 210;  // 中心 (200,200)
    float ex = 0, ey = 0;
    // roi 320×320，准星 (160,160)，目标中心 (200,200) → err +40/+40
    CHECK(aim::CoordinateTransform::pixel_error(box, 0, prof, 320, 320, &ex, &ey));
    CHECK_EQ(ex, 40.0f);
    CHECK_EQ(ey, 40.0f);
    // ROI 改变（192）→ 准星 (96,96) → err +104/+104（自动重算）
    CHECK(aim::CoordinateTransform::pixel_error(box, 0, prof, 192, 192, &ex, &ey));
    CHECK_EQ(ex, 104.0f);
    // ★ V1.0.13：aim_offset_x/y 已删 ⇒ 准星恒为裁剪区正中心（160,160）
    CHECK(aim::CoordinateTransform::pixel_error(box, 0, prof, 320, 320, &ex, &ey));
    CHECK_EQ(ex, 40.0f);   // 200 - 160
    CHECK_EQ(ey, 40.0f);   // 200 - 160
}

// ---------------------------------------------------------------------------
// 4. AimTracker（帧差速度 + EMA 平滑 + 目标切换重置 + 预测）
// ---------------------------------------------------------------------------
TEST(mouse_aim_tracker_velocity_and_switch) {
    aim::AimTracker tr;
    tr.update(100.0f, 100.0f, 0, 0);
    // 100ms 移动 +10px → raw=100px/s, EMA(0.4)=40px/s
    tr.update(110.0f, 100.0f, 0, 100000);
    CHECK_EQ(tr.state().vx, 40.0f);
    CHECK_EQ(tr.state().vy, 0.0f);
    // 预测 = 平滑位置 + 速度×时域（速度估计仍用原始帧差，见 AimTracker.cpp）。
    // ★ V1.0.11：One-Euro 的 beta 从 0.10 降到 0.03 ⇒ 平滑更强、平滑位置更靠后，
    //   旧期望值 127.45（= 平滑位置 107.45 + 40×0.5）随之失效。
    //   改为以「当前平滑位置」推期望：测的是语义（预测 = 平滑位置 + v×t），
    //   不是某个 beta 下的具体数字 —— 否则每调一次滤波常数都要改一次测试。
    const float smooth_x = tr.state().x;
    CHECK(smooth_x > 100.0f);   // 平滑确实在跟随
    CHECK(smooth_x < 110.0f);   // 且没有直接等于原始值（确实滤了）
    float px = 0, py = 0;
    tr.predict(0.5f, &px, &py);
    CHECK(std::abs(px - (smooth_x + 40.0f * 0.5f)) < 0.05f);
    // 目标切换（target_id 变化）→ 速度清零
    tr.update(200.0f, 200.0f, 1, 200000);
    CHECK_EQ(tr.state().vx, 0.0f);
    CHECK_EQ(tr.state().vy, 0.0f);
    // reset
    tr.reset();
    CHECK(!tr.state().valid);
}

// ---------------------------------------------------------------------------
// 4b. AimTracker 位置平滑（OneEuro）：检测框噪声抑制
// 真实场景：人体框上边缘 y1 帧间跳变 ±18px（模型头顶边界不确定），
// 平滑后逐帧 step 应显著下降（否则 PID 输入噪声 → 鼠标抖动）。
// ---------------------------------------------------------------------------
TEST(mouse_aim_tracker_position_smoothing) {
    aim::AimTracker tr;
    // 真实抓取 y1 序列（138fps，中心 715px 附近 ±18px 脉冲噪声）
    const float ys[] = {723.7f, 715.4f, 712.1f, 716.0f, 714.6f, 719.3f, 717.9f,
                        716.8f, 729.1f, 718.8f, 716.0f, 730.3f, 719.0f, 718.7f,
                        727.3f, 712.7f, 718.3f, 714.0f, 723.6f, 719.4f, 717.1f,
                        715.9f, 713.4f, 719.4f, 715.5f, 725.7f, 713.4f, 717.0f,
                        716.2f, 716.9f};
    const uint64_t dt_us = 1000000 / 138;  // 138fps 帧间隔
    tr.update(1274.0f, ys[0], 2, 0);
    float raw_max_step = 0.0f, smooth_max_step = 0.0f;
    float prev_raw = ys[0], prev_smooth = tr.state().y;
    for (size_t i = 1; i < sizeof(ys) / sizeof(ys[0]); ++i) {
        tr.update(1274.0f, ys[i], 2, i * dt_us);
        const float step = std::fabs(tr.state().y - prev_smooth);
        if (step > smooth_max_step) smooth_max_step = step;
        prev_smooth = tr.state().y;
        const float raw_step = std::fabs(ys[i] - prev_raw);
        if (raw_step > raw_max_step) raw_max_step = raw_step;
        prev_raw = ys[i];
    }
    // 原始最大逐帧跳变（真实数据 18.2px）
    CHECK(raw_max_step > 10.0f);
    // 平滑后逐帧 step 应 < 原始最大 step 的 30%（实测 ≤3.5px）
    CHECK(smooth_max_step < raw_max_step * 0.30f);
    // 平滑位置不能发散出合理范围（仍在目标区域附近）
    CHECK(std::fabs(tr.state().y - 717.0f) < 30.0f);
}

// ---------------------------------------------------------------------------
// 2026-09-20 定案：pid1.cpp 为唯一标准控制器，旧经典 PID MotionController
// 连同其 3 个用例（p_only / ki / kd）一并删除（Pid1Controller 覆盖见 test_pid1.cpp）。
// ---------------------------------------------------------------------------

// ---------------------------------------------------------------------------
// 6. Deadzone（X/Y 独立）
// ---------------------------------------------------------------------------
TEST(mouse_deadzone_xy_independent) {
    aim::MouseProfile p;
    p.deadzone_x = 1.0f;
    p.deadzone_y = 2.0f;
    CHECK_EQ(aim::deadzone_x(0.5f, p), 0.0f);
    CHECK_EQ(aim::deadzone_x(3.0f, p), 3.0f);
    CHECK_EQ(aim::deadzone_y(1.9f, p), 0.0f);
    CHECK_EQ(aim::deadzone_y(2.1f, p), 2.1f);
}

// ---------------------------------------------------------------------------
// 7. RateLimit（±127 拆包）
// ---------------------------------------------------------------------------
TEST(mouse_rate_limit_split_reports) {
    aim::RateLimiter lim;
    // 单次 300 超出 127 → 第一包 127，pending 173
    auto s1 = lim.step(300, 0, 127);
    CHECK_EQ(s1.dx, 127);
    CHECK_EQ(s1.pending_dx, 173);
    // 第二包继续消耗
    auto s2 = lim.step(0, 0, 127);
    CHECK_EQ(s2.dx, 127);
    CHECK_EQ(s2.pending_dx, 46);
    auto s3 = lim.step(0, 0, 127);
    CHECK_EQ(s3.dx, 46);
    CHECK_EQ(s3.pending_dx, 0);
    // 负方向
    lim.reset();
    auto sn = lim.step(-300, -5, 127);
    CHECK_EQ(sn.dx, -127);
    CHECK_EQ(sn.dy, -5);
    CHECK_EQ(sn.pending_dx, -173);
}

// ---------------------------------------------------------------------------
// 8. MotionMerge（物理 + AI；block 屏蔽；int16 clamp）
// ---------------------------------------------------------------------------
TEST(mouse_merge_no_ai_passthrough_unchanged) {
    // AI 未启用（ai=0）→ 物理透传不变
    aim::PhysicalMotion phys; phys.dx = 10; phys.dy = -5; phys.buttons = 1;
    aim::AiMove ai; ai.dx = 0; ai.dy = 0;
    auto m = aim::MotionMerge::merge(phys, ai, false, false);
    CHECK_EQ(m.dx, 10);
    CHECK_EQ(m.dy, -5);
    CHECK_EQ(m.buttons, 1u);
}

TEST(mouse_merge_ai_only) {
    aim::PhysicalMotion phys;  // 全零
    aim::AiMove ai; ai.dx = 20; ai.dy = -20;
    auto m = aim::MotionMerge::merge(phys, ai, false, false);
    CHECK_EQ(m.dx, 20);
    CHECK_EQ(m.dy, -20);
}

TEST(mouse_merge_ai_plus_physical) {
    aim::PhysicalMotion phys; phys.dx = 10; phys.dy = 5;
    aim::AiMove ai; ai.dx = 20; ai.dy = -20;
    auto m = aim::MotionMerge::merge(phys, ai, false, false);
    CHECK_EQ(m.dx, 30);
    CHECK_EQ(m.dy, -15);
}

TEST(mouse_merge_block_x_and_y) {
    aim::PhysicalMotion phys; phys.dx = 10; phys.dy = 5;
    aim::AiMove ai; ai.dx = 20; ai.dy = -20;
    // X block：物理 X 被屏蔽，Y 保留
    auto mx = aim::MotionMerge::merge(phys, ai, true, false);
    CHECK_EQ(mx.dx, 20);
    CHECK_EQ(mx.dy, -15);
    // Y block
    auto my = aim::MotionMerge::merge(phys, ai, false, true);
    CHECK_EQ(my.dx, 30);
    CHECK_EQ(my.dy, -20);
    // 全 block
    auto mb = aim::MotionMerge::merge(phys, ai, true, true);
    CHECK_EQ(mb.dx, 20);
    CHECK_EQ(mb.dy, -20);
}

TEST(mouse_merge_int16_clamp) {
    aim::PhysicalMotion phys; phys.dx = 32000;
    aim::AiMove ai; ai.dx = 2000;
    auto m = aim::MotionMerge::merge(phys, ai, false, false);
    CHECK_EQ(m.dx, 32767);
    aim::PhysicalMotion pn; pn.dx = -32000;
    aim::AiMove an; an.dx = -2000;
    auto mn = aim::MotionMerge::merge(pn, an, false, false);
    CHECK_EQ(mn.dx, -32768);
    // 未溢出：-32000 + 2000 = -30000（int16 内，不 clamp）
    aim::AiMove ai2; ai2.dx = 2000;
    auto m2 = aim::MotionMerge::merge(pn, ai2, false, false);
    CHECK_EQ(m2.dx, -30000);
}

// ---------------------------------------------------------------------------
// 9. AimState（78ms 丢失宽限 / 超时 Reset / 热键）
// ---------------------------------------------------------------------------
TEST(mouse_aim_state_lost_grace_recovery) {
    aim::AimStateMachine sm;
    aim::AimStateEvent ev;
    ev.hotkey_active = true;
    ev.now_ms = 1000;
    ev.has_target = true;
    CHECK(sm.update(ev, 78.0f));          // IDLE → AIMING（进入重置）
    CHECK(sm.state() == aim::AimState::kAiming);
    ev.has_target = false;
    CHECK(!sm.update(ev, 78.0f));         // AIMING → LOST_GRACE
    CHECK(sm.state() == aim::AimState::kLostGrace);
    ev.has_target = true;
    ev.now_ms = 1030;                     // 30ms 内找回
    CHECK(!sm.update(ev, 78.0f));
    CHECK(sm.state() == aim::AimState::kAiming);
}

TEST(mouse_aim_state_lost_grace_timeout_reset) {
    aim::AimStateMachine sm;
    aim::AimStateEvent ev;
    ev.hotkey_active = true;
    ev.has_target = true;
    ev.now_ms = 1000;
    sm.update(ev, 78.0f);
    CHECK(sm.state() == aim::AimState::kAiming);
    ev.has_target = false;
    sm.update(ev, 78.0f);
    ev.now_ms = 1100;                     // 超过 78ms
    const bool reset = sm.update(ev, 78.0f);
    CHECK(reset);                          // 超时 → IDLE + Reset
    CHECK(sm.state() == aim::AimState::kIdle);
}

TEST(mouse_aim_state_hotkey_release) {
    aim::AimStateMachine sm;
    aim::AimStateEvent ev;
    ev.hotkey_active = true;
    ev.has_target = true;
    ev.now_ms = 1000;
    sm.update(ev, 78.0f);
    CHECK(sm.state() == aim::AimState::kAiming);
    ev.hotkey_active = false;
    CHECK(sm.update(ev, 78.0f));           // 松开 → IDLE + Reset
    CHECK(sm.state() == aim::AimState::kIdle);
}

TEST(mouse_aim_state_reset_clears_lock_and_confirmation) {
    aim::AimStateMachine sm;
    aim::LockConfirmConfig cfg;
    cfg.confirmation_frames = 1;
    cfg.enter_conf = 0.8f;
    cfg.hold_conf = 0.2f;
    cfg.instant_enter_enabled = false;

    aim::AimStateEvent ev;
    ev.hotkey_active = true;
    ev.has_target = true;
    ev.now_ms = 1000;
    ev.target_confidence = 0.9f;
    CHECK(sm.update(ev, 78.0f, cfg));
    CHECK(sm.state() == aim::AimState::kAiming);

    sm.reset();
    CHECK(sm.state() == aim::AimState::kIdle);
    // reset 后不能沿用旧 HOLD 阈值；低于 enter_conf 的新世代首帧必须留在 selecting。
    ev.now_ms = 2000;
    ev.target_confidence = 0.3f;
    CHECK(!sm.update(ev, 78.0f, cfg));
    CHECK(sm.state() == aim::AimState::kSelecting);
}

TEST(mouse_target_selector_reset_restarts_track_identity) {
    aim::TargetSelector selector;
    aim::TargetSelectorConfig cfg;
    cfg.roi_w = 320;
    cfg.roi_h = 320;
    cfg.confidence = 0.1f;
    DetectionBox box;
    box.x1 = 140; box.y1 = 120; box.x2 = 180; box.y2 = 200;
    box.score = 0.9f; box.class_id = 0;
    const auto first = selector.select({box}, cfg, 1000);
    CHECK(first.valid);
    CHECK_EQ(first.target_id, 1);
    selector.reset();
    const auto next_generation = selector.select({box}, cfg, 2000);
    CHECK(next_generation.valid);
    CHECK_EQ(next_generation.target_id, 1);
}

// ---------------------------------------------------------------------------
// 10. RuntimeProfile mouse 段序列化/反序列化
// ---------------------------------------------------------------------------
TEST(mouse_runtime_profile_json_roundtrip) {
    RuntimeProfile p;
    p.mouse.enabled = true;
    p.mouse.aim_profiles[0].hotkey = 0x02;
    p.mouse.fov_range = 0.41f;
    p.mouse.aim_alpha = 0.4f;
    p.mouse.aim_gain = 17.0f;
    p.mouse.aim_max_move = 10.0f;
    p.mouse.aim_deadzone_ratio = 0.3f;
    p.mouse.output_scale = 1.0f;
    p.mouse.deadzone_x = 1.0f;
    p.mouse.lost_grace_ms = 78.0f;
    p.mouse.gain_x_px_per_count = 0.42f;   // 自动标定产物（px/count）必须落盘生效
    p.mouse.gain_y_px_per_count = 0.71f;
    p.mouse.response_delay_ms = 51.0f;   // V3 阶段5 前置：实测回路延迟（板端 51ms）
    // V3 阶段5：抖动前馈扣除（默认关，这里显式打开验证往返）
    p.mouse.jitter_feedforward.enabled = true;
    p.mouse.jitter_feedforward.delay_ms = 0.0f;          // 0 = 用 response_delay_ms
    p.mouse.jitter_feedforward.gain_px_per_count = 0.0f; // V1.0.12 起 0 = 用 mouse.gain_y（热键档 gain 已删）
    p.mouse.jitter_feedforward.scale = 0.8f;
    p.mouse.jitter_feedforward.max_px = 25.0f;
    p.mouse.fov_mode = true;
    p.mouse.hfov = 90.0f;
    p.mouse.vfov = 60.0f;
    p.mouse.move_speed_x = 700.0f;
    p.mouse.move_speed_y = 650.0f;
    aim::ClassOffset co;
    co.class_id = 0; co.offset_x = 0.48f; co.offset_y = 0.49f; co.priority = 0;
    p.mouse.aim_point.class_offsets.push_back(co);

    const JsonValue j = p.to_json();
    RuntimeProfile q = RuntimeProfile::from_json(j);
    CHECK(q.mouse.enabled);
    CHECK_EQ(q.mouse.aim_profiles.at(0).hotkey, 0x02u);
    CHECK(q.mouse.fov_range == 0.41f);
    CHECK(q.mouse.aim_alpha == 0.4f);
    CHECK(q.mouse.aim_gain == 17.0f);
    CHECK(q.mouse.aim_max_move == 10.0f);
    CHECK(q.mouse.aim_deadzone_ratio == 0.3f);
    CHECK(q.mouse.output_scale == 1.0f);
    CHECK(q.mouse.deadzone_x == 1.0f);
    CHECK(q.mouse.lost_grace_ms == 78.0f);
    CHECK(q.mouse.gain_x_px_per_count == 0.42f);
    CHECK(q.mouse.gain_y_px_per_count == 0.71f);
    CHECK(q.mouse.response_delay_ms == 51.0f);   // ★ 延迟必须落盘，否则前馈没得对齐
    CHECK(q.mouse.jitter_feedforward.enabled);            // ★ 前馈开关必须落盘
    CHECK(q.mouse.jitter_feedforward.delay_ms == 0.0f);
    CHECK(q.mouse.jitter_feedforward.gain_px_per_count == 0.0f);
    CHECK(q.mouse.jitter_feedforward.scale == 0.8f);
    CHECK(q.mouse.jitter_feedforward.max_px == 25.0f);
    CHECK(q.mouse.fov_mode);
    CHECK(q.mouse.hfov == 90.0f);
    CHECK(q.mouse.vfov == 60.0f);
    CHECK(q.mouse.move_speed_x == 700.0f);
    CHECK(q.mouse.move_speed_y == 650.0f);
    CHECK_EQ(q.mouse.aim_point.class_offsets.size(), 1u);
    CHECK(q.mouse.aim_point.class_offsets[0].offset_y == 0.49f);
    // validate 通过（合法配置）
    std::string verr;
    CHECK(q.validate(&verr));
    // 非法值被拒
    q.mouse.fov_range = 1.5f;
    CHECK(!q.validate(&verr));
}

// ---------------------------------------------------------------------------
// 附加：FovAngle（角度换算输出模式）
// ---------------------------------------------------------------------------
TEST(mouse_fov_angle_direction_and_scale) {
    // 方向：目标右侧(err>0) → 移动>0；左侧 → <0（与纯 P 一致）
    CHECK(aim::fov_move_x(50.0f, 320.0f, 83.105f, 500.0f) > 0.0f);
    CHECK(aim::fov_move_x(-50.0f, 320.0f, 83.105f, 500.0f) < 0.0f);
    CHECK(aim::fov_move_y(30.0f, 320.0f, 53.0f, 500.0f) > 0.0f);
    CHECK(aim::fov_move_y(-30.0f, 320.0f, 53.0f, 500.0f) < 0.0f);
    // 单调：误差越大移动越大
    const float s1 = aim::fov_move_x(20.0f, 320.0f, 83.105f, 500.0f);
    const float s2 = aim::fov_move_x(60.0f, 320.0f, 83.105f, 500.0f);
    CHECK(s2 > s1);
    // 速度越大移动越大
    CHECK(aim::fov_move_x(40.0f, 320.0f, 83.105f, 800.0f) >
          aim::fov_move_x(40.0f, 320.0f, 83.105f, 500.0f));
    // 非法 FOV 回退线性（不崩溃）
    CHECK(aim::fov_move_x(10.0f, 320.0f, 0.0f, 500.0f) == 10.0f);
}

// ---------------------------------------------------------------------------
// 附加：MouseRouter 解析（罗技 c53f input1：ReportID 0x02 + int16 轴）
// ---------------------------------------------------------------------------
TEST(mouse_router_parse_logitech_layout) {
    aim::MouseLayout lay;
    lay.report_id = 0x02;
    lay.buttons_offset = 1;
    lay.buttons_size = 2;
    lay.axis_offset = 3;
    lay.axis_size = 2;
    lay.wheel_offset = 7;
    // 9 字节：ReportID, buttons(2B), X(2B), Y(2B), wheel, pan
    uint8_t rep[9] = {0x02, 0x01, 0x00, 0x0A, 0x00, 0xFE, 0xFF, 0x05, 0x00};
    aim::MouseRouter router;
    aim::PhysicalMotion m;
    CHECK(router.parse(rep, sizeof(rep), 123, lay, &m));
    CHECK_EQ(m.buttons, 1u);
    CHECK_EQ(m.dx, 10);
    CHECK_EQ(m.dy, -2);
    CHECK_EQ(m.wheel, 5);
    // ReportID 不匹配 → false
    rep[0] = 0x03;
    CHECK(!router.parse(rep, sizeof(rep), 123, lay, &m));
    // 长度不足 → false
    CHECK(!router.parse(rep, 5, 123, lay, &m));
    // ★ 长度边界（2026-09-23 审查复核 #19）：轴要读 X/Y **两个**分量 ⇒ 真实需求是
    //   axis_offset + 2*axis_size = 3+4 = 7 字节，而原判据只算了一个分量（5 字节）。
    //   于是 size=6 落在「旧判据放行、真实需求不足」的窗口里，旧代码会返回 true 并越界读
    //   data[5..6]。这条就是为了钉死那个窗口——只跑现有用例测不出来（原 size=5 的用例是被
    //   ReportID 提前返回挡住的，根本没走到长度判据）。
    rep[0] = 0x02;  // 恢复 ReportID，确保拦住它的是「长度」而不是 ReportID
    CHECK(!router.parse(rep, 6, 123, lay, &m));
    // 恰好够 7 字节 → 正常解析（X=10 / Y=-2）；wheel 在 index 7，size=7 ⇒ 不读、保持默认 0
    CHECK(router.parse(rep, 7, 123, lay, &m));
    CHECK_EQ(m.dx, 10);
    CHECK_EQ(m.dy, -2);
    CHECK_EQ(m.wheel, 0);
    // INT8 轴布局（axis_size=1）同样要 2 个分量：offset(1)+2 = 3 字节；size=2 必须拒绝
    aim::MouseLayout lay8;
    lay8.report_id = 0x00;
    lay8.buttons_offset = 0;
    lay8.buttons_size = 1;
    lay8.axis_offset = 1;
    lay8.axis_size = 1;
    lay8.wheel_offset = 255;  // 无 wheel
    uint8_t rep8[3] = {0x00, 0xF6, 0x0A};  // dx=-10, dy=10
    CHECK(!router.parse(rep8, 2, 123, lay8, &m));
    CHECK(router.parse(rep8, 3, 123, lay8, &m));
    CHECK_EQ(m.dx, -10);
    CHECK_EQ(m.dy, 10);
    // 非法 axis_size（既非 1 也非 2）→ 拒绝，避免走 else 分支按 int16 读
    aim::MouseLayout bad;
    bad.axis_size = 3;
    CHECK(!router.parse(rep, sizeof(rep), 123, bad, &m));
}

// ---------------------------------------------------------------------------
// 插件：PullCurve（拉枪曲线）/ ContinuousLead（持续提前量）
// 注：Humanize（拟人微动）已于 2026-09-29 删除，见本文件下方说明。
// ---------------------------------------------------------------------------
TEST(mouse_pull_curve_activates_only_beyond_min_distance) {
    aim::PullCurveConfig cfg;  // enabled=true min_distance=80 strength=0.8
    aim::PullCurve pc;
    // 距离 50 < 80：不激活 → 附加 0
    CHECK_EQ(pc.apply(-30.0f, 40.0f, 10.0f, 5.0f, cfg, 4.0f), 0.0f);
    // 距离 100 > 80：激活（|附加| > 0）
    const float v = pc.apply(60.0f, 80.0f, 20.0f, 0.0f, cfg, 4.0f);
    CHECK(std::fabs(v) > 0.0f);
    // 方向：out_x >= 0 → 弧线方向为正
    CHECK(v > 0.0f);
    pc.reset();
}

TEST(mouse_continuous_lead_needs_accumulated_distance) {
    aim::ContinuousLeadConfig cfg;  // enabled=false 默认
    aim::ContinuousLead cl;
    // 未启用：返回 0
    CHECK_EQ(cl.apply(50, 0, 4.0f, cfg), 0.0f);
    // 启用 + 单次未达 enter_distance → 0
    cfg.enabled = true;
    cfg.enter_distance = 150.0f;
    cfg.scale = 0.5f;
    CHECK_EQ(cl.apply(50, 0, 4.0f, cfg), 0.0f);   // accum=50
    CHECK_EQ(cl.apply(50, 0, 4.0f, cfg), 0.0f);   // accum=100
    // 累计 50×3=150 ≥ 150 → 开始输出偏置
    CHECK(std::fabs(cl.apply(50, 0, 4.0f, cfg)) > 0.0f);  // accum=150
    cl.reset();
    // 复位后需重新累计
    CHECK_EQ(cl.apply(50, 0, 4.0f, cfg), 0.0f);   // accum=50
    cl.reset();
}

// 2026-09-29：Humanize（固定正弦 X 微动）整模块已删除 —— 它属于 TTBOX 自研的 4 套
// 固定正弦抖动之一，非 BB 来源。对应用例 `mouse_humanize_adds_jitter_only_when_enabled`
// 一并移除；HumanizeConfig 死结构体 2026-09-30 也已删除。

// ---------------------------------------------------------------------------
// RuntimeProfile mouse 段序列化（对齐参数/自适应死区/拉枪插件）
// ---------------------------------------------------------------------------
TEST(mouse_profile_fields_roundtrip) {
    RuntimeProfile p;
    // V1.0.41：pid1 删除，换 SmoothAimController 4 参数，这里锁往返
    p.mouse.aim_alpha = 0.6f;
    p.mouse.aim_gain = 0.42f;
    p.mouse.aim_max_move = 19.0f;
    p.mouse.aim_deadzone_ratio = 0.7f;
    p.mouse.output_deadzone = 1.5f;
    p.mouse.pull_curve.enabled = true;
    p.mouse.pull_curve.strength = 0.9f;
    p.mouse.pull_curve.jitter_px = 2.5f;
    p.mouse.pull_curve.min_distance = 100.0f;

    const JsonValue j = p.to_json();
    const RuntimeProfile q = RuntimeProfile::from_json(j);
    CHECK(q.mouse.aim_alpha == 0.6f);
    CHECK(q.mouse.aim_gain == 0.42f);
    CHECK(q.mouse.aim_max_move == 19.0f);
    CHECK(q.mouse.aim_deadzone_ratio == 0.7f);
    CHECK(q.mouse.output_deadzone == 1.5f);
    CHECK(q.mouse.pull_curve.enabled);
    CHECK(q.mouse.pull_curve.strength == 0.9f);
    CHECK(q.mouse.pull_curve.min_distance == 100.0f);
}


// ---------------------------------------------------------------------------
// 编译期守卫：落点入口唯一化（V1.0.13）
//   · AimPointProfile::aim_offset_x/y —— 准星像素偏移（第二个落点入口）不得存在
//   · CaptureProfile::offset_x/y     —— 裁剪区偏移（第三个落点入口）不得存在
// ---------------------------------------------------------------------------
template <typename T, typename = void>
struct has_aim_offset_field : std::false_type {};
template <typename T>
struct has_aim_offset_field<T, decltype(void(std::declval<T&>().aim_offset_x))>
    : std::true_type {};

template <typename T, typename = void>
struct has_capture_offset_field : std::false_type {};
template <typename T>
struct has_capture_offset_field<T, decltype(void(std::declval<T&>().offset_x))>
    : std::true_type {};

static_assert(!has_aim_offset_field<aim::AimPointProfile>::value,
              "V1.0.13：AimPointProfile 不得再有 aim_offset_x（落点只留瞄点一个入口）");
static_assert(!has_capture_offset_field<CaptureProfile>::value,
              "V1.0.13：CaptureProfile 不得再有 offset_x（裁剪区恒居中）");

// ★★★ V1.0.41：pid1 删除，缺省值换 SmoothAimController 4 参数
//   （alpha=0.5 / gain=0.15 / max_move=30 / deadzone_ratio=0.05）。
//   ⚠ 业主 2026-10-05 决定不写迁移：老配置升级后需重跑标定，不保兼容。
TEST(mouse_defaults_are_smooth_aim) {
    auto p = json_parse(R"({"mouse":{"enabled":true}})");
    CHECK(p.ok);
    if (!p.ok) return;
    const RuntimeProfile t = RuntimeProfile::from_json(p.value);
    CHECK(std::fabs(t.mouse.aim_alpha - 0.5f) < 1e-6f);
    CHECK(std::fabs(t.mouse.aim_gain - 0.15f) < 1e-6f);
    CHECK(std::fabs(t.mouse.aim_max_move - 30.0f) < 1e-6f);
    CHECK(std::fabs(t.mouse.aim_deadzone_ratio - 0.05f) < 1e-6f);
    CHECK(t.to_json().dump().find("\"aim_gain\"") != std::string::npos);
}

// ---------------------------------------------------------------------------
// 持续提前量（continuous_lead）配置往返 —— M2 补齐的断点
//
// 背景：ContinuousLeadConfig 结构体与 ContinuousLead.hpp 算法 + 本文件
// mouse_continuous_lead_needs_accumulated_distance 单测**早就存在**，
// 但 RuntimeProfile::MouseProfile 缺 continuous_lead 成员、AimThread 亦未调用
// ⇒ 该功能**永远无法通过配置开启**（算法在、管线断）。
// 本用例锁死"配置能存能读"，防此断点再次出现。
// ---------------------------------------------------------------------------
TEST(mouse_continuous_lead_profile_roundtrip) {
    RuntimeProfile p;
    p.mouse.continuous_lead.enabled = true;
    p.mouse.continuous_lead.enter_distance = 180.0f;
    p.mouse.continuous_lead.scale = 0.7f;
    p.mouse.continuous_lead.fade_in_ms = 250.0f;
    p.mouse.continuous_lead.fade_out_ms = 400.0f;
    p.mouse.continuous_lead.near_disable_ratio = 0.5f;

    const JsonValue j = p.to_json();
    const RuntimeProfile q = RuntimeProfile::from_json(j);
    CHECK(q.mouse.continuous_lead.enabled);
    CHECK(q.mouse.continuous_lead.enter_distance == 180.0f);
    CHECK(q.mouse.continuous_lead.scale == 0.7f);
    CHECK(q.mouse.continuous_lead.fade_in_ms == 250.0f);
    CHECK(q.mouse.continuous_lead.fade_out_ms == 400.0f);
    CHECK(q.mouse.continuous_lead.near_disable_ratio == 0.5f);
}

TEST(mouse_continuous_lead_defaults_off_and_backward_compatible) {
    // ① 默认必须"关"：这是新插件不得改变既有瞄准行为的安全默认。
    RuntimeProfile d;
    CHECK(!d.mouse.continuous_lead.enabled);
    CHECK(d.mouse.continuous_lead.enter_distance == 150.0f);
    CHECK(d.mouse.continuous_lead.scale == 0.5f);

    // ② 旧配置 / 旧预设文件里没有 continuous_lead 键 ⇒ 加载后仍为"关"，
    //    且取与 yu 对齐的默认值（yu config.json: enabled=false,
    //    enter_distance=150, scale=0.5, fade_in/out=300, near_disable_ratio=0.66）。
    const RuntimeProfile q = RuntimeProfile::from_json(JsonValue::object());
    CHECK(!q.mouse.continuous_lead.enabled);
    CHECK(q.mouse.continuous_lead.enter_distance == 150.0f);
    CHECK(q.mouse.continuous_lead.scale == 0.5f);
    CHECK(q.mouse.continuous_lead.fade_in_ms == 300.0f);
    CHECK(q.mouse.continuous_lead.fade_out_ms == 300.0f);
    CHECK(q.mouse.continuous_lead.near_disable_ratio == 0.66f);
}
