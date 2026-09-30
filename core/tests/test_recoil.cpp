// test_recoil.cpp — 压枪引擎（recoil assist）单元测试 · 2026-09-30 对照 yu 重做
//
// 引擎语义：每帧下压 = 3·strength·speed·ramp·dt（纯 Y，开火即全量），
// 释放段 ramp = 1 − curve_strength·smoothstep(80ms)，累计量夹在 roi_h 内，残差独立结转。
//
// 覆盖场景：
//   Case1  未启用（enabled=false）→ 零输出
//   Case2  启用 + 热键按住 + 量测有效 + strength>0 → Y 下压输出
//   Case3  热键未按 → 零输出
//   Case4  only_when_target_visible=true + 从未有过有效量测 → 零输出（防空压）
//   Case5  释放保持窗：量测无效后 target_lost_release_ms 内继续压，超窗停止
//   Case6  trigger_delay：按住 <120ms 不压，>=120ms 开始压
//   Case7  strength=0 → 零输出（有热键有量测也不压）
//   Case8  hotkey_mode=all：需双键同时按下
//   Case9  亚像素残差独立结转：低速压枪小数不丢（多帧累加）
//   Case10 ★ 无缓入：首帧就是全量（旧版缓入 ramp 是本轮修掉的逻辑错）
//   Case11 释放渐出：curve_strength 越大，释放后输出衰减越快
//   Case12 roi_h 钳制：累计下压不超过 roi_h（yu 的上限保护）
//   Case13 X 恒为 0（yu 压枪是单轴引擎）
//   Case14 speed 越界（含 NaN）钉到 [0.1,3.0]
#include <cstdio>
#include <cmath>

#include "mouse/RecoilController.hpp"

using namespace ttbox::core::aim;

namespace {

int failures = 0;

void check(bool cond, const char* msg) {
    if (!cond) { std::printf("  FAIL: %s\n", msg); failures++; }
    else { std::printf("  PASS: %s\n", msg); }
}

RecoilConfig make_cfg() {
    RecoilConfig c;
    c.enabled = true;
    c.hotkey = 0x01;          // left
    c.hotkey2 = 0x00;
    c.hotkey_mode = 1;        // any
    c.only_when_target_visible = true;
    c.target_lost_release_ms = 300.0f;
    c.trigger_delay_enabled = false;
    c.trigger_delay_ms = 120.0f;
    c.strength = 100.0f;      // 拉速 = 3×100×1 = 300 px/s
    c.speed = 1.0f;
    c.curve_strength = 0.6f;
    c.roi_h = 300.0f;
    return c;
}

constexpr float kPpc = 0.65f;   // px/count
constexpr float kDtMs = 16.0f;  // ~60fps

// Case1: 未启用 → 零输出
void test_disabled() {
    std::printf("[Case1] 未启用（enabled=false）→ 零输出\n");
    RecoilController rc;
    RecoilConfig c = make_cfg();
    c.enabled = false;
    const auto d = rc.update(0x01, true, c, kDtMs, kPpc);
    check(d.y == 0.0f && d.x == 0.0f, "输出恒 0");
    check(!rc.active(), "active=false");
}

// Case2: 启用 + 热键 + 量测有效 → Y 下压
void test_basic_pull() {
    std::printf("[Case2] 启用 + 热键 + 量测有效 → Y 下压\n");
    RecoilController rc;
    const RecoilConfig c = make_cfg();
    float total = 0.0f;
    for (int i = 0; i < 10; ++i) total += rc.update(0x01, true, c, kDtMs, kPpc).y;
    check(total > 0.0f, "累计下压 > 0");
    check(rc.active(), "active=true");
    // 10 帧 × 16ms × 300px/s = 48px ⇒ /0.65 ≈ 73.8 count
    check(std::fabs(total - 73.0f) <= 3.0f, "累计量 ≈ 3·strength·speed·t/ppc");
}

// Case3: 热键未按 → 零输出
void test_not_pressed() {
    std::printf("[Case3] 热键未按 → 零输出\n");
    RecoilController rc;
    const RecoilConfig c = make_cfg();
    const auto d = rc.update(0x02, true, c, kDtMs, kPpc);
    check(d.y == 0.0f, "未按开火键不压");
}

// Case4: 从未有过有效量测 → 零输出
void test_only_when_visible_no_target() {
    std::printf("[Case4] 仅有效量测才压 + 从未有效 → 零输出\n");
    RecoilController rc;
    const RecoilConfig c = make_cfg();
    const auto d = rc.update(0x01, false, c, kDtMs, kPpc);
    check(d.y == 0.0f, "没有有效量测不压（防空压）");
}

// Case5: 释放保持窗
void test_release_window() {
    std::printf("[Case5] 释放保持窗：无效后 release_ms 内继续压，超窗停止\n");
    RecoilConfig c = make_cfg();
    // 保持窗取 16ms（= 1 帧）：第 1 个无效帧仍在窗内，第 2 帧就已经超窗
    // （取 100ms 的话"超窗"那一段里还有 4 帧是真在压的，断言会自相矛盾）。
    c.target_lost_release_ms = 16.0f;
    c.curve_strength = 0.0f;   // 先关渐出，单独看窗口
    {
        RecoilController rc;
        for (int i = 0; i < 5; ++i) rc.update(0x01, true, c, kDtMs, kPpc);
        float in_win = 0.0f;
        for (int i = 0; i < 2; ++i) in_win += rc.update(0x01, false, c, kDtMs, kPpc).y;  // 32ms
        check(in_win > 0.0f, "窗口内仍压");
        float after = 0.0f;
        for (int i = 0; i < 13; ++i) after += rc.update(0x01, false, c, kDtMs, kPpc).y; // 再 208ms
        check(after == 0.0f, "超窗后停止");
    }
    {
        RecoilController rc;
        for (int i = 0; i < 5; ++i) rc.update(0x01, true, c, kDtMs, kPpc);
        const auto d = rc.update(0x00, false, c, kDtMs, kPpc);   // 松火
        check(d.y > 0.0f, "松火瞬间仍有保持量（渐出窗口）");
    }
}

// Case6: trigger_delay
void test_trigger_delay() {
    std::printf("[Case6] trigger_delay：按住 <120ms 不压，>=120ms 开始压\n");
    RecoilConfig c = make_cfg();
    c.trigger_delay_enabled = true;
    c.trigger_delay_ms = 120.0f;
    RecoilController rc;
    float early = 0.0f;
    for (int i = 0; i < 5; ++i) early += rc.update(0x01, true, c, kDtMs, kPpc).y;  // 80ms
    check(early == 0.0f, "延迟内零输出");
    float late = 0.0f;
    for (int i = 0; i < 10; ++i) late += rc.update(0x01, true, c, kDtMs, kPpc).y;  // 累计 240ms
    check(late > 0.0f, "超过延迟后开始压");
}

// Case7: strength=0
void test_zero_strength() {
    std::printf("[Case7] strength=0 → 零输出\n");
    RecoilConfig c = make_cfg();
    c.strength = 0.0f;
    RecoilController rc;
    const auto d = rc.update(0x01, true, c, kDtMs, kPpc);
    check(d.y == 0.0f, "拉速 0 不输出");
}

// Case8: hotkey_mode=all
void test_hotkey_all() {
    std::printf("[Case8] hotkey_mode=all 需双键同时按下\n");
    RecoilConfig c = make_cfg();
    c.hotkey = 0x01;
    c.hotkey2 = 0x02;
    c.hotkey_mode = 2;
    RecoilController rc;
    check(rc.update(0x01, true, c, kDtMs, kPpc).y == 0.0f, "只按一个键不压");
    check(rc.update(0x03, true, c, kDtMs, kPpc).y > 0.0f, "两键同按才压");
}

// Case9: 亚像素残差独立结转
void test_residual() {
    std::printf("[Case9] 亚像素残差独立结转（低速多帧不丢精度）\n");
    RecoilConfig c = make_cfg();
    c.strength = 1.0f;        // 3 px/s ⇒ 每帧 0.048px ≈ 0.074 count
    RecoilController rc;
    float total = 0.0f;
    const int n = 200;
    for (int i = 0; i < n; ++i) total += rc.update(0x01, true, c, kDtMs, kPpc).y;
    // 理论：200 × 16ms × 3px/s = 9.6px ⇒ /0.65 ≈ 14.8 count
    check(std::fabs(total - 14.8f) <= 2.0f, "多帧累加逼近理论值（残差没丢）");
}

// Case10: ★ 无缓入（本轮修的逻辑错）
void test_no_ramp_in() {
    std::printf("[Case10] 无缓入：首帧即全量（旧版缓入 ≈200ms 压不住，本轮删除）\n");
    RecoilConfig c = make_cfg();
    RecoilController rc;
    const float first = rc.update(0x01, true, c, kDtMs, kPpc).y;
    RecoilController rc2;
    float steady = 0.0f;
    for (int i = 0; i < 3; ++i) steady = rc2.update(0x01, true, c, kDtMs, kPpc).y;
    check(first >= steady - 1.0f, "首帧输出不低于稳态（无渐入）");
}

// Case11: 释放渐出
void test_release_fade() {
    std::printf("[Case11] 释放渐出：curve_strength 越大衰得越快\n");
    // 逐帧比较会被 count 量化（1 count ≈ 0.65px）吃掉差异 ⇒ 比 5 帧（80ms 窗口）的累计量。
    auto run = [](float cs) {
        RecoilConfig c = make_cfg();
        c.curve_strength = cs;
        RecoilController rc;
        for (int i = 0; i < 5; ++i) rc.update(0x01, true, c, kDtMs, kPpc);
        float sum = 0.0f;
        for (int i = 0; i < 5; ++i) sum += rc.update(0x00, false, c, kDtMs, kPpc).y;
        return sum;
    };
    const float hard = run(0.0f);
    const float fade = run(1.0f);
    check(hard > 0.0f, "硬停时仍有整帧量");
    check(fade < hard, "渐出比硬停衰得快（同窗累计输出更小）");
}

// Case12: roi_h 钳制
void test_roi_clamp() {
    std::printf("[Case12] roi_h 钳制：累计下压不超过 roi_h\n");
    RecoilConfig c = make_cfg();
    c.roi_h = 10.0f;          // 10px 上限
    c.strength = 300.0f;
    c.speed = 3.0f;
    RecoilController rc;
    float total = 0.0f;
    for (int i = 0; i < 100; ++i) total += rc.update(0x01, true, c, kDtMs, kPpc).y;
    const float cap = 10.0f / kPpc;   // ≈15.4 count
    check(total <= cap + 1.5f, "累计量被 roi_h 夹住（不无限拉）");
}

// Case13: X 恒 0
void test_x_always_zero() {
    std::printf("[Case13] X 恒为 0（yu 压枪单轴）\n");
    RecoilController rc;
    const RecoilConfig c = make_cfg();
    float max_x = 0.0f;
    for (int i = 0; i < 30; ++i) {
        const float x = rc.update(0x01, true, c, kDtMs, kPpc).x;
        if (std::fabs(x) > max_x) max_x = std::fabs(x);
    }
    check(max_x == 0.0f, "X 全程 0");
}

// Case14: speed 越界钉边界
void test_speed_clamp() {
    std::printf("[Case14] speed 越界（含 NaN）钉到 [0.1,3.0]\n");
    RecoilConfig c = make_cfg();
    RecoilController a;
    c.speed = 0.0f;
    check(a.update(0x01, true, c, kDtMs, kPpc).y > 0.0f, "speed=0 → 钉到 0.1 仍出量");
    RecoilController b;
    c.speed = std::nanf("");
    check(b.update(0x01, true, c, kDtMs, kPpc).y > 0.0f, "speed=NaN → 钉到 0.1 仍出量");
}

}  // namespace

int main() {
    std::printf("=== test_recoil (yu 式速率压枪) ===\n");
    test_disabled();
    test_basic_pull();
    test_not_pressed();
    test_only_when_visible_no_target();
    test_release_window();
    test_trigger_delay();
    test_zero_strength();
    test_hotkey_all();
    test_residual();
    test_no_ramp_in();
    test_release_fade();
    test_roi_clamp();
    test_x_always_zero();
    test_speed_clamp();
    std::printf(failures == 0 ? "=== ALL PASS ===\n" : "=== FAILED: %d ===\n", failures);
    return failures == 0 ? 0 : 1;
}
