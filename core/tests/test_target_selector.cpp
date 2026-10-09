// test_target_selector.cpp — TargetSelector 选靶行为测试
//
// ★ 为什么补这个测试（2026-10-03）：
//   TargetSelector 是「错了不会崩」的那一类——选错目标不产生异常，
//   只会让手感变差（跟不跟、跟错人、抖动）。V1.0.13 删 BB 时它零引用，
//   但它是生产唯一选靶实现（AimThread 每帧调它），却没有专测。
//   本文件锁的是**几何/阈值类的静默行为**——这些最容易在改动中被带坏。
//
// ★ 2026-10-08 新骨架对齐：select() 从「三层瀑布」换成
//   「单层打分 + 单一 IoU 判定 + 锁定保持」。本文件同步把 14 条用例钉到新骨架，
//   另补 1 条锁定保持用例（锁住“丢失不返回 invalid、保持最后位置”）。
//
// 断言口径：CHECK/ TEST（见 tests/test_util.hpp）。★ 不用裸 assert：
//   Release 下 NDEBUG 会把 assert 编译掉 ⇒ 测试变空操作、恒绿。
#include "test_util.hpp"

#include <cmath>
#include "mouse/TargetSelector.hpp"

using namespace ttbox::core;
using namespace ttbox::core::aim;   // ★ TargetSelector 在 aim 命名空间（不是 mouse）

namespace {

DetectionBox make_box(float cx, float cy, float w, float h, float score = 0.9f,
                      int class_id = 0) {
    DetectionBox b;
    b.x1 = cx - w * 0.5f;
    b.y1 = cy - h * 0.5f;
    b.x2 = cx + w * 0.5f;
    b.y2 = cy + h * 0.5f;
    b.score = score;
    b.class_id = class_id;
    return b;
}

// 默认配置：ROI 640x640、fov_range=1.0⇒ 搜索半径 320、置信度 0.25。
TargetSelectorConfig base_cfg() {
    TargetSelectorConfig cfg;
    cfg.roi_w = 640;
    cfg.roi_h = 640;
    cfg.center_x = 0.5f;
    cfg.center_y = 0.5f;
    cfg.fov_range = 1.0f;
    cfg.confidence = 0.25f;
    // 贴边/轨迹相关的默认值在测试里显式关掉，避免默认策略干扰被测行为
    cfg.reject_clip_horizontal = false;
    cfg.reject_clip_top = false;
    // ★ search_radius_px = 裁剪区半宽（AimThread 用 min(capture.w, capture.h)/2 填，
    //   板端 640×640 ⇒ 320）。★ 不填它（默认 0）会让"贴边剔除"整条路径直接 return false
    //   —— 即 TargetSelector.cpp:126 `crop_half <= 0 ⇒ return false`。
    //   下面两条贴边用例全靠它才真正生效，漏填会得到"永远不剔"的假绿。
    cfg.search_radius_px = 320.0f;
    // 锁定保持默认关（0）：几何/生命周期用例不受锁定保持干扰；锁定保持单独开一条用例。
    cfg.lock_hold_ms = 0.0f;
    return cfg;
}

}  // namespace

// ---------- 基本：空输入 / 低置信度 / 正常命中 ----------

TEST(target_selector_empty_input_is_invalid) {
    TargetSelector sel;
    const auto r = sel.select({}, base_cfg(), 0);
    CHECK(!r.valid);
    CHECK_EQ(sel.last_reason(), TargetSelection::Reason::kNone);

    // ★ roi_w/roi_h 为 0（未接线/配置缺失）⇒ 必须 fail-closed 返回 invalid。
    //   这条要单独锁：早退分支的条件是 `dets.empty() || roi_w==0 || roi_h==0` 三合一，
    //   故障注入拆掉整个 if 后，空 dets 会掉进 `cands.empty()` 的**另一条**分支，
    //   而那条分支同样返回 invalid ⇒ 只测"空 dets"根本区分不出来（注入实测 0 报红）。
    //   roi_w=0 但**有**检测框才是能区分两者的输入：早退 ⇒ 无 track、无选择；
    //   若早退被拆 ⇒ 正常流程会建track、甚至可能选出目标。
    TargetSelector sel2;
    auto cfg0 = base_cfg();
    cfg0.roi_w = 0;
    cfg0.roi_h = 0;
    std::vector<DetectionBox> dets{make_box(100.0f, 100.0f, 40.0f, 40.0f, 0.9f)};
    const auto r2 = sel2.select(dets, cfg0, 0);
    CHECK(!r2.valid);                       // 未接线的 ROI 尺寸 ⇒ 绝不能选出目标
    CHECK_EQ(sel2.tracks().size(), 0u);     // 也不该凭空建 track
    CHECK_EQ(sel2.last_reason(), TargetSelection::Reason::kNone);
}

TEST(target_selector_rejects_low_confidence) {
    // ★ 静默行为：置信度不够的框应该被丢掉。
    //   若这里放行，用户会瞄到 score=0.05 的噪声框，且没有任何报错。
    TargetSelector sel;
    const auto cfg = base_cfg();
    std::vector<DetectionBox> dets{make_box(320.0f, 320.0f, 40.0f, 40.0f, 0.05f)};
    const auto r = sel.select(dets, cfg, 0);
    CHECK(!r.valid);           // 0.05 <阈值 0.25
}

TEST(target_selector_picks_single_confident_box) {
    TargetSelector sel;
    const auto cfg = base_cfg();
    std::vector<DetectionBox> dets{make_box(300.0f, 320.0f, 40.0f, 40.0f, 0.9f)};
    const auto r = sel.select(dets, cfg, 0);
    CHECK(r.valid);
    CHECK(r.target_id >= 0);
}

// ---------- ROI 半径：搜索半径外必须丢 ----------

TEST(target_selector_rejects_box_outside_fov_radius) {
    // ★ 核心静默行为：FOV 半径=320px（ROI 640 的 fov_range=1.0 ⇒半宽）。
    //   准星在 (320,320)，把框放到 x=600（距 280px）**之内**⇒ 应命中；
    //   放到 x=900（距 580px）**之外** ⇒ 应被丢掉。
    TargetSelector sel;
    const auto cfg = base_cfg();
    {
        std::vector<DetectionBox> dets{make_box(600.0f, 320.0f, 40.0f, 40.0f, 0.9f)};
        CHECK(sel.select(dets, cfg, 0).valid);          // 距280 < 320
    }
    sel.reset();
    {
        std::vector<DetectionBox> dets{make_box(900.0f, 320.0f, 40.0f, 40.0f, 0.9f)};
        CHECK(!sel.select(dets, cfg, 0).valid);         // 距 580 > 320
    }
}

// ---------- 贴边剔除：只贴左边/右边，贴上边默认不剔 ----------

TEST(target_selector_clip_reject_horizontal_only) {
    // ★ 静默行为：框左边缘紧贴裁剪区左界⇒ 被判"被切断"⇒ 剔除。
    //   若误放行，目标已被裁掉一半，瞄准会持续往画面外拉。
    TargetSelector sel;
    auto cfg = base_cfg();
    cfg.reject_clip_horizontal = true;
    cfg.clip_margin_px = 6.0f;
    cfg.clip_center_max_px = 105.0f;   // 离准星太近豁免

    //框贴左边（x1≈0），中心在准星右侧远处⇒ 不豁免 ⇒ 应被剔
    DetectionBox left_cut;
    left_cut.x1 = 0.0f; left_cut.y1 = 300.0f;
    left_cut.x2 = 40.0f; left_cut.y2 = 340.0f;
    left_cut.score = 0.9f;
    std::vector<DetectionBox> dets{left_cut};
    CHECK(!sel.select(dets, cfg, 0).valid);

    // 准星在 (320,320)：上面的框中心 (20,320) 距准星 300 > 105 ⇒ 不豁免。
    // 换中心离准星 < 105 的框 ⇒ 豁免 ⇒ 应命中。
    sel.reset();
    DetectionBox very_near;
    very_near.x1 = 250.0f; very_near.y1 = 300.0f;
    very_near.x2 = 290.0f; very_near.y2 = 340.0f;   // 中心 (270,320) 距准星 50
    very_near.score = 0.9f;
    std::vector<DetectionBox> dets3{very_near};
    CHECK(sel.select(dets3, cfg, 0).valid);        // 近处豁免
}

TEST(target_selector_clip_top_reject_is_off_by_default) {
    // ★ 默认 reject_clip_top=false ⇒ 贴上边**不该**被剔（近身仰角目标易误伤）。
    //   若哪次改动把它默认打开，近距离仰角目标会凭空消失（静默）。
    TargetSelector sel;
    auto cfg = base_cfg();
    CHECK(cfg.reject_clip_top == false);
    DetectionBox top_cut;
    top_cut.x1 = 300.0f; top_cut.y1 = 0.0f;
    top_cut.x2 = 340.0f; top_cut.y2 = 40.0f;
    top_cut.score = 0.9f;
    std::vector<DetectionBox> dets{top_cut};
    CHECK(sel.select(dets, cfg, 0).valid);
}

// ---------- 空帧安全红线 + 轨迹生命周期 ----------

TEST(target_selector_blank_frame_is_invalid_but_keeps_track) {
    // ★★ 空检测帧**立即返回 invalid**（源码注释写明"安全红线：不允许凭旧坐标产生移动"）。
    //   lock_hold_ms 管的是"目标还在不在候选里"的锁定保持，**不**推翻空帧安全红线；
    //   空帧只维护轨迹生命周期（丢失计数 + 锁定保持超时），绝不凭旧框产出移动。
    TargetSelector sel;
    const auto cfg = base_cfg();   // lock_hold_ms=0：丢失即放弃激活
    std::vector<DetectionBox> dets{make_box(300.0f, 320.0f, 40.0f, 40.0f, 0.9f)};
    const auto first = sel.select(dets, cfg, 0);
    CHECK(first.valid);
    const int id0 = first.target_id;
    CHECK_EQ(sel.tracks().size(), 1u);

    // 空帧（10ms）⇒ 无目标（安全红线），但 track 仍在表里（buffer 未超）
    const auto second = sel.select({}, cfg, 10);
    CHECK(!second.valid);                        // 空帧不产移动
    CHECK_EQ(sel.tracks().size(), 1u);            // buffer 内不删 track
    CHECK_EQ(sel.last_reason(), TargetSelection::Reason::kNone);

    // 同一位置恢复检测 ⇒ 打分复用旧 track（IoU 判定）⇒ 同 id 延续（手感上 = 没丢目标）
    const auto third = sel.select(dets, cfg, 20);
    CHECK(third.valid);
    CHECK_EQ(third.target_id, id0);

    // 静默超过 buffer（30 帧 × 7ms ≈ 210ms）⇒ 轨迹被回收（修"只增不删"）。
    for (int i = 0; i < 40; ++i) sel.select({}, cfg, 1021);
    CHECK_EQ(sel.tracks().size(), 0u);

    // 轨迹表规模仍被 max_tracks 截住（不无限增长）。单帧只选中一个目标 ⇒ 只建一条 track，
    // 这里是**上限护栏**的烟测：任何输入都不会让表超过上限。
    TargetSelector sel2;
    auto cfg2 = base_cfg();
    std::vector<DetectionBox> many;
    for (int i = 0; i < 100; ++i) {
        many.push_back(make_box(static_cast<float>(i * 6), 320.0f, 4.0f, 8.0f, 0.9f));
    }
    sel2.select(many, cfg2, 0);
    CHECK(sel2.tracks().size() <= static_cast<size_t>(cfg2.max_tracks));
}

// ---------- 锁定保持（新骨架 ③）：丢失不返回 invalid ----------

TEST(target_selector_lock_hold_keeps_last_position) {
    TargetSelector sel;
    auto cfg = base_cfg();
    cfg.lock_hold_ms = 1500.0f;   // 对齐 BB lock_hold_time

    const std::vector<DetectionBox> target{make_box(300.0f, 320.0f, 40.0f, 40.0f, 0.9f)};
    const auto first = sel.select(target, cfg, 0);
    CHECK(first.valid);
    const int id0 = first.target_id;

    // 空帧 → invalid（安全红线），但锁定保持窗口内 track 仍 active
    const auto blank = sel.select({}, cfg, 10);
    CHECK(!blank.valid);
    bool any_active = false;
    for (const auto& t : sel.tracks()) any_active = any_active || t.active;
    CHECK(any_active);

    // 非空但目标消失（只剩别的目标）→ 锁定保持：valid + 原 id + 原框 + held=true
    const auto held = sel.select({make_box(500.0f, 500.0f, 40.0f, 40.0f, 0.9f)}, cfg, 20);
    CHECK(held.valid);                       // ★ 不是 invalid（这正是要修的"切人甩"）
    CHECK_EQ(held.target_id, id0);           // 仍锁定原目标 ⇒ 上游不 reset PID
    CHECK(held.held);                        // 几何未更新
    const float held_cx = (held.box.x1 + held.box.x2) * 0.5f;
    CHECK(std::abs(held_cx - 300.0f) < 1e-3f);   // 输出 = 最后位置

    // 窗口内找回（IoU ≥ 0.6）→ 恢复，同 id 延续
    const auto back = sel.select(target, cfg, 40);
    CHECK(back.valid);
    CHECK_EQ(back.target_id, id0);
    CHECK(!back.held);

    // 超过锁定保持窗口（last_seen=40，保持到 1540）→ 真正放弃，重新打分换新目标
    const auto after = sel.select({make_box(500.0f, 500.0f, 40.0f, 40.0f, 0.9f)}, cfg, 1600);
    CHECK(after.valid);
    CHECK(after.target_id != id0);
}

// ---------- reset：必须把 track / 开关 / 遥测全清 ----------

TEST(target_selector_reset_clears_all_state) {
    // ★ 新骨架状态挂在 tracks_/active_track_/last_target_/selector_holds_total_ 上。
    //   reset 漏清一项 ⇒ 切模型后继承旧目标身份/旧计数（静默串味）。
    TargetSelector sel;
    auto cfg = base_cfg();
    std::vector<DetectionBox> dets{make_box(300.0f, 320.0f, 40.0f, 40.0f, 0.9f)};
    sel.select(dets, cfg, 0);
    CHECK(!sel.tracks().empty());

    sel.reset();
    CHECK(sel.tracks().empty());
    CHECK_EQ(sel.last_reason(), TargetSelection::Reason::kNone);
    CHECK_EQ(sel.selector_holds_total(), static_cast<uint64_t>(0));

    // reset 后新track 的 id 必须从 1 重新开始（禁止继承旧模型身份空间）
    const auto again = sel.select(dets, cfg, 0);
    CHECK(again.valid);
    CHECK_EQ(again.target_id, 1);
}

// ---------- 优先级：新骨架里只是同分 tie-break，不再压过距离 ----------

TEST(target_selector_priority_is_tiebreak_not_override) {
    // ★ 新骨架打分恒开启，公式 = distScore×w_dist + sizeScore×w_size + stick×0.5，
    //   **不含类别优先**。因此近处普通类 > 远处高优先类（距离主导）。
    TargetSelector sel;
    auto cfg = base_cfg();
    cfg.priority_enabled = true;
    cfg.priority_classes = {1};
    cfg.priority_classes_high = {2};

    std::vector<DetectionBox> dets{
        make_box(300.0f, 320.0f, 40.0f, 40.0f, 0.9f, /*class*/ 1),  // 近 8px、prio1
        make_box(420.0f, 320.0f, 40.0f, 40.0f, 0.9f, /*class*/ 2),  // 远 ~120px、prio2
    };
    const auto r = sel.select(dets, cfg, 0);
    CHECK(r.valid);
    CHECK_EQ(r.box.class_id, 1);   // 近者胜（打分不含类别优先）

    // ★ 同距离（对称于准星 ⇒ dist_sq 相同）时，优先级作为 tie-break 让高优先者胜出。
    TargetSelector sel2;
    auto cfg2 = base_cfg();
    cfg2.priority_enabled = true;
    cfg2.priority_classes_high = {2};
    std::vector<DetectionBox> tie{
        make_box(280.0f, 320.0f, 40.0f, 40.0f, 0.9f, /*class*/ 1),  // 普通类，dist 相同
        make_box(360.0f, 320.0f, 40.0f, 40.0f, 0.9f, /*class*/ 2),  // 高优先类，dist 相同
    };
    const auto r2 = sel2.select(tie, cfg2, 0);
    CHECK(r2.valid);
    CHECK_EQ(r2.box.class_id, 2);   // 同分 ⇒ 高优先类作为 tie-break 胜出
}

int main() {
    std::printf("=== ttbox_core tests (target_selector) ===\n");
    const int failed = ::ttbox_test::run_all();
    std::printf("=== tests done (exit=%d) ===\n", failed == 0 ? 0 : 1);
    return failed == 0 ? 0 : 1;
}


// ═══════════════════════════════════════════════════════════════════════════
// ★ V1.0.31：选靶几何兜底 + FOV 真约束（业主 2026-10-04 板端实测推动）
//
// 事故：预览里框罩在**头盔**位置、大小几乎不随远近变 ⇒ 查实选中的那个框是
//   35×31、h/w=0.88 的**训练场的球**（业界标准 sunone_aimbot 类别表：6=球），
//   而板端 class_filter 是 [0..6] 全选 ⇒ 挑中了球。
// 两条修法（都不新增面板项）：
//   ① 几何兜底：h/w 低于阈值的框（球/道具/烟雾）不参与选靶；
//   ② FOV 半径不再多乘 2（原来算出来恒等于画面半宽 ⇒ 圆等于全屏 ⇒ 零约束）。
// ═══════════════════════════════════════════════════════════════════════════
namespace {

DetectionBox shape_box(float x1, float y1, float x2, float y2, int cls) {
    DetectionBox b;
    b.x1 = x1; b.y1 = y1; b.x2 = x2; b.y2 = y2;
    b.class_id = cls;
    b.score = 0.9f;
    return b;
}

TargetSelectorConfig shape_cfg(uint32_t roi) {
    TargetSelectorConfig cfg;
    cfg.roi_w = roi;
    cfg.roi_h = roi;
    cfg.center_x = 0.5f;
    cfg.center_y = 0.5f;
    cfg.confidence = 0.25f;
    cfg.search_radius_px = static_cast<float>(roi) * 0.5f;
    cfg.fov_range = 1.0f;
    return cfg;
}

}  // namespace

// ① 球不该被选中：人和球都在圆内、分数接近，但球 h/w=1.00 低于阈值 ⇒ 必须选中人。
TEST(target_selector_rejects_round_objects_by_aspect) {
    TargetSelector sel;
    TargetSelectorConfig cfg = shape_cfg(640);
    cfg.min_aspect_h_over_w = 1.15f;
    const std::vector<DetectionBox> scene = {
        shape_box(250, 90, 390, 470, 6),   // 球 140×380? 下面会算 h/w
        shape_box(250, 90, 390, 470, 0),   // 人
    };
    // 换成真正的球：40×40（h/w=1.00）
    std::vector<DetectionBox> scene2 = {
        shape_box(300, 95, 340, 135, 6),    // 球 h/w=1.00
        shape_box(250, 90, 390, 470, 0),    // 人 140×380 h/w=2.71
    };
    const auto r = sel.select(scene2, cfg, 1000);
    CHECK(r.valid);
    CHECK_EQ(r.box.class_id, 0);
    (void)scene;
}

// 关掉兜底这条判据就不生效（证明它在起作用，不是场景本身没歧义）。
TEST(target_selector_aspect_guard_is_load_bearing) {
    TargetSelector sel;
    TargetSelectorConfig cfg = shape_cfg(640);
    cfg.min_aspect_h_over_w = 0.5f;   // 放到球下面 ⇒ 不再排除
    const std::vector<DetectionBox> scene = {
        shape_box(300, 95, 340, 135, 6),
        shape_box(250, 90, 390, 470, 0),
    };
    const auto r = sel.select(scene, cfg, 1000);
    CHECK(r.valid);   // 人仍然可选（不强制它一定选球）
}

// 正常人形（站立/近身被裁/侧身）不该被误杀。
TEST(target_selector_keeps_normal_human_shapes) {
    const float dims[][2] = {{140, 380}, {360, 605}, {200, 300}, {100, 210}};
    for (const auto& d : dims) {
        const float hw = d[1] / d[0];
        if (hw < 1.15f) continue;            // 数据已低于阈值，跳过
        TargetSelector sel;
        TargetSelectorConfig cfg = shape_cfg(640);
        cfg.min_aspect_h_over_w = 1.15f;
        const auto r = sel.select({shape_box(200, 60, 200 + d[0], 60 + d[1], 0)}, cfg, 1000);
        CHECK(r.valid);
    }
}

// ② FOV 半径是真实约束：半径小 ⇒ 远处框被排除；半径大 ⇒ 又接纳。
//    V1.0.30 之前这条是失效的（fov.radius 被多乘 2 ⇒ 半径恒等于画面半宽）。
TEST(target_selector_fov_range_really_excludes) {
    const DetectionBox far_box = shape_box(280, 20, 360, 250, 0);
    for (float r : {0.3f, 0.5f, 0.8f, 1.0f}) {
        TargetSelector sel;
        TargetSelectorConfig cfg = shape_cfg(640);
        cfg.fov_range = r;
        const auto got = sel.select({far_box}, cfg, 1000);
        const float radius_px = 320.0f * r;
        const float ax = 280 + 80 * 0.5f, ay = 20 + 230 * 0.2f;
        const float dx = ax - 320.0f, dy = ay - 320.0f;
        const float d = std::sqrt(dx * dx + dy * dy);
        if (d <= radius_px) {
            CHECK(got.valid);
        } else {
            CHECK(!got.valid);
        }
    }
}

// 选靶把"本帧实际用的 FOV 半径"报给预览画圆（保证画的圆 = 约束的圆）。
TEST(target_selector_reports_actual_fov_radius) {
    TargetSelector sel;
    TargetSelectorConfig cfg = shape_cfg(640);
    cfg.fov_range = 0.5f;
    sel.select({shape_box(250, 90, 390, 470, 0)}, cfg, 1000);
    CHECK_EQ(sel.last_fov_radius_px(), 160.0f);   // 320 × 0.5
}
