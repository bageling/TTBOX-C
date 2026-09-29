// test_aim_point_clip.cpp — V1.0.08：框底被裁剪区下边界截断时的落点外推。
//
// 定障来源（2026-09-29 板端记录器 + 离线复算，不是拍脑袋）：
//   业主症状「瞄胸口、走进目标以后准星就跑到头上」。
//   机理：crop 只取画面中心 640x640（半径 320px）⇒ 近身目标的下半身落在 crop 之外，
//     可见框高 h_obs = crop 下边(1040) - y1 比真实身高小 ⇒ ty = y1 + 0.31*h_obs 相对人体上飘。
//     越近截得越多，所以是「走进去才飘」而不是「一直偏」。
//   实测（tail_trace，h>=250 的 161 帧）：
//     - 框底贴到裁剪区下边的 54 帧：h_obs p50=456，按框宽反推身高 p50=654 ⇒ 被截 191px(29%)
//     - 落点比真身位置高 p50=59px、p90=80px、max=210px（≈身高 30% ⇒ 正好从胸口到头顶）
//   cls5 的人体框 宽/高 在三批记录里都稳定：p50=0.32、p10-p90=0.30~0.34 ⇒ 可用框宽反推身高。
//
// 板端口径（GET_CONFIG 实测）：整帧 2560x1440，capture 640x640 居中 ⇒ 裁剪区
//   x[960,1600] y[400,1040]；crop_bottom = 1440/2 + 0 + 320 = 1040；offset = (0.5, 0.31)。
#include <cmath>

#include "mouse/AimPointProfile.hpp"
#include "test_util.hpp"

using namespace ttbox::core;
using namespace ttbox::core::aim;

namespace {

constexpr float kBoardCropBottom = 1040.0f;   // 板端裁剪区下边界（全帧像素）

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

// 板端现场复刻：框底压在裁剪区下边的近身大框
//   w = 209, h = 456, y1 = 584, y2 = 1040
DetectionBox board_clipped_box() { return mk(1150, 584, 1359, 1040); }

}  // namespace

// ① 不传 crop_bottom（旧调用口径）⇒ 行为与加此机制前逐字节一致。
TEST(aim_clip_legacy_signature_unchanged) {
    auto prof = board_prof();
    float tx = 0, ty = 0;
    CHECK(aim_point_at(board_clipped_box(), 5, prof, &tx, &ty));
    CHECK(near(tx, 1150.0f + 0.5f * 209.0f));          // 1254.5
    CHECK(near(ty, 584.0f + 0.31f * 456.0f));          // 725.36
}

// ② crop_bottom 未知（<=0）⇒ 不做外推，与旧口径一致。
TEST(aim_clip_unknown_bottom_no_extrapolate) {
    auto prof = board_prof();
    float tx = 0, ty = 0;
    CHECK(aim_point_at(board_clipped_box(), 5, prof, &tx, &ty, -1.0f));
    CHECK(near(ty, 725.36f));
    CHECK(aim_point_at(board_clipped_box(), 5, prof, &tx, &ty, 0.0f));
    CHECK(near(ty, 725.36f));
}

// ③ 框底没贴到裁剪区下边 ⇒ 框高可信 ⇒ 不做外推（远/中距离目标的常态）。
TEST(aim_clip_box_below_bottom_no_extrapolate) {
    auto prof = board_prof();
    const DetectionBox b = mk(1150, 300, 1359, 756);   // y2=756，离 1040 还差 284
    float tx = 0, ty = 0;
    CHECK(aim_point_at(b, 5, prof, &tx, &ty, kBoardCropBottom));
    CHECK(near(ty, 300.0f + 0.31f * 456.0f));          // 441.36，未被改动
}

// ④ 贴边 ⇒ 用框宽反推身高：h_eff = w / 0.32 = 653.125 ⇒ 落点下移。
TEST(aim_clip_bottom_hits_extrapolate_by_width) {
    auto prof = board_prof();
    float tx = 0, ty = 0;
    CHECK(aim_point_at(board_clipped_box(), 5, prof, &tx, &ty, kBoardCropBottom));
    CHECK(near(ty, 584.0f + 0.31f * (209.0f / 0.32f)));   // 786.47
    CHECK(ty > 725.36f + 50.0f);                          // 明显下移
}

// ⑤ 现场量化：修正量应与板端实测吻合（dy p50 = 59px，此处 61.1px）。
TEST(aim_clip_extrapolate_amount_matches_board_measurement) {
    auto prof = board_prof();
    float tx0 = 0, ty0 = 0, tx1 = 0, ty1 = 0;
    CHECK(aim_point_at(board_clipped_box(), 5, prof, &tx0, &ty0));                 // 旧口径
    CHECK(aim_point_at(board_clipped_box(), 5, prof, &tx1, &ty1, kBoardCropBottom));  // 修正后
    const float dy = ty1 - ty0;
    CHECK(dy > 55.0f && dy < 70.0f);   // 实测 p50=59 ⇒ 61.1 落在窗口内
    CHECK(near(tx0, tx1));             // X 不参与外推
}

// ⑥ 只放大、不缩小：框比「按宽度反推」更高时保持原高（防把落点往上拽）。
TEST(aim_clip_extrapolate_never_shrinks) {
    auto prof = board_prof();
    const DetectionBox b = mk(1200, 640, 1250, 1040);   // w=50 ⇒ h_from_w=156 < h=400
    float tx = 0, ty = 0;
    CHECK(aim_point_at(b, 5, prof, &tx, &ty, kBoardCropBottom));
    CHECK(near(ty, 640.0f + 0.31f * 400.0f));          // 764，与旧口径一致
}

// ⑦ 上限：夸张宽框（两人重叠等）最多按 3 倍可见框高外推，防止把落点推到脚下。
TEST(aim_clip_extrapolate_capped_at_3x) {
    auto prof = board_prof();
    const DetectionBox b = mk(300, 940, 2300, 1040);   // w=2000 ⇒ h_from_w=6250，h=100
    float tx = 0, ty = 0;
    CHECK(aim_point_at(b, 5, prof, &tx, &ty, kBoardCropBottom));
    CHECK(near(ty, 940.0f + 0.31f * 300.0f));          // h_eff 被 3h=300 卡住 ⇒ 1033（不是 2877.5）
}

// ⑧ 开关关掉 ⇒ 完全回到 V1.0.07 行为（留 A/B 通路）。
TEST(aim_clip_switch_off_restores_legacy) {
    auto prof = board_prof();
    prof.clip_bottom_extrapolate = false;
    float tx = 0, ty = 0;
    CHECK(aim_point_at(board_clipped_box(), 5, prof, &tx, &ty, kBoardCropBottom));
    CHECK(near(ty, 725.36f));
}

// ⑨ margin 生效：框底离下边超过 clip_bottom_margin_px 就不算贴边。
TEST(aim_clip_margin_respected) {
    auto prof = board_prof();
    prof.clip_bottom_margin_px = 12.0f;                 // 判据：y2 >= 1040-12 = 1028
    float tx = 0, ty = 0;
    const DetectionBox just_out = mk(1150, 564, 1359, 1020);   // y2=1020 < 1028
    CHECK(aim_point_at(just_out, 5, prof, &tx, &ty, kBoardCropBottom));
    CHECK(near(ty, 564.0f + 0.31f * 456.0f));           // 未外推
    const DetectionBox just_in = mk(1150, 572, 1359, 1028);    // y2=1028 == 阈值
    CHECK(aim_point_at(just_in, 5, prof, &tx, &ty, kBoardCropBottom));
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
    CHECK(aim_point_at(board_clipped_box(), 5, prof, &tx, &ty, kBoardCropBottom));
    CHECK(near(ty, 584.0f + 0.45f * (209.0f / 0.32f))); // 878.0
    // 类别 6 没配 ⇒ 用全局 0.31
    CHECK(aim_point_at(board_clipped_box(), 6, prof, &tx, &ty, kBoardCropBottom));
    CHECK(near(ty, 584.0f + 0.31f * (209.0f / 0.32f)));
}

int main() { return ttbox_test::run_all(); }
