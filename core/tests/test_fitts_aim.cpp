// test_fitts_aim.cpp — FittsAimController 单测（V1.0.43，dat58 思路的回归证据）。
//
// 钉死 Fitts 定律控制器的核心行为，防回归：
//   ① 近慢远快（远处绝对移动快、近处精调慢）
//   ② 尺寸自适应死区（误差 < max(3px, 框高×5%) 不移动）
//   ③ px → count 换算（标定增益 gain；缺失兜底不除零）
//   ④ 纯函数无状态（连续同样输入同样输出）
//   ⑤ MT 钳位 + 方向正确（超大误差不发散、输出与误差同号、单调）
//   ⑥ dt 必须实测（帧间隔线性 ⇒ 写死 dt 会系统性失配）
//   ⑦ 速度前馈方向正确 + 上限保护（补 Fitts 固有滞后，ff 过大反而抖）
#include <cmath>
#include <cstdio>
#include <limits>

#include "aim/FittsAimController.hpp"
#include "test_util.hpp"

namespace {
constexpr float kGain = 0.65f;  // 标定增益（px/count）
constexpr float kDt = 7.0f;     // 典型帧间隔（ms，144fps≈6.9）
constexpr float kBoxH = 100.0f;  // 典型目标框高（px）

bool near(float a, float b, float eps) { return std::fabs(a - b) < eps; }
}  // namespace

// ① 近慢远快：大误差的绝对移动量 > 小误差（远处快拉、近处精调）
TEST(fitts_far_moves_faster_than_near) {
    ttbox::core::aim::FittsAimController c;
    c.configure(20.0f, 20.0f, 3.0f, 0.05f, 0.0f);  // ff_gain=0 隔离前馈
    const float near_move = c.update(10.0f, kBoxH, kGain, 0.0f, kDt);
    const float far_move = c.update(100.0f, kBoxH, kGain, 0.0f, kDt);
    CHECK(far_move > near_move);
    CHECK(far_move > 0.0f && near_move > 0.0f);
    // 近处相对误差的比例更高（收敛更快）
    CHECK(near_move / 10.0f > far_move / 100.0f);
}

// ② 尺寸自适应死区：误差 < max(3px, 框高×5%) 不移动；超出则动
TEST(fitts_deadzone_adaptive_to_box) {
    ttbox::core::aim::FittsAimController c;
    c.configure(20.0f, 20.0f, 3.0f, 0.05f, 0.0f);
    // 框高 100 ⇒ 死区 = max(3, 5) = 5px
    CHECK_EQ(c.update(4.0f, 100.0f, kGain, 0.0f, kDt), 0.0f);
    CHECK_EQ(c.update(-4.0f, 100.0f, kGain, 0.0f, kDt), 0.0f);
    CHECK(c.update(6.0f, 100.0f, kGain, 0.0f, kDt) > 0.0f);
    // 框高 300 ⇒ 死区 = max(3, 15) = 15px（近目标框大死区大）
    CHECK_EQ(c.update(10.0f, 300.0f, kGain, 0.0f, kDt), 0.0f);
    CHECK(c.update(20.0f, 300.0f, kGain, 0.0f, kDt) > 0.0f);
    // 框高 20 ⇒ 死区 = max(3, 1) = 3px（像素下限兜底）
    CHECK_EQ(c.update(2.0f, 20.0f, kGain, 0.0f, kDt), 0.0f);
    CHECK(c.update(4.0f, 20.0f, kGain, 0.0f, kDt) > 0.0f);
}

// ③ px → count 换算：输出 = px 移动 / gain；gain 缺失兜底 0.65 不除零
TEST(fitts_px_to_count_via_gain) {
    ttbox::core::aim::FittsAimController c;
    c.configure(20.0f, 20.0f, 3.0f, 0.05f, 0.0f);
    const float out_g1 = c.update(100.0f, kBoxH, 1.0f, 0.0f, kDt);
    const float out_g065 = c.update(100.0f, kBoxH, kGain, 0.0f, kDt);
    // gain 越小，同样 px 移动需要的 count 越多（count = px/gain）
    CHECK(out_g065 > out_g1);
    CHECK(near(out_g065 / out_g1, 1.0f / kGain, 0.01f));
    const float out_bad = c.update(100.0f, kBoxH, 0.0f, 0.0f, kDt);
    CHECK(near(out_bad, out_g065, 1e-3f));
}

// ④ 纯函数无状态：连续同样输入返回同样输出（不像 pid1 有积分惯性）
TEST(fitts_is_pure_function) {
    ttbox::core::aim::FittsAimController c;
    c.configure(20.0f, 20.0f, 3.0f, 0.05f, 0.0f);
    const float a = c.update(50.0f, kBoxH, kGain, 0.0f, kDt);
    const float b = c.update(50.0f, kBoxH, kGain, 0.0f, kDt);
    const float d = c.update(50.0f, kBoxH, kGain, 0.0f, kDt);
    CHECK(near(a, b, 1e-6f));
    CHECK(near(b, d, 1e-6f));
}

// ⑤ MT 钳位 + 方向正确：超大误差不发散、输出与误差同号、单调
TEST(fitts_large_error_bounded_and_signed) {
    ttbox::core::aim::FittsAimController c;
    c.configure(20.0f, 20.0f, 3.0f, 0.05f, 0.0f);
    const float pos = c.update(500.0f, kBoxH, kGain, 0.0f, kDt);
    const float neg = c.update(-500.0f, kBoxH, kGain, 0.0f, kDt);
    CHECK(pos > 0.0f);
    CHECK(neg < 0.0f);
    CHECK(near(pos, -neg, 1e-3f));  // 对称
    CHECK(pos < 50.0f);              // MT 钳位：一帧不打飞
    const float m50 = c.update(50.0f, kBoxH, kGain, 0.0f, kDt);
    const float m200 = c.update(200.0f, kBoxH, kGain, 0.0f, kDt);
    const float m500 = c.update(500.0f, kBoxH, kGain, 0.0f, kDt);
    CHECK(m200 > m50);
    CHECK(m500 > m200);
}

// ⑥ ★dt 必须实测：Fitts 是时间模型，帧间隔加倍 ⇒ 单帧移动加倍（线性）。
//    写死 dt 会让 4ms/12ms 的帧率抖动变成 ±50% 的系统性速度失配。
TEST(fitts_scales_linearly_with_measured_dt) {
    ttbox::core::aim::FittsAimController c;
    c.configure(20.0f, 20.0f, 3.0f, 0.05f, 0.0f);
    const float at_7 = c.update(100.0f, kBoxH, kGain, 0.0f, 7.0f);
    const float at_14 = c.update(100.0f, kBoxH, kGain, 0.0f, 14.0f);
    CHECK(near(at_14 / at_7, 2.0f, 0.01f));   // dt 加倍 ⇒ 输出加倍
    // dt 非法（0）走兜底，不炸不出 0
    CHECK(c.update(100.0f, kBoxH, kGain, 0.0f, 0.0f) > 0.0f);
    CHECK(c.update(100.0f, kBoxH, kGain, 0.0f, -5.0f) > 0.0f);
    // dt 超大（长卡顿）被钳到 40ms ⇒ 不会一帧倾泻
    const float at_stall = c.update(100.0f, kBoxH, kGain, 0.0f, 1000.0f);
    CHECK(at_stall > 0.0f);
    CHECK(at_stall < at_7 * 8.0f);
}

// ⑦ ★速度前馈：目标同向移动 ⇒ 输出加大（补滞后）；反向 ⇒ 减小；上限保护生效
TEST(fitts_velocity_feedforward_reduces_lag) {
    ttbox::core::aim::FittsAimController c;
    c.configure(20.0f, 20.0f, 3.0f, 0.05f, 0.0f);
    const float no_ff = c.update(100.0f, kBoxH, kGain, 0.0f, kDt);
    c.configure(20.0f, 20.0f, 3.0f, 0.05f, 0.6f);
    const float with_ff = c.update(100.0f, kBoxH, kGain, 200.0f, kDt);
    CHECK(with_ff > no_ff);  // 同向 ⇒ 追得更快（补滞后）
    // 反向移动：符号护栏接管（见 fitts_feedforward_never_reverses_output）——
    // 前馈被钳到不改变误差符号，输出**不会反向**，只会比无前馈更小（甚至归零）。
    const float against = c.update(100.0f, kBoxH, kGain, -200.0f, kDt);
    CHECK(against <= no_ff);
    CHECK(against > 0.0f);  // 仍与误差同向，绝不反向
    // ff_gain 上限保护：给到 5.0 等效于钳到 0.85（防速度噪声放大成抖）
    c.configure(20.0f, 20.0f, 3.0f, 0.05f, 5.0f);
    const float ff_huge = c.update(100.0f, kBoxH, kGain, 200.0f, kDt);
    c.configure(20.0f, 20.0f, 3.0f, 0.05f, 0.85f);
    const float ff_cap = c.update(100.0f, kBoxH, kGain, 200.0f, kDt);
    CHECK(near(ff_huge, ff_cap, 1e-3f));
}

// ⑧ 默认参数 = 仿真定案（A=20/B=20/dz=3/ff=0.6）：不 configure 也能用
TEST(fitts_defaults_are_sim_tuned) {
    ttbox::core::aim::FittsAimController c;  // 不 configure，吃默认
    // 默认死区 = max(3, 100×0.05) = 5px ⇒ 4px 不动、6px 动
    CHECK_EQ(c.update(4.0f, 100.0f, kGain, 0.0f, kDt), 0.0f);
    CHECK(c.update(6.0f, 100.0f, kGain, 0.0f, kDt) > 0.0f);
    // 默认带前馈 ff=0.6：同向移动目标应比静止更快
    ttbox::core::aim::FittsAimController still_only;
    still_only.configure(20.0f, 20.0f, 3.0f, 0.05f, 0.0f);
    const float still = still_only.update(100.0f, 100.0f, kGain, 0.0f, kDt);
    CHECK(c.update(100.0f, 100.0f, kGain, 200.0f, kDt) > still);
}

// ⑨ ★NaN/Inf 输入必须被挡住：abs(NaN)<min_zone 恒假会穿过死区，
//    log2(NaN)=NaN 一路泄漏到 HID 输出（int16 转 NaN 是未定义行为 ⇒ 输出链被毁）。
//    上游 tracker 偶发吐 NaN（除零/时间跳变）时，这道闸是最后防线。
TEST(fitts_rejects_non_finite_input) {
    ttbox::core::aim::FittsAimController c;
    c.configure(20.0f, 20.0f, 3.0f, 0.05f, 0.6f);
    const float nan = std::nanf("");
    const float inf = std::numeric_limits<float>::infinity();
    CHECK_EQ(c.update(nan, 100.0f, kGain, 0.0f, kDt), 0.0f);        // error=NaN
    CHECK_EQ(c.update(100.0f, nan, kGain, 0.0f, kDt), 0.0f);         // box_h=NaN
    CHECK_EQ(c.update(100.0f, 100.0f, kGain, nan, kDt), 0.0f);       // vel=NaN
    CHECK_EQ(c.update(inf, 100.0f, kGain, 0.0f, kDt), 0.0f);        // error=Inf
    CHECK_EQ(c.update(100.0f, 100.0f, kGain, inf, kDt), 0.0f);       // vel=Inf
    CHECK_EQ(c.update(-inf, -inf, kGain, -inf, kDt), 0.0f);         // 全 Inf
    // 病态增益（Inf）也不该产出 NaN/Inf
    const float out = c.update(100.0f, 100.0f, inf, 0.0f, kDt);
    CHECK(std::isfinite(out));
}

// ⑩ ★反向过冲护栏：目标急速反向时，前馈不能把输出推到反方向
//    （准星往回跑比"跟慢一点"更糟）。前馈只允许加速同向收敛。
//    注意：当 |ff| > |err| 时护栏把 ff 钳到 |err|，error+ff 恰好为 0 ⇒ 输出归零，
//    这也是「不反向」的正确形态（宁可不追，也不往回跑）。
TEST(fitts_feedforward_never_reverses_output) {
    ttbox::core::aim::FittsAimController c;
    c.configure(20.0f, 20.0f, 3.0f, 0.05f, 0.6f);
    // 误差为正、目标向左急冲（vel 很负）：输出必须**不为负**
    for (float vel : {-100.0f, -500.0f, -2000.0f, -10000.0f}) {
        const float out = c.update(50.0f, kBoxH, kGain, vel, kDt);
        CHECK(out >= 0.0f);
    }
    // 误差为负、目标向右急冲：输出必须**不为正**
    for (float vel : {100.0f, 500.0f, 2000.0f, 10000.0f}) {
        const float out = c.update(-50.0f, kBoxH, kGain, vel, kDt);
        CHECK(out <= 0.0f);
    }
    // 中等反向（|ff| < |err|）：应当减速但仍同向（不归零、不反向）
    const float mild = c.update(200.0f, kBoxH, kGain, -200.0f, kDt);
    CHECK(mild > 0.0f);
}

int main() {
    std::printf("=== ttbox_core tests (fitts_aim) ===\n");
    const int failed = ::ttbox_test::run_all();
    std::printf("=== tests done (exit=%d) ===\n", failed == 0 ? 0 : 1);
    return failed == 0 ? 0 : 1;
}
