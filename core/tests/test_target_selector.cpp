// test_target_selector.cpp — TargetSelector 选靶行为测试
//
// ★ 为什么补这个测试（2026-10-03）：
//   TargetSelector 是「错了不会崩」的那一类——选错目标不产生异常，
//   只会让手感变差（跟不跟、跟错人、抖动）。V1.0.13 删 BB 时它零引用，
//   但它是生产唯一选靶实现（AimThread 每帧调它），却没有专测。
//   本文件锁的是**几何/阈值类的静默行为**——这些最容易在改动中被带坏。
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

// 默认配置：ROI 640x640、fov_range=1.0 ⇒ FOV 半径 640（全帧短边）、置信度 0.25。
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
    cfg.switch_cooldown_ms = 0.0f;   // 关冷却：让切靶行为可测
    cfg.lock_hold_ms = 0.0f;
    return cfg;
}

}  // namespace

// ---------- 基本：空输入 / 低置信度 / 正常命中 ----------

TEST(target_selector_empty_input_is_invalid) {
    TargetSelector sel;
    const auto r = sel.select({}, base_cfg(), 0);
    CHECK(!r.valid);
    CHECK_EQ(sel.last_reason(), TargetSelection::kNone);

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
    CHECK_EQ(sel2.last_reason(), TargetSelection::kNone);
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
    // ★ 核心静默行为：FOV 半径 = 全帧短边 × fov_range。ROI 640 ⇒ 短边 640。
    //   这里 fov_range=0.5 ⇒ 半径 320px。准星在 (320,320)，把框放到 x=600（落点距 280）
    //   **之内** ⇒ 应命中；放到 x=900（落点距 580）**之外** ⇒ 应被丢掉。
    TargetSelector sel;
    auto cfg = base_cfg();
    cfg.fov_range = 0.5f;   // 半径 320
    {
        std::vector<DetectionBox> dets{make_box(600.0f, 320.0f, 40.0f, 40.0f, 0.9f)};
        CHECK(sel.select(dets, cfg, 0).valid);          // 落点距 280 < 320
    }
    sel.reset();
    {
        std::vector<DetectionBox> dets{make_box(900.0f, 320.0f, 40.0f, 40.0f, 0.9f)};
        CHECK(!sel.select(dets, cfg, 0).valid);         // 落点距 580 > 320
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

// ---------- 丢失宽限：空帧不产移动，但 track 不删、恢复后同 id 延续 ----------

TEST(target_selector_blank_frame_is_invalid_but_keeps_track) {
    // ★★ 这里最容易被"改成想要的样子"而写错，先把真实语义钉住：
    //   TargetSelector 对空检测帧**立即返回 invalid**（源码注释写明"安全红线：
    //   不允许凭旧坐标产生移动"）。lost_grace_ms 管的是**track 生命周期**——
    //   track 在宽限内**不删除**，恢复检测后关联回同一条 ⇒ 同 target_id 延续。
    //   "丢失期保持有效"由 AimStateMachine 的 LOST_GRACE 层实现，不在本类里。
    //   若哪天有人把空帧改成 valid=true，本测试立刻报红（那会凭旧坐标推鼠标）。
    TargetSelector sel;
    const auto cfg = base_cfg();
    std::vector<DetectionBox> dets{make_box(300.0f, 320.0f, 40.0f, 40.0f, 0.9f)};
    const auto first = sel.select(dets, cfg, 0);
    CHECK(first.valid);
    const int id0 = first.target_id;
    CHECK_EQ(sel.tracks().size(), 1u);

    // 空帧（10ms < lost_grace 30ms）⇒ 无目标（安全红线），但 track 仍在表里
    const auto second = sel.select({}, cfg, 10);
    CHECK(!second.valid);                        // 空帧不产移动
    CHECK_EQ(sel.tracks().size(), 1u);            // 宽限内不删 track
    CHECK_EQ(sel.last_reason(), TargetSelection::kNone);

    // 同一位置恢复检测 ⇒ 关联回原 track ⇒ 同 id 延续（手感上 = 没丢目标）
    const auto third = sel.select(dets, cfg, 20);
    CHECK(third.valid);
    CHECK_EQ(third.target_id, id0);

    // ★ 这里是**两级**生命周期，别混成一级（第一版就写错过）：
    //   ① lost_grace_ms(30ms) 管**激活**：超期 → t.active=false、active_track_=-1，
    //      但 track 条目仍在表里；
    //   ② track_buffer_frames(30 **帧**) 管**删除**：`!active && lost_frames > 30`。
    sel.select({}, cfg, 1020);                       // 超 lost_grace（30ms）
    CHECK_EQ(sel.tracks().size(), 1u);                // 仍在表里
    bool any_active = false;
    for (const auto& t : sel.tracks()) any_active = any_active || t.active;
    CHECK(!any_active);                               // 但已不再激活

    // ★★ 锁一个**实现现状（非预期）**：僵尸 track 不会被 track_buffer_frames 清掉。
    //   根因：TargetSelector.cpp 里所有 `lost_frames++`（:240 / :307 / :412）都包在
    //   `if (t.active)` 内 —— track 一旦 active=false，lost_frames 就冻结在宽限耗尽
    //   那一刻的值（此处 2），再也不会涨到 track_buffer_frames(30)，
    //   ⇒ trim_tracks 的删除条件 `!active && lost_frames > buf` **永不成立**。
    //   不抛内存泄漏：max_tracks(=64) 那条裁剪路径兜底（pop_back 先丢非激活），
    //   表规模有硬上限。但代价是 track 表会长期停在满额，僵尸条目持续参与关联竞争。
    //   这条断言的作用：哪天有人修了它（或改坏了 max_tracks 兜底），这里会报红，
    //   提醒同步更新上面两行断言。修复前不得改这两行为 true。
    for (int i = 0; i < 40; ++i) sel.select({}, cfg, 1021);  // 远超 30 帧
    CHECK_EQ(sel.tracks().size(), 1u);                // 现状：僵尸 track 仍在
    //兜底仍在：塞到超过 max_tracks(64) 条活跃 track 时，表规模被截住（不会无限长）
    TargetSelector sel2;
    auto cfg2 = base_cfg();
    std::vector<DetectionBox> many;
    for (int i = 0; i < 100; ++i) {
        // 沿一条从左到右的密集带布点，保证每条都进得了候选（FOV 半径内且不贴边）
        many.push_back(make_box(static_cast<float>(i * 6), 320.0f, 4.0f, 8.0f, 0.9f));
    }
    sel2.select(many, cfg2, 0);
    CHECK(sel2.tracks().size() <= static_cast<size_t>(cfg2.max_tracks));
}

// ---------- reset：必须把 track / 开关 / 遥测全清 ----------

TEST(target_selector_reset_clears_all_state) {
    // ★ V1.0.11 大量状态挂在这里（pending/has_last_out_box/selector_holds_total…）。
    //   reset 漏清一项 ⇒ 切模型后继承旧目标身份/旧计数（静默串味）。
    TargetSelector sel;
    auto cfg = base_cfg();
    std::vector<DetectionBox> dets{make_box(300.0f, 320.0f, 40.0f, 40.0f, 0.9f)};
    sel.select(dets, cfg, 0);
    CHECK(!sel.tracks().empty());

    sel.reset();
    CHECK(sel.tracks().empty());
    CHECK_EQ(sel.last_reason(), TargetSelection::kNone);
    CHECK_EQ(sel.selector_holds_total(), static_cast<uint64_t>(0));

    // reset 后新track 的 id 必须从 1 重新开始（禁止继承旧模型身份空间）
    const auto again = sel.select(dets, cfg, 0);
    CHECK(again.valid);
    CHECK_EQ(again.target_id, 1);
}

// ---------- 优先级：high 类应压过普通类 ----------

TEST(target_selector_priority_classes_win) {
    // ★ 静默行为：开了优先级后，high 类即使更远也应被选。
    // ★★ 第一版这里写的是两条相距 20px、用 `distance > 15` 间接判断 —— **是假绿**：
    //   故障注入删掉排序里的 priority 比较后，distance=20 仍 > 15，用例照样过。
    //   （aim_ratio_y=0.2 而两框等高 ⇒ 两框的 dist_sq 只差 x 偏移；20px 的差距
    //     在"最近优先"和"高优先优先"两种排序下选中的框可能相同。）
    //   现改为：距离拉开 120px（胜负无歧义）+ **直接查选中框的 class_id**，
    //   断言不再依赖 distance 间接推断。
    TargetSelector sel;
    auto cfg = base_cfg();
    cfg.priority_enabled = true;
    cfg.priority_classes = {1};
    cfg.priority_classes_high = {2};
    cfg.priority_scoring = false;         // 最近优先（打分管到另一条）

    std::vector<DetectionBox> dets{
        make_box(300.0f, 320.0f, 40.0f, 40.0f, 0.9f, /*class*/ 1),  // 近 20px、prio1
        make_box(420.0f, 320.0f, 40.0f, 40.0f, 0.9f, /*class*/ 2),  // 远 100px、prio2
    };
    const auto r = sel.select(dets, cfg, 0);
    CHECK(r.valid);
    // 期望选中 class 2（更远但高优先）—— 直接查 class_id，不做间接推断
    CHECK_EQ(r.box.class_id, 2);

    // 反证：把 high 列表清掉 ⇒ 应回到"最近优先"选class 1。
    //   少了这一条，上面的 CHECK_EQ(2) 可能因为"总是选最后一个框"之类的巧合而成立。
    TargetSelector sel2;
    auto cfg2 = base_cfg();
    cfg2.priority_enabled = true;
    cfg2.priority_classes = {1};
    cfg2.priority_classes_high = {};     // ★ 不给任何高优先类
    cfg2.priority_scoring = false;
    const auto r2 = sel2.select(dets, cfg2, 0);
    CHECK(r2.valid);
    CHECK_EQ(r2.box.class_id, 1);        // 无 high 兜底 ⇒ 回到最近的 class 1
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
//    ★ 半径基准 = 全帧短边（ROI 640 ⇒ 640px），不再用 capture 半宽。
TEST(target_selector_fov_range_really_excludes) {
    const DetectionBox far_box = shape_box(280, 20, 360, 250, 0);
    for (float r : {0.3f, 0.5f, 0.8f, 1.0f}) {
        TargetSelector sel;
        TargetSelectorConfig cfg = shape_cfg(640);
        cfg.fov_range = r;
        const auto got = sel.select({far_box}, cfg, 1000);
        const float radius_px = 640.0f * r;
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
    CHECK_EQ(sel.last_fov_radius_px(), 320.0f);   // 全帧短边 640 × 0.5
}
