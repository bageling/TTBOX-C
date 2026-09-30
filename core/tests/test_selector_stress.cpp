// test_selector_stress.cpp — C6：TargetSelector 压力测试（防横跳专项）。
// 场景：单目标 / 多目标 / 同Y / 同X / 交叉 / 突现 / 消失 / 抖动 / 排序变化。
// 断言核心：激活后 target_id 不横跳（宽限内保持原目标，不切换到邻近目标）。
#include <cmath>
#include <vector>

#include "mouse/TargetSelector.hpp"
#include "test_util.hpp"

using namespace ttbox::core;
using namespace ttbox::core::aim;

namespace {

TargetSelectorConfig make_cfg() {
    TargetSelectorConfig c;
    c.fov_range = 1.0f;
    c.confidence = 0.25f;
    c.roi_w = 640;
    c.roi_h = 480;
    c.lost_grace_ms = 30.0f;
    return c;
}

DetectionBox box(float cx, float cy, float w, float h) {
    DetectionBox b;
    b.x1 = cx - w * 0.5f; b.y1 = cy - h * 0.5f;
    b.x2 = cx + w * 0.5f; b.y2 = cy + h * 0.5f;
    b.score = 0.9f;
    b.class_id = 0;
    return b;
}

}  // namespace

// 单目标抖动：目标框每帧 ±3px 抖动，track id 必须稳定
TEST(selector_single_target_jitter_stable_id) {
    TargetSelector sel;
    auto cfg = make_cfg();
    int first_id = -1;
    for (int i = 0; i < 60; ++i) {
        const float jx = static_cast<float>((i % 7) - 3);
        const float jy = static_cast<float>((i % 5) - 2);
        auto r = sel.select({box(320 + jx, 240 + jy, 60, 120)}, cfg, static_cast<uint32_t>(i * 7));
        CHECK(r.valid);
        if (i == 10) first_id = r.target_id;
        if (i >= 10) CHECK_EQ(r.target_id, first_id);  // 锁定后不换 id
    }
}

// 双目标交叉：A 从右向左、B 从左向右穿过 —— 锁定的目标不能中途跳到另一个
TEST(selector_crossing_targets_no_switch) {
    TargetSelector sel;
    auto cfg = make_cfg();
    int locked = -1;
    for (int i = 0; i < 80; ++i) {
        // 初始 A=500（近右），B=200（左）——最近的是 A，锁定 A
        const float ax = 500.0f - static_cast<float>(i) * 5.0f;  // A 向左
        const float bx = 200.0f + static_cast<float>(i) * 5.0f;  // B 向右
        auto r = sel.select({box(ax, 240, 60, 120), box(bx, 240, 60, 120)}, cfg,
                            static_cast<uint32_t>(i * 7));
        CHECK(r.valid);
        if (i == 5) locked = r.target_id;
        // 锁定后即使两目标交叉（i≈30 时 ax≈bx≈350），id 不能变
        if (i >= 5) CHECK_EQ(r.target_id, locked);
    }
}

// 目标消失：空帧时 selector 返回 invalid（安全语义：不凭旧坐标输出移动），
// 恢复检测后 id 延续（track 仍在宽限内，经 rect_lock/track 复用）。
TEST(selector_target_gone_then_reacquire) {
    TargetSelector sel;
    auto cfg = make_cfg();
    auto r1 = sel.select({box(320, 240, 60, 120)}, cfg, 100);
    CHECK(r1.valid);
    const int id1 = r1.target_id;
    // 空帧：invalid（安全红线：不允许旧坐标漂移）
    auto r2 = sel.select({}, cfg, 115);
    CHECK(!r2.valid);
    // 宽限内目标重现：track 延续，id 不变（不重建新 track）
    auto r3 = sel.select({box(322, 242, 60, 120)}, cfg, 125);
    CHECK(r3.valid);
    CHECK_EQ(r3.target_id, id1);
}

// 目标丢失宽限：空帧立即 invalid（安全），但 track 保留；宽限内恢复 → 同 id 延续；
// 超过宽限（lost_frames*7 >= grace+7）→ 放弃激活，重新出现 → 重新选择。
TEST(selector_target_lost_grace_exhausted) {
    TargetSelector sel;
    auto cfg = make_cfg();  // lost_grace_ms=30 → 空帧宽限 ≈5 帧
    auto r1 = sel.select({box(320, 240, 60, 120)}, cfg, 100);
    CHECK(r1.valid);
    const int id1 = r1.target_id;
    // 宽限内（约 2 个空帧）恢复 → 同 id 延续（track 未删）
    sel.select({}, cfg, 110);                       // 空帧1
    auto r2 = sel.select({box(321, 241, 60, 120)}, cfg, 115);
    CHECK(r2.valid);
    CHECK_EQ(r2.target_id, id1);
    // 超过宽限：连续 10 个空帧（≈70ms > 30ms）→ 最后一次 invalid（激活已放弃）
    for (int i = 0; i < 10; ++i) {
        sel.select({}, cfg, static_cast<uint32_t>(200 + i * 7));
    }
    auto r_last = sel.select({}, cfg, 300);
    CHECK(!r_last.valid);
    // 目标重新出现：能重新选择（valid）
    auto r_new = sel.select({box(320, 240, 60, 120)}, cfg, 350);
    CHECK(r_new.valid);
}

// 目标排序变化（vector 顺序翻转）：track id 仍按位置匹配，不因排序变化换人
TEST(selector_detection_order_flip_stable) {
    TargetSelector sel;
    auto cfg = make_cfg();
    int locked = -1;
    for (int i = 0; i < 40; ++i) {
        const float y = 200.0f + static_cast<float>(i % 4) * 2.0f;
        std::vector<DetectionBox> dets;
        // 每 4 帧换一次顺序
        if ((i / 4) % 2 == 0) {
            dets = {box(260, y, 50, 100), box(380, y, 50, 100)};
        } else {
            dets = {box(380, y, 50, 100), box(260, y, 50, 100)};
        }
        auto r = sel.select(dets, cfg, static_cast<uint32_t>(i * 7));
        CHECK(r.valid);
        if (i == 10) locked = r.target_id;
        if (i >= 10) CHECK_EQ(r.target_id, locked);
    }
}

// 目标突现：空场 60ms 后新目标出现，能正常捕获（不卡死）
TEST(selector_target_appears_after_idle) {
    TargetSelector sel;
    auto cfg = make_cfg();
    for (int i = 0; i < 10; ++i) {
        auto r = sel.select({}, cfg, static_cast<uint32_t>(i * 7));
        CHECK(!r.valid);
    }
    auto r = sel.select({box(320, 240, 60, 120)}, cfg, 100);
    CHECK(r.valid);
    CHECK(r.target_id >= 0);
}

// 目标跳变（teleport 到远处）：应切换目标（rect_lock 匹配失败 → 新 acquire），不卡死
TEST(selector_target_teleport_reattach) {
    TargetSelector sel;
    auto cfg = make_cfg();
    auto r1 = sel.select({box(300, 260, 60, 120)}, cfg, 100);
    CHECK(r1.valid);
    const int id1 = r1.target_id;
    // 目标瞬间跳到 500,400（超出 rect_lock 匹配半径）
    auto r2 = sel.select({box(500, 400, 60, 120)}, cfg, 130);
    CHECK(r2.valid);
    // 允许换 id（teleport 语义）——重点是 valid 且不崩
    (void)id1;
    CHECK(r2.target_id >= 0);
}

// priority 排序 —— 启用优先级后，远处高优先目标胜过近处普通目标
TEST(selector_priority_classes_win) {
    TargetSelector sel;
    auto cfg = make_cfg();
    cfg.priority_enabled = true;
    cfg.priority_classes_high = {5};   // class 5（bus）高优先
    // 近处普通目标（class 0, 距中心近）+ 远处高优先目标（class 5, 距中心远）
    const auto near_common = box(330, 240, 60, 120);   // class 0，近
    const auto far_prio   = box(500, 380, 60, 120);    // class 5，远
    // 手动构造不同 class：box() 默认 class 0，需单独设 class
    auto b1 = near_common; b1.class_id = 0;
    auto b2 = far_prio;   b2.class_id = 5;
    auto r = sel.select({b1, b2}, cfg, 100);
    CHECK(r.valid);
    // 高优先 class 5 应被选中（即使更远）
    CHECK_EQ(r.box.class_id, 5);
}

// 未启用 priority 时保持"距离最近优先"（兼容旧行为）
TEST(selector_priority_disabled_distance_wins) {
    TargetSelector sel;
    auto cfg = make_cfg();
    cfg.priority_enabled = false;  // 默认
    auto b1 = box(330, 240, 60, 120); b1.class_id = 0;  // 近
    auto b2 = box(500, 380, 60, 120); b2.class_id = 5;  // 远
    auto r = sel.select({b1, b2}, cfg, 100);
    CHECK(r.valid);
    CHECK_EQ(r.box.class_id, 0);  // 距离最近优先
}

// 激活切换后旧 track 必须释放：连续 teleport 到不同位置应始终只有一个激活轨迹，
// 且轨迹总量受 max_tracks 上限约束（旧实现 active 轨迹只增不清）。
TEST(selector_switch_releases_old_active_tracks) {
    TargetSelector sel;
    auto cfg = make_cfg();
    for (int i = 0; i < 150; ++i) {
        // 每帧把目标放到不同远处位置，强制 score 层不断新建/切换 track。
        const float cx = 200.0f + static_cast<float>(i % 60) * 5.0f;
        const float cy = 160.0f + static_cast<float>((i / 60) % 40) * 3.0f;
        auto r = sel.select({box(cx, cy, 40, 80)}, cfg, static_cast<uint32_t>(100 + i * 7));
        CHECK(r.valid);
        size_t active_count = 0;
        for (const auto& t : sel.tracks()) {
            if (t.active) ++active_count;
        }
        CHECK_EQ(active_count, 1u);
        CHECK(sel.tracks().size() <= static_cast<size_t>(cfg.max_tracks));
    }
}

// ---- 切靶防抖（对齐 BB switch_cooldown）----
// 多候选场景：切换发生后冷却期内不再另选目标；冷却过后恢复正常。
// 单候选不受影响（目标瞬移/高速移动走那条路，见上一个用例）。
TEST(selector_switch_cooldown_blocks_multi_candidate) {
    TargetSelector sel;
    auto cfg = make_cfg();
    cfg.switch_cooldown_ms = 600.0f;
    cfg.switch_hysteresis = 0.0f;  // 只验冷却
    cfg.lost_grace_ms = 0.0f;      // 排除宽限干扰：一丢就允许重选
    auto r0 = sel.select({box(320, 240, 60, 120), box(460, 300, 60, 120)}, cfg, 0);
    CHECK(r0.valid);
    // 冷却内：原目标瞬移到远处（第 1/2 层都匹配不上），另一个候选仍在 ⇒ 不切
    auto r1 = sel.select({box(150, 180, 60, 120), box(460, 300, 60, 120)}, cfg, 100);
    CHECK(!r1.valid);
    // 冷却过：恢复正常选择
    auto r2 = sel.select({box(150, 180, 60, 120), box(460, 300, 60, 120)}, cfg, 700);
    CHECK(r2.valid);
}

// ---- 切靶滞后（对齐 BB target_switch_hysteresis）----
// 新目标必须比刚失去的锁定目标明显更近才允许切；滞后置 0 时行为与加入前一致。
TEST(selector_switch_hysteresis_blocks_far_candidate) {
    // ★ 两个候选都必须落在 FOV 半径内（中心 320,240，半径 240），否则会被
    //   collect_candidates 过滤掉 ⇒ 只剩单候选，守卫按设计不拦（那不是"切换"）。
    const std::vector<DetectionBox> first = {box(340, 240, 60, 120), box(520, 240, 60, 120)};
    // A(340,240) 距中心 20px；随后 A 瞬移到 (150,180)，剩下的候选都比它远
    const std::vector<DetectionBox> second = {box(150, 180, 60, 120), box(520, 240, 60, 120)};
    {
        TargetSelector sel;
        auto cfg = make_cfg();
        cfg.switch_cooldown_ms = 0.0f;
        cfg.switch_hysteresis = 0.5f;
        cfg.lost_grace_ms = 0.0f;
        // V1.0.11：本用例专测切靶滞后 ⇒ 关掉切靶确认窗，
        //   否则两种机制都会让结果 invalid，判不出是谁挡的。
        cfg.switch_confirm_frames = 0;
        CHECK(sel.select(first, cfg, 0).valid);
        // B 的 dist_sq=10000，锁定基准 400：10000×1.5² = 22500 不小于 400 ⇒ 挡
        CHECK(!sel.select(second, cfg, 1000).valid);
    }
    {
        TargetSelector sel;  // 对照：滞后关掉，同样输入必须能选上
        auto cfg = make_cfg();
        cfg.switch_cooldown_ms = 0.0f;
        cfg.switch_hysteresis = 0.0f;
        cfg.lost_grace_ms = 0.0f;
        cfg.switch_confirm_frames = 0;   // V1.0.11：同上，本用例只测滞后
        CHECK(sel.select(first, cfg, 0).valid);
        CHECK(sel.select(second, cfg, 1000).valid);
    }
}
#include <cstdio>

// ── V3 阶段 4：选靶打分去量纲（2026-09-28）──
// 两个候选到中心**距离相同** ⇒ dist 项打平，胜负只由 size 项决定。
// scale = 画面整体放大倍数（所有像素量同比缩放）。
static int pick_winner_width(float scale) {
    TargetSelectorConfig cfg = make_cfg();
    cfg.roi_w = 640.0f * scale;
    cfg.roi_h = 480.0f * scale;
    cfg.priority_scoring = true;
    cfg.weight_dist = 1.0f;
    cfg.weight_size = 0.3f;
    const float cx = cfg.roi_w * 0.5f;
    const float cy = cfg.roi_h * 0.5f;
    const float d = 100.0f * scale;
    TargetSelector sel;
    const auto r = sel.select({box(cx - d, cy, 100.0f * scale, 150.0f * scale),
                               box(cx + d, cy, 150.0f * scale, 200.0f * scale)}, cfg, 0);
    return r.valid ? static_cast<int>(r.box.x2 - r.box.x1 + 0.5f) : -1;
}

// 尺寸项原式 min(1, w×h/10000)：两个候选面积 15000 与 30000 **都撞顶拿 1.0**
// ⇒ size 项等于没配，只能看 dist（这里打平）⇒ 取先出现的那个（宽 100）。
// 改 sqrt(面积)/参考边长后，大目标（宽 150）应当靠尺寸胜出。
TEST(selector_size_score_no_longer_saturates) {
    CHECK(pick_winner_width(1.0f) == 150);
}

// ★ V1.0.12（2026-09-30）：原先「两项都除以本档倍镜真实倍率 M」的归一化已删
//   （业主口径：不区分倍镜）。本用例锁**删除后**的真实行为：
//   画面整体放大 ⇒ 尺寸项全部撞顶 1.0、距离项被同一组权重压小 ⇒ 两者同分，
//   退回"谁先谁赢" ⇒ 选出的是先出现的那个小目标（宽 100×6）。
//   ★ 已知取舍：板端无影响 —— zoom_scale 一直是 1，归一化本来就没生效过。
//     真要让打分对倍镜不敏感，正确做法是让 size_ref_px / dist_ref_px 跟着
//     画面尺度走，而不是在打分里除一个需要人工填的 M（那正是被删掉的"参考物"）。
TEST(selector_scoring_is_pixel_absolute_after_zoom_removal) {
    const int hip = pick_winner_width(1.0f);
    const int scaled = pick_winner_width(6.0f);
    CHECK(hip == 150);         // 腰射：大目标靠尺寸胜出
    CHECK(scaled == 100 * 6);  // 放大 6 倍：尺寸项撞顶 ⇒ 同分 ⇒ 取先出现的
}

int main() {
    return ttbox_test::run_all();
}
