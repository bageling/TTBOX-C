// test_aim_point_clip.cpp — V1.0.08/09：框底被裁剪区下边界截断时的落点外推。
//
// 定障来源（2026-09-29 板端常驻记录器 + 离线复算，不是拍脑袋）：
//   业主症状「瞄胸口、走进目标以后准星就跑到头上」。
//   机理：检测只吃画面中心一块 crop，近身目标的下半身落在 crop 之外 ⇒
//     可见框高 h_obs = crop 下边 - y1 比真实身高小 ⇒ ty = y1 + 0.31*h_obs 相对人体上飘。
//     越近截得越多，所以是「走进去才飘」而不是「一直偏」。
//
// ★ 板端口径（2026-09-29 实机钉死，两处独立证据）：
//   · 帧 = 2560x1440（V4L2 G_FMT）；capture = 416x416 居中 ⇒ 裁剪区
//     x[1072,1488]、y[512,928]，下边界 = 1440/2 + 0 + 208 = **928**。
//     证据一：cls5 原始检测框 y2 在 928 出现巨峰（928 命中 102、927 命中 52、926 命中 22）。
//     证据二：「走近目标」段（t≈7443.7~7444.6s）选中框 y2 从 832 一路涨到 928 就钉死
//             不动，而 y1 从 631 继续上冒到 526 ⇒ 框底被截。
//   · 历史档：capture = 640x640（同一批记录里早期时段）⇒ 下边界 = 1040。
//     两个档位都要覆盖：crop_bottom 是运行时按 capture 算的，不是常量。
//
// 量化（tgt_y 与 _det.csv 原始框按时间对齐、按 oy≈0.31 反查当帧选中框，6069 帧）：
//   · 选中框贴到裁剪区下边（y2 >= 924）的帧 = 351（5.8%）。
//   · 这些帧落点相对真实胸口上飘 p50≈65px、p90≈173px、max≈240px ——
//     在 300~400px 高的身体上就是 20%，0.31 的胸口位直接变成 0.10 的头部。
//
// 修法：贴边时用**框宽** × 身高宽比反推身高。
//   比例优先取「同一目标最近一次未截断帧的 h/w」（AimThread 自校准）——
//   本模型 cls5 的框宽高比实测在 0.29~0.52 之间漂（远距离常只框上半身），写死会过度
//   修正；同一目标的比例按距离等比缩放，可直接外推。没有自校准值时退回 body_w_over_h。
#include <cmath>

#include "mouse/AimPointProfile.hpp"
#include "mouse/ClipHeightRatio.hpp"
#include "mouse/FrozenRect.hpp"    // V1.0.10：冻结落点
#include "test_util.hpp"

using namespace ttbox::core;
using namespace ttbox::core::aim;

namespace {

// 两档真实裁剪区下边界（全帧像素，与检测框同一坐标系）
constexpr float kCrop416Bottom = 928.0f;    // 板端当前档（capture 416x416）
constexpr float kCrop640Bottom = 1040.0f;   // 历史档（capture 640x640）

AimPointProfile board_prof() {
    AimPointProfile p;
    p.offset_x = 0.5f;
    p.offset_y = 0.31f;
    p.class_offsets.clear();
    return p;
}

DetectionBox mk(float x1, float y1, float x2, float y2, int cls = 5) {
    DetectionBox b;
    b.x1 = x1; b.y1 = y1; b.x2 = x2; b.y2 = y2;
    b.score = 0.9f;
    b.class_id = cls;
    return b;
}

bool near(float a, float b, float eps = 0.5f) { return std::fabs(a - b) <= eps; }

// 现场复刻（640 档）：框底压在裁剪区下边 1040 的近身大框
DetectionBox clipped_box_640() { return mk(1150, 584, 1359, 1040); }

// 现场复刻（416 档，当前板端）：「走近目标」段末尾那种框底钉在 928 的框
//   w = 180、可见 h = 378（y1=550 → y2=928）
DetectionBox clipped_box_416() { return mk(1080, 550, 1260, 928); }

}  // namespace

// ① 不传 crop_bottom（旧调用口径）⇒ 行为与加此机制前逐字节一致。
TEST(aim_clip_legacy_signature_unchanged) {
    auto prof = board_prof();
    float tx = 0, ty = 0;
    CHECK(aim_point_at(clipped_box_640(), 5, prof, &tx, &ty));
    CHECK(near(tx, 1150.0f + 0.5f * 209.0f));          // 1254.5
    CHECK(near(ty, 584.0f + 0.31f * 456.0f));          // 725.36
}

// ② crop_bottom 未知（<=0）⇒ 不做外推，与旧口径一致。
TEST(aim_clip_unknown_bottom_no_extrapolate) {
    auto prof = board_prof();
    float tx = 0, ty = 0;
    CHECK(aim_point_at(clipped_box_640(), 5, prof, &tx, &ty, -1.0f));
    CHECK(near(ty, 725.36f));
    CHECK(aim_point_at(clipped_box_640(), 5, prof, &tx, &ty, 0.0f));
    CHECK(near(ty, 725.36f));
}

// ③ 框底没贴到裁剪区下边 ⇒ 框高可信 ⇒ 不做外推（远/中距离目标的常态）。
TEST(aim_clip_box_below_bottom_no_extrapolate) {
    auto prof = board_prof();
    const DetectionBox b = mk(1150, 300, 1359, 756);   // y2=756，离 1040 还差 284
    float tx = 0, ty = 0;
    CHECK(aim_point_at(b, 5, prof, &tx, &ty, kCrop640Bottom));
    CHECK(near(ty, 300.0f + 0.31f * 456.0f));          // 441.36，未被改动
}

// ④ 640 档贴边 + 无自校准值 ⇒ 退回兜底 body_w_over_h：
//    h_eff = w / 0.32 = 653.125 ⇒ 落点下移。
TEST(aim_clip_bottom_hits_extrapolate_by_width_fallback) {
    auto prof = board_prof();
    float tx = 0, ty = 0;
    CHECK(aim_point_at(clipped_box_640(), 5, prof, &tx, &ty, kCrop640Bottom));
    CHECK(near(ty, 584.0f + 0.31f * (209.0f / 0.32f)));   // 786.47
    CHECK(ty > 725.36f + 50.0f);                          // 明显下移
}

// ⑤ 现场量化：兜底档的修正量应与「被截帧上飘量」同量级（此处 61.1px；实测 p50 约 65）。
TEST(aim_clip_extrapolate_amount_matches_board_measurement) {
    auto prof = board_prof();
    float tx0 = 0, ty0 = 0, tx1 = 0, ty1 = 0;
    CHECK(aim_point_at(clipped_box_640(), 5, prof, &tx0, &ty0));                        // 旧口径
    CHECK(aim_point_at(clipped_box_640(), 5, prof, &tx1, &ty1, kCrop640Bottom));       // 修正后
    const float dy = ty1 - ty0;
    CHECK(dy > 55.0f && dy < 70.0f);   // 实测 p50≈65 ⇒ 61.1 落在窗口内
    CHECK(near(tx0, tx1));             // X 不参与外推
}

// ⑥ 只放大、不缩小：框比「按宽度反推」更高时保持原高（防把落点往上拽）。
TEST(aim_clip_extrapolate_never_shrinks) {
    auto prof = board_prof();
    const DetectionBox b = mk(1200, 640, 1250, 1040);   // w=50 ⇒ h_from_w=156 < h=400
    float tx = 0, ty = 0;
    CHECK(aim_point_at(b, 5, prof, &tx, &ty, kCrop640Bottom));
    CHECK(near(ty, 640.0f + 0.31f * 400.0f));          // 764，与旧口径一致
}

// ⑦ 上限：夸张宽框（两人重叠等）最多按 3 倍可见框高外推，防止把落点推到脚下。
TEST(aim_clip_extrapolate_capped_at_3x) {
    auto prof = board_prof();
    const DetectionBox b = mk(300, 940, 2300, 1040);   // w=2000 ⇒ h_from_w=6250，h=100
    float tx = 0, ty = 0;
    CHECK(aim_point_at(b, 5, prof, &tx, &ty, kCrop640Bottom));
    CHECK(near(ty, 940.0f + 0.31f * 300.0f));          // h_eff 被 3h=300 卡住 ⇒ 1033
}

// ⑧ 开关关掉 ⇒ 完全回到 V1.0.07 行为（留 A/B 通路）。
TEST(aim_clip_switch_off_restores_legacy) {
    auto prof = board_prof();
    prof.clip_bottom_extrapolate = false;
    float tx = 0, ty = 0;
    CHECK(aim_point_at(clipped_box_640(), 5, prof, &tx, &ty, kCrop640Bottom));
    CHECK(near(ty, 725.36f));
}

// ⑨ margin 生效：框底离下边超过 clip_bottom_margin_px 就不算贴边。
TEST(aim_clip_margin_respected) {
    auto prof = board_prof();
    prof.clip_bottom_margin_px = 12.0f;                 // 判据：y2 >= 1040-12 = 1028
    float tx = 0, ty = 0;
    const DetectionBox just_out = mk(1150, 564, 1359, 1020);   // y2=1020 < 1028
    CHECK(aim_point_at(just_out, 5, prof, &tx, &ty, kCrop640Bottom));
    CHECK(near(ty, 564.0f + 0.31f * 456.0f));           // 未外推
    const DetectionBox just_in = mk(1150, 572, 1359, 1028);    // y2=1028 == 阈值
    CHECK(aim_point_at(just_in, 5, prof, &tx, &ty, kCrop640Bottom));
    CHECK(ty > 572.0f + 0.31f * 456.0f + 50.0f);        // 外推生效
}

// ⑩ 类别偏移照常生效（外推只改 h，不动 ox/oy 的选择）。
TEST(aim_clip_class_offset_still_applies) {
    auto prof = board_prof();
    ClassOffset co;
    co.class_id = 5;
    co.offset_x = 0.5f;
    co.offset_y = 0.45f;                                // 本类改成瞄框内 45%
    co.priority = 1;
    prof.class_offsets.push_back(co);
    float tx = 0, ty = 0;
    CHECK(aim_point_at(clipped_box_640(), 5, prof, &tx, &ty, kCrop640Bottom));
    CHECK(near(ty, 584.0f + 0.45f * (209.0f / 0.32f))); // 878.0
    // 类别 6 没配 ⇒ 用全局 0.31
    CHECK(aim_point_at(clipped_box_640(), 6, prof, &tx, &ty, kCrop640Bottom));
    CHECK(near(ty, 584.0f + 0.31f * (209.0f / 0.32f)));
}

// ⑪ ★ 当前板端档（416 裁剪、下边界 928）+ 自校准比 h/w=3.0：
//    可见 h 只有 378；不修 ⇒ ty=667.18；兜底(1/0.32=3.125) ⇒ 724.375；
//    自校准 3.0 ⇒ h_eff=540 ⇒ ty=717.4（相对不修下移 50.2px）。
TEST(aim_clip_live_416_crop_uses_calibrated_ratio) {
    auto prof = board_prof();
    float tx = 0, ty_raw = 0, ty_fb = 0, ty_cal = 0;
    CHECK(aim_point_at(clipped_box_416(), 5, prof, &tx, &ty_raw, -1.0f));                   // 不修
    CHECK(aim_point_at(clipped_box_416(), 5, prof, &tx, &ty_fb, kCrop416Bottom));           // 兜底
    CHECK(aim_point_at(clipped_box_416(), 5, prof, &tx, &ty_cal, kCrop416Bottom, 3.0f));    // 自校准
    CHECK(near(ty_raw, 550.0f + 0.31f * 378.0f));                    // 667.18
    CHECK(near(ty_fb, 550.0f + 0.31f * (180.0f / 0.32f)));           // 724.375
    CHECK(near(ty_cal, 550.0f + 0.31f * 540.0f));                    // 717.4
    CHECK(ty_cal > ty_raw + 45.0f);                                  // 相对不修确实下移
}

// ⑫ 自校准比优先于配置兜底：3.125（=1/0.32）与 3.0 会给出不同结果，必须是 3.0 生效。
TEST(aim_clip_calibrated_ratio_overrides_fallback) {
    auto prof = board_prof();
    float tx = 0, ty = 0;
    CHECK(aim_point_at(clipped_box_416(), 5, prof, &tx, &ty, kCrop416Bottom, 2.5f));
    CHECK(near(ty, 550.0f + 0.31f * (180.0f * 2.5f)));      // 689.5（不是兜底的 717.4）
}

// ⑬ 自校准比越界（退化框）⇒ 退回配置兜底，不拿坏比值去算。
TEST(aim_clip_ratio_out_of_range_uses_fallback) {
    auto prof = board_prof();
    float tx = 0, ty = 0;
    CHECK(aim_point_at(clipped_box_416(), 5, prof, &tx, &ty, kCrop416Bottom, 0.5f));   // < 0.8
    CHECK(near(ty, 550.0f + 0.31f * (180.0f / 0.32f)));     // 兜底 3.125 ⇒ 724.375
    CHECK(aim_point_at(clipped_box_416(), 5, prof, &tx, &ty, kCrop416Bottom, 99.0f));  // > 12
    CHECK(near(ty, 550.0f + 0.31f * (180.0f / 0.32f)));
}

// ⑭ 自校准比 ≈ 真实比例时是「空操作」（不许把本来就没截的框改歪）。
TEST(aim_clip_ratio_near_true_ratio_is_noop) {
    auto prof = board_prof();
    const float ratio = 378.0f / 180.0f;                    // 2.1，正好等于可见框的 h/w
    float tx = 0, ty = 0;
    CHECK(aim_point_at(clipped_box_416(), 5, prof, &tx, &ty, kCrop416Bottom, ratio));
    CHECK(near(ty, 550.0f + 0.31f * 378.0f));               // 667.18，未被改动
}

// ⑮ 416 档：框底没贴到 928（离下边 > 12px）⇒ 不做外推，即便给了自校准比。
TEST(aim_clip_live_416_no_extrapolate_when_not_touching) {
    auto prof = board_prof();
    const DetectionBox b = mk(1080, 420, 1260, 900);        // y2=900 < 928-12=916
    float tx = 0, ty = 0;
    CHECK(aim_point_at(b, 5, prof, &tx, &ty, kCrop416Bottom, 3.0f));
    CHECK(near(ty, 420.0f + 0.31f * 480.0f));               // 568.8
}

// ⑯ 自校准比也受 3 倍上限约束。
TEST(aim_clip_calibrated_ratio_still_capped) {
    auto prof = board_prof();
    const DetectionBox b = mk(300, 900, 2300, 928);         // w=2000、h=28
    float tx = 0, ty = 0;
    CHECK(aim_point_at(b, 5, prof, &tx, &ty, kCrop416Bottom, 12.0f));
    CHECK(near(ty, 900.0f + 0.31f * 84.0f));                // h_eff 被 3h=84 卡住 ⇒ 926.04
}

// ============================================================================
// V1.0.09：身高反推比的按目标自校准（ClipHeightRatioTracker）
// 为什么不能写死一个「人体宽高比」——本模型 cls5 实测在 0.29~0.52 之间漂，见文件头。
// ============================================================================

// ⑰ 未截断帧会被学成基准比（h/w）。
TEST(clip_ratio_tracker_learns_from_unclipped_frame) {
    ClipHeightRatioTracker tr;
    CHECK(near(tr.ratio_for(1), 0.0f));                 // 还没学过
    tr.observe(180.0f, 540.0f, false, 1);               // h/w = 3.0
    CHECK(near(tr.ratio_for(1), 3.0f));
    CHECK_EQ(tr.per_target_id(), 1);
}

// ⑱ 被截断帧**不能**当基准（它的 h 本身偏小，拿它当基准等于承认错误）。
TEST(clip_ratio_tracker_ignores_clipped_frame_as_baseline) {
    ClipHeightRatioTracker tr;
    tr.observe(180.0f, 540.0f, false, 1);               // 基准 3.0
    tr.observe(240.0f, 300.0f, true, 1);                // 截断帧 h/w=1.25，不许覆盖
    CHECK(near(tr.ratio_for(1), 3.0f));
}

// ⑲ 本目标缓存优先于跨目标 EMA；换个目标才退到 EMA。
//    逐步算术（α_target=0.2 / α_ema=0.05）：
//      目标1: h/w=3.0 ⇒ per_target=3.0、ema=3.0
//      目标2: h/w=2.0 ⇒ per_target=3.0×0.8+2.0×0.2=2.8、ema=3.0×0.95+2.0×0.05=2.95
//      目标3: h/w=5.0 ⇒ per_target=2.8×0.8+5.0×0.2=3.24、ema=2.95×0.95+5.0×0.05=3.0525
TEST(clip_ratio_tracker_per_target_wins_over_ema) {
    ClipHeightRatioTracker tr;
    tr.observe(180.0f, 540.0f, false, 1);
    CHECK(near(tr.ratio_for(1), 3.0f, 0.01f));
    tr.observe(200.0f, 400.0f, false, 2);
    CHECK(near(tr.ratio_for(2), 2.8f, 0.01f));          // 目标2 用自己学到的那份
    CHECK(near(tr.ratio_for(1), 2.95f, 0.01f));         // 目标1 已不是当前目标 ⇒ 退 EMA
    CHECK(near(tr.ratio_for(3), 2.95f, 0.01f));         // 没见过目标3 ⇒ 也是 EMA
    tr.observe(100.0f, 500.0f, false, 3);
    CHECK(near(tr.ratio_for(3), 3.24f, 0.01f));         // 目标3 有了自己的
    CHECK(near(tr.ema(), 3.0525f, 0.01f));
}

// ⑳ 退化框（近方形 / 超宽并框 / 零尺寸）一律不学，免得把身高外推炸上天。
TEST(clip_ratio_tracker_rejects_degenerate_ratio) {
    ClipHeightRatioTracker tr;
    tr.observe(100.0f, 60.0f, false, 1);                // h/w = 0.6 < kMinRatio
    CHECK(near(tr.ratio_for(1), 0.0f));
    tr.observe(100.0f, 20.0f, false, 1);                // h/w = 0.2
    CHECK(near(tr.ratio_for(1), 0.0f));
    tr.observe(2000.0f, 30000.0f, false, 1);            // h/w = 15 > kMaxRatio
    CHECK(near(tr.ratio_for(1), 0.0f));
    tr.observe(0.0f, 0.0f, false, 1);                   // 零尺寸
    CHECK(near(tr.ratio_for(1), 0.0f));
    tr.observe(50.0f, 0.0f, false, 1);
    CHECK(near(tr.ratio_for(1), 0.0f));
}

// ㉑ EMA 会随多次观测平滑收敛（不做单帧突变）。
TEST(clip_ratio_tracker_ema_smooths) {
    ClipHeightRatioTracker tr;
    tr.observe(100.0f, 300.0f, false, 7);               // 3.0
    tr.observe(100.0f, 400.0f, false, 7);               // 4.0 ⇒ 本目标 EMA: 3.2
    CHECK(near(tr.per_target(), 3.2f, 0.02f));
    CHECK(near(tr.ema(), 3.05f, 0.02f));                // 跨目标 α=0.05 ⇒ 3.05
}

// ㉒ 端到端串起来：先远（未截断、学到比例）→ 再近（截断）⇒ 落点被拉回真实胸口。
TEST(clip_ratio_tracker_end_to_end_pull_back_to_chest) {
    auto prof = board_prof();
    ClipHeightRatioTracker tr;
    // 远：w=60、h=180（h/w=3.0），没贴底边 ⇒ 学基准
    tr.observe(60.0f, 180.0f, false, 42);
    // 近：同一个目标、框长到 w=180，但可见高被 928 卡在 378
    const DetectionBox near_box = clipped_box_416();
    const bool clipped = near_box.y2 >= kCrop416Bottom - prof.clip_bottom_margin_px;
    CHECK(clipped);
    const float ratio = tr.ratio_for(42);
    CHECK(near(ratio, 3.0f));
    float tx = 0, ty = 0;
    CHECK(aim_point_at(near_box, 5, prof, &tx, &ty, kCrop416Bottom, ratio));
    // h_eff = 180×3.0 = 540 ⇒ ty = 550 + 167.4 = 717.4（不修的话是 667.18，落在头上）
    CHECK(near(ty, 717.4f));
    CHECK(ty > 667.18f + 50.0f);
}

// ==================== V1.0.10：冻结落点（FrozenRectTracker） ====================
// 思路来源：yu 的 holding_previous —— 腿被裁掉时**不修坏量测，而是拒绝它**，
// 落点保持上一次能看全的那一帧。走近时框顶上升与身高变大互相抵消，
// 「你正在瞄的那个点，屏幕上本来就不该动」。

// ㉓ 未截断帧：冻结点更新成这一帧。
TEST(frozen_rect_updates_on_unclipped_frame) {
    FrozenRectTracker fr;
    fr.observe(clipped_box_416(), false, 3);
    CHECK(fr.valid());
    CHECK(fr.id() == 3);
    DetectionBox out{};
    CHECK(fr.frozen_for(3, &out));
    CHECK(near(out.y2, 928.0f));
}

// ㉔ 截断帧：冻结器不更新，frozen_for 仍给上一帧的完整框。
TEST(frozen_rect_keeps_previous_on_clipped_frame) {
    FrozenRectTracker fr;
    const DetectionBox full = mk(1080, 400, 1260, 900);   // 能看全：h=500
    fr.observe(full, false, 7);
    fr.observe(clipped_box_416(), true, 7);               // 腿被 928 截断
    DetectionBox out{};
    CHECK(fr.frozen_for(7, &out));
    CHECK(near(out.y1, 400.0f));
    CHECK(near(out.y2, 900.0f));
    CHECK(!near(out.y2, 928.0f));                          // 没被截断帧污染
}

// ㉕ 换目标（track id 变）：旧冻结框立即作废，新目标要等第一次未截断帧才重建。
TEST(frozen_rect_invalidates_on_target_switch) {
    FrozenRectTracker fr;
    fr.observe(mk(1000, 400, 1180, 900), false, 1);
    DetectionBox out{};
    CHECK(fr.frozen_for(1, &out));
    fr.observe(clipped_box_416(), true, 2);                // 新目标、且被截
    CHECK(!fr.frozen_for(1, &out));                        // 老目标的框已作废
    CHECK(!fr.frozen_for(2, &out));                        // 新目标还没见过完整框
    fr.observe(mk(1080, 380, 1260, 880), false, 2);
    CHECK(fr.frozen_for(2, &out));
    CHECK(near(out.y2, 880.0f));
}

// ㉖ 目标一出现就被截（拐角撞脸）：没有可冻结的帧 ⇒ false，调用方退回 V1.0.09 外推兜底。
TEST(frozen_rect_no_baseline_returns_false) {
    FrozenRectTracker fr;
    fr.observe(clipped_box_416(), true, 9);
    DetectionBox out{};
    CHECK(!fr.frozen_for(9, &out));
    CHECK(!fr.valid());
}

// ㉗ 退化框（宽或高 <= 1px）不参与冻结；reset 清空。
TEST(frozen_rect_ignores_degenerate_box_and_resets) {
    FrozenRectTracker fr;
    fr.observe(mk(1200, 500, 1200.5f, 900), false, 4);     // 宽 0.5
    CHECK(!fr.valid());
    fr.observe(mk(1100, 800, 1300, 800.5f), false, 4);     // 高 0.5
    CHECK(!fr.valid());
    fr.observe(mk(1100, 400, 1280, 900), false, 4);
    CHECK(fr.valid());
    fr.reset();
    CHECK(!fr.valid());
    CHECK(fr.id() == -1);
}

// ㉘ 端到端：走近一帧，冻结落点不动；直接吃这一帧会抬头 ~40px。
TEST(frozen_rect_end_to_end_landing_point_does_not_drift) {
    auto prof = board_prof();
    FrozenRectTracker fr;
    const DetectionBox full = mk(1080, 420, 1250, 900);    // w=170 h=480
    fr.observe(full, false, 7);
    float tx = 0, ty = 0;
    CHECK(aim_point_at(full, 5, prof, &tx, &ty, -1.0f, 0.0f));
    const float ty_far = ty;                               // 420 + 0.31*480 = 568.8
    CHECK(near(ty_far, 568.8f));

    const DetectionBox near_clipped = mk(1075, 350, 1255, 928);   // 框顶上冒、腿被截
    fr.observe(near_clipped, true, 7);
    DetectionBox frozen{};
    CHECK(fr.frozen_for(7, &frozen));
    float fx = 0, fy = 0;
    CHECK(aim_point_at(frozen, 5, prof, &fx, &fy, -1.0f, 0.0f));
    CHECK(near(fy, ty_far));                               // 冻结 ⇒ 落点不动

    float nx = 0, ny = 0;
    CHECK(aim_point_at(near_clipped, 5, prof, &nx, &ny, -1.0f, 0.0f));
    CHECK(near(ny, 529.18f));                              // 350 + 0.31*578
    CHECK(ny < ty_far - 39.0f);                            // 不冻就抬头约 40px
}

int main() { return ttbox_test::run_all(); }
