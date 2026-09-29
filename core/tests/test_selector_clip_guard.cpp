// test_selector_clip_guard.cpp — V1.0.07：贴裁剪区边界剔除 + 锁定尺寸一致性。
//
// 两个机制都来自 2026-09-29 的实机定障，用例按记录器里的现场数值构造，不是拍脑袋：
//   ① 现场 t=3484.41（开火刚结束）：锁定的框 x[1577,1600] y[660,805]（宽 23px 高 145px），
//      右边界 1600 正好压在 capture 裁剪区右缘 ⇒ 瞄准点离准星 308px ⇒ 单帧鼠标位移 +27。
//      全段统计：瞄准中贴裁剪边界的帧位移 p50=15/p90=27，非贴边帧 p50=0/p90=3。
//   ② 类别跳变 94% 是 cls5↔cls0 两套"人体"框，框高差 3~14 倍 ⇒ 落点按 offset_y×Δh
//      跳 60~200px（跨类别 |Δtgt_y| p90=64，同类别 p90=10）。
//
// 板端口径（GET_CONFIG 实测）：整帧 2560×1440，capture 640×640 居中 ⇒ 裁剪区
// x[960,1600] y[400,1040]，search_radius_px = 320，offset = (0.5, 0.31)。
#include <cmath>
#include <vector>

#include "mouse/TargetSelector.hpp"
#include "test_util.hpp"

using namespace ttbox::core;
using namespace ttbox::core::aim;

namespace {

TargetSelectorConfig board_cfg() {
    TargetSelectorConfig c;
    c.roi_w = 2560;
    c.roi_h = 1440;
    c.center_x = 0.5f;
    c.center_y = 0.5f;   // 准星 = (1280, 720)
    c.aim_ratio_x = 0.5f;
    c.aim_ratio_y = 0.31f;
    c.search_radius_px = 320.0f;   // = 裁剪区半宽（capture 640×640）
    c.fov_range = 1.0f;
    c.confidence = 0.25f;
    c.lost_grace_ms = 30.0f;
    // 本文件只验这两个新机制本身 ⇒ 把切靶防抖关掉，避免干扰判据。
    c.switch_hysteresis = 0.0f;
    c.switch_cooldown_ms = 0.0f;
    return c;
}

DetectionBox mk(float x1, float y1, float x2, float y2, int cls = 5) {
    DetectionBox b;
    b.x1 = x1; b.y1 = y1; b.x2 = x2; b.y2 = y2;
    b.score = 0.9f;
    b.class_id = cls;
    return b;
}

// 现场复刻：贴着裁剪区右缘的瘦长条（画面外的人只露一小条）
DetectionBox sliver_right() { return mk(1577, 660, 1600, 805); }
// 现场复刻：同一帧里的真目标（准星附近）
DetectionBox real_target() { return mk(1220, 660, 1310, 804); }

}  // namespace

// ① 贴右边界：只有瘦条 ⇒ 无可瞄准目标（而不是把准星拉过去）。
TEST(selector_clip_only_sliver_yields_no_target) {
    TargetSelector sel;
    auto cfg = board_cfg();
    std::vector<DetectionBox> dets = {sliver_right()};
    const auto s = sel.select(dets, cfg, 1000);
    CHECK(!s.valid);
}

// ① 贴右边界：真目标在场时不能被瘦条抢走（瘦条的瞄准点距离更远，本就该让位）。
TEST(selector_clip_sliver_does_not_steal_lock) {
    TargetSelector sel;
    auto cfg = board_cfg();
    std::vector<DetectionBox> dets = {sliver_right(), real_target()};
    const auto s = sel.select(dets, cfg, 1000);
    CHECK(s.valid);
    CHECK_EQ(static_cast<int>(s.box.x1), 1220);
}

// ① 负控：关掉横向剔除 ⇒ 现场那个瘦条会被选中，准星被拉走 300px。
//    这一条保证"上面两条不是白测的"——机制真的是剔除在起作用。
TEST(selector_clip_disabled_sliver_steals_lock_negctrl) {
    TargetSelector sel;
    auto cfg = board_cfg();
    cfg.reject_clip_horizontal = false;   // 负控：回到加此参数前的行为
    std::vector<DetectionBox> dets = {sliver_right()};
    const auto s = sel.select(dets, cfg, 1000);
    CHECK(s.valid);
    CHECK_EQ(static_cast<int>(s.box.x1), 1577);
    // 瞄准点离准星 300px 量级 ⇒ 这正是"猛拉一把"的来源
    const float aim_x = s.box.x1 + (s.box.x2 - s.box.x1) * cfg.aim_ratio_x;
    CHECK(std::fabs(aim_x - 1280.0f) > 250.0f);
}

// ① 贴左边界同样剔除（对称性）。
TEST(selector_clip_left_edge_rejected) {
    TargetSelector sel;
    auto cfg = board_cfg();
    std::vector<DetectionBox> dets = {mk(960, 660, 1050, 800)};
    CHECK(!sel.select(dets, cfg, 1000).valid);
}

// ① 下边界必须**不**判：近身目标的腿被裁剪区下边截掉是常态（实测 9 帧这种大框
//    位移只有 -1~-6 count）。判了会把近距离目标整片丢掉。
TEST(selector_clip_bottom_edge_kept) {
    TargetSelector sel;
    auto cfg = board_cfg();
    // 现场 t=3454.96：x[1114,1324] y[588,1041]，框高 453 —— 腿出裁剪区下边界
    std::vector<DetectionBox> dets = {mk(1114, 588, 1324, 1041, 0)};
    const auto s = sel.select(dets, cfg, 1000);
    CHECK(s.valid);
    CHECK_EQ(static_cast<int>(s.box.x1), 1114);
}

// ① 上边界默认不判（近身仰角目标头顶会被裁，判了会误伤）⇒ 打开后才剔除。
//    注意：框要放在准星附近，否则它本来就出 FOV 圆、测不出"是上边界判据在起作用"。
TEST(selector_clip_top_edge_opt_in) {
    TargetSelector off;
    auto cfg_off = board_cfg();   // reject_clip_top 默认 false
    std::vector<DetectionBox> far_top = {mk(1200, 402, 1300, 600)};
    CHECK(off.select(far_top, cfg_off, 1000).valid);

    TargetSelector on;
    auto cfg_on = board_cfg();
    cfg_on.reject_clip_top = true;
    std::vector<DetectionBox> far_top2 = {mk(1200, 402, 1300, 600)};
    CHECK(!on.select(far_top2, cfg_on, 1000).valid);
}

// ① 离准星近的框豁免贴边判定（clip_center_max_px），避免误伤近身贴边目标。
TEST(selector_clip_near_center_exempt) {
    TargetSelector sel;
    auto cfg = board_cfg();
    // 近身大框：x1=960 正好压在裁剪区左缘，但框心 (1240,720) 离准星只有 40px ⇒ 豁免
    std::vector<DetectionBox> dets = {mk(960, 560, 1520, 880, 0)};
    const auto s = sel.select(dets, cfg, 1000);
    CHECK(s.valid);
    CHECK_EQ(static_cast<int>(s.box.x1), 960);
}

// ② 尺寸一致性：同一位置换成"小得多"的框（cls0 那套）时不许整块换，
//    沿用上一帧的锁定框（落点因此不跳）。
TEST(selector_track_lock_size_consistency_holds_old_box) {
    TargetSelector sel;
    auto cfg = board_cfg();
    const DetectionBox big = real_target();                  // h = 144
    CHECK(sel.select({big}, cfg, 1000).valid);

    const DetectionBox small = mk(1220, 780, 1310, 825, 0);   // h = 45，比 3.2 倍
    const auto s = sel.select({small}, cfg, 1010);
    CHECK(s.valid);
    CHECK_EQ(static_cast<int>(s.box.y2), 804);               // 仍是旧框
    CHECK_EQ(static_cast<int>(s.box.y1), 660);
}

// ② 负控：track_size_ratio = 0（关闭）⇒ 当场换块，落点跳 —— 复现旧行为。
TEST(selector_track_lock_size_ratio_disabled_swaps_negctrl) {
    TargetSelector sel;
    auto cfg = board_cfg();
    cfg.track_size_ratio = 0.0f;   // 负控
    const DetectionBox big = real_target();
    CHECK(sel.select({big}, cfg, 1000).valid);

    const DetectionBox small = mk(1220, 780, 1310, 825, 0);
    const auto s = sel.select({small}, cfg, 1010);
    CHECK(s.valid);
    CHECK_EQ(static_cast<int>(s.box.y2), 825);   // 整块换掉了
}

// ② 正常跟随不受影响：框高小幅变化（1.04 倍）必须照常跟随。
TEST(selector_track_lock_small_size_change_follows) {
    TargetSelector sel;
    auto cfg = board_cfg();
    CHECK(sel.select({real_target()}, cfg, 1000).valid);        // h = 144
    const DetectionBox grown = mk(1218, 655, 1312, 805, 5);     // h = 150
    const auto s = sel.select({grown}, cfg, 1010);
    CHECK(s.valid);
    CHECK_EQ(static_cast<int>(s.box.y1), 655);
    CHECK_EQ(static_cast<int>(s.box.y2), 805);
}

// ② 场景串起来：cls5 与 cls0 两套人体框交替出现 —— 锁定必须稳住，落点不得来回跳。
//    （实测这两套框同位置出现时，框架落点本来只差 1~5px；跳 60px 以上的是"换成了
//      另一个目标"，判据拦的就是那种。这里按"宽限窗内保持锁定"验行为。）
TEST(selector_track_lock_class_flip_keeps_aim_point_stable) {
    TargetSelector sel;
    auto cfg = board_cfg();
    const DetectionBox body5 = mk(1231, 667, 1322, 797, 5);   // h = 130
    CHECK(sel.select({body5}, cfg, 1000).valid);

    float prev_aim = body5.y1 + (body5.y2 - body5.y1) * cfg.aim_ratio_y;
    for (int i = 1; i <= 5; ++i) {
        // cls0 那套框：另一个目标的框，高度只有 45（比值 2.9 > 2.0）⇒ 不许换
        const DetectionBox other = mk(1231, 745, 1322, 790, 0);
        const auto s = sel.select({other}, cfg, 1000 + i * 5);   // 宽限 30ms 内
        CHECK(s.valid);
        const float aim = s.box.y1 + (s.box.y2 - s.box.y1) * cfg.aim_ratio_y;
        CHECK(std::fabs(aim - prev_aim) < 5.0f);   // 落点不许跳
        prev_aim = aim;
    }
}

int main() {
    return ttbox_test::run_all();
}
