// test_aim_head_box.cpp — 几何配对识头（不依赖类别号）：落点取「头部小框」正中心。
//
// 背景（业主 2026-09-30 指令二）：模型同一目标给出「大框(身体)+小框(头)」两个框，
//   但**无法确定头部类别号**。训练场「单个目标只有两个框」场景下，落点应该瞄小框正中心。
//   判据纯几何：小框被大框包住、面积明显更小（≤0.5）、中心落在大框上半部。
//
// ★ 只动「控制链落点」（AimThread 的 tx/ty → 平滑 → PID）；显示框 / measurement_valid /
//   冻结判定仍用身体框 —— 「逻辑 / 视频两条线」不混。
#include <cmath>

#include "model/RuntimeProfile.hpp"
#include "mouse/AimPointProfile.hpp"
#include "test_util.hpp"

using namespace ttbox::core;
using namespace ttbox::core::aim;

namespace {

DetectionBox mk(float x1, float y1, float x2, float y2, int cls = 0) {
    DetectionBox b;
    b.x1 = x1; b.y1 = y1; b.x2 = x2; b.y2 = y2;
    b.score = 0.9f;
    b.class_id = cls;
    return b;
}

bool near(float a, float b, float eps = 0.5f) { return std::fabs(a - b) <= eps; }

// 训练场典型：身体大框 + 头部小框（小框被大框包住、面积 ~1/8、在大框顶部）
DetectionBox body() { return mk(100, 100, 300, 500); }
DetectionBox head() { return mk(170, 110, 230, 200); }   // 60x90，中心 (200,155)，在 body 上半部

}  // namespace

// ① 命中：body + head，返回 head，中心即头部小框正中心。
TEST(head_box_pair_hits_head) {
    std::vector<DetectionBox> dets = {body(), head()};
    DetectionBox out{};
    CHECK(resolve_head_box(body(), dets, &out));
    CHECK(near(out.x1, 170.0f) && near(out.y1, 110.0f));
    const float cx = (out.x1 + out.x2) * 0.5f;
    const float cy = (out.y1 + out.y2) * 0.5f;
    CHECK(near(cx, 200.0f));
    CHECK(near(cy, 155.0f));
}

// ② 类别号无关：head 与 body 不同 class（甚至 head 类号未知/任意），照样按几何命中。
TEST(head_box_ignores_class_id) {
    std::vector<DetectionBox> dets = {body(), mk(170, 110, 230, 200, 7)};   // 头类号=7
    DetectionBox out{};
    CHECK(resolve_head_box(body(), dets, &out));
    CHECK(near(out.y1, 110.0f));
}

// ③ 只给一个身体框（模型没单独出头框）⇒ 不命中，退回身体框落点。
TEST(head_box_single_body_no_match) {
    std::vector<DetectionBox> dets = {body()};
    DetectionBox out{};
    CHECK(!resolve_head_box(body(), dets, &out));
}

// ④ ref 自己在 dets 里也不能自己配自己（坐标完全一致的框要跳过）。
TEST(head_box_skips_ref_itself) {
    std::vector<DetectionBox> dets = {body()};   // 只有 ref
    DetectionBox out{};
    CHECK(!resolve_head_box(body(), dets, &out));
}

// ⑤ 面积不够小（≥ ref 面积 × 0.5）⇒ 不当头（防把躯干/大框误认成头）。
TEST(head_box_rejects_not_much_smaller) {
    const DetectionBox big = mk(100, 100, 300, 500);          // 200x400 = 80000
    const DetectionBox nearly = mk(110, 100, 290, 350);       // 180x250 = 45000 > 40000
    std::vector<DetectionBox> dets = {big, nearly};
    DetectionBox out{};
    CHECK(!resolve_head_box(big, dets, &out));
}

// ⑥ 头框中心不在大框上半部（跑到底部）⇒ 不当头。
TEST(head_box_rejects_lower_half) {
    const DetectionBox big = mk(100, 100, 300, 500);
    const DetectionBox bottom = mk(170, 320, 230, 410);        // 中心 y=365，在 body 下半部
    std::vector<DetectionBox> dets = {big, bottom};
    DetectionBox out{};
    CHECK(!resolve_head_box(big, dets, &out));
}

// ⑦ 头框中心越出大框水平范围 ⇒ 不当头（隔壁目标的小框）。
TEST(head_box_rejects_horizontal_outside) {
    const DetectionBox big = mk(100, 100, 300, 500);
    const DetectionBox side = mk(350, 110, 410, 200);          // 中心 x=380 > 300
    std::vector<DetectionBox> dets = {big, side};
    DetectionBox out{};
    CHECK(!resolve_head_box(big, dets, &out));
}

// ⑧ 大体被包住才认：越出大框超过自身 40% 的小框不当头。
TEST(head_box_rejects_large_overflow) {
    const DetectionBox big = mk(100, 100, 300, 500);
    const DetectionBox hanging = mk(250, 110, 340, 200);        // 越出右界 40px = 宽的 44%
    std::vector<DetectionBox> dets = {big, hanging};
    DetectionBox out{};
    CHECK(!resolve_head_box(big, dets, &out));
}

// ⑨ 多个候选取面积最小者（最像头）。
TEST(head_box_picks_smallest) {
    const DetectionBox big = mk(100, 100, 300, 500);
    const DetectionBox medium = mk(150, 120, 250, 260);         // 100x140 = 14000
    const DetectionBox small = mk(170, 110, 230, 200);          // 60x90 = 5400
    std::vector<DetectionBox> dets = {big, medium, small};
    DetectionBox out{};
    CHECK(resolve_head_box(big, dets, &out));
    CHECK(near(out.y1, 110.0f));                                 // 命中 small 而非 medium
}

// ⑩ 空 dets ⇒ 不命中。
TEST(head_box_empty_dets_no_match) {
    DetectionBox out{};
    CHECK(!resolve_head_box(body(), {}, &out));
}

// ⑪ 退化框（零宽/零高）不参与、不崩溃。
TEST(head_box_ignores_degenerate) {
    const DetectionBox big = mk(100, 100, 300, 500);
    const DetectionBox zero = mk(170, 110, 170, 200);            // 宽 0
    std::vector<DetectionBox> dets = {big, zero};
    DetectionBox out{};
    CHECK(!resolve_head_box(big, dets, &out));
}

// ⑫ 配置开关 aim_at_head_box 序列化往返不丢（默认 false → 置 true → 读回仍 true）。
TEST(head_box_flag_serde_roundtrip) {
    RuntimeProfile p;
    CHECK(!p.mouse.aim_point.aim_at_head_box);           // 默认关 = 零变化
    p.mouse.aim_point.aim_at_head_box = true;
    const auto res = json_parse(p.to_json().dump());
    CHECK(res.ok);
    if (!res.ok) return;
    RuntimeProfile q = RuntimeProfile::from_json(res.value);
    CHECK(q.mouse.aim_point.aim_at_head_box == true);
}

int main() { return ttbox_test::run_all(); }
