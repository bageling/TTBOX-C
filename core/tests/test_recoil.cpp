// test_recoil.cpp — 压枪引擎（recoil assist）单元测试
//
// 覆盖场景：
//   Case1  未启用（enabled=false）→ 零输出
//   Case2  启用 + 热键按住 + 有目标 + strength>0 → Y 下压输出
//   Case3  热键未按 → 零输出
//   Case4  only_when_target_visible=true + 无目标 → 零输出
//   Case5  目标丢失保持窗口：丢失 200ms 内继续压，超时停止
//   Case6  trigger_delay：按住 <120ms 不压，>=120ms 开始压
//   Case7  strength=0 → 零输出（有热键有目标也不压）
//   Case8  hotkey_mode=all：需双键同时按下
//   Case9  亚像素残差累计：低速压枪小数不丢（多帧累加输出）
//   Case10 缓入缓出 ramp：首帧输出 < 稳态输出
//   Case11 X 轴微动已删除：jitter_px 不再被消费，X 恒为 0
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
    c.target_lost_release_ms = 200.0f;
    c.trigger_delay_enabled = false;
    c.trigger_delay_ms = 120.0f;
    c.strength = 60.0f;       // px/s
    c.speed = 1.0f;
    c.humanize_enabled = true;
    c.humanize_curve_strength = 1.0f;   // 快速 ramp（便于测试）
    c.humanize_jitter_px = 0.0f;        // 字段保留兼容老配置；X 微动已于 2026-09-29 删除
    c.humanize_jitter_frequency = 8.0f;
    return c;
}

constexpr float kPpc = 0.65f;   // px/count
constexpr float kDtMs = 16.0f;  // ~60fps

// Case1: 未启用 → 零输出
void test_disabled() {
    std::printf("[Case1] 未启用（enabled=false）→ 零输出\n");
    RecoilController rc;
    RecoilConfig cfg = make_cfg();
    cfg.enabled = false;
    const auto d = rc.update(0x01, true, cfg, kDtMs, kPpc);
    check(d.y == 0.0f && d.x == 0.0f, "enabled=false 时零输出");
    check(!rc.active(), "enabled=false 时 active=false");
}

// Case2: 启用 + 热键 + 有目标 → Y 下压
void test_basic_pull() {
    std::printf("[Case2] 启用 + 左键按住 + 有目标 → Y 下压\n");
    RecoilController rc;
    RecoilConfig cfg = make_cfg();
    float total = 0.0f;
    for (int i = 0; i < 64; ++i) {  // ~1.0s，ramp 已到顶
        const auto d = rc.update(0x01, true, cfg, kDtMs, kPpc);
        total += d.y;
    }
    // strength=60px/s × 1.0s = 60px = 92.3 count（ramp 到顶后全速）
    check(total > 80.0f, "1s 累计下压 > 80 count");
    check(rc.active(), "压枪激活中 active=true");
}

// Case3: 热键未按 → 零输出
void test_no_hotkey() {
    std::printf("[Case3] 热键未按 → 零输出\n");
    RecoilController rc;
    RecoilConfig cfg = make_cfg();
    const auto d = rc.update(0x00, true, cfg, kDtMs, kPpc);
    check(d.y == 0.0f && d.x == 0.0f, "无热键时零输出");
}

// Case4: 仅有目标才压 + 无目标 → 零输出
void test_no_target() {
    std::printf("[Case4] only_when_target_visible + 无目标 → 零输出\n");
    RecoilController rc;
    RecoilConfig cfg = make_cfg();
    float total = 0.0f;
    for (int i = 0; i < 30; ++i) {
        const auto d = rc.update(0x01, false, cfg, kDtMs, kPpc);
        total += d.y;
    }
    check(total == 0.0f, "无目标时全程零输出");
}

// Case5: 目标丢失保持窗口
void test_target_lost_release() {
    std::printf("[Case5] 目标丢失保持窗口（200ms 内继续压，超时停止）\n");
    RecoilController rc;
    RecoilConfig cfg = make_cfg();
    // 先有目标压 10 帧建立状态
    for (int i = 0; i < 10; ++i) rc.update(0x01, true, cfg, kDtMs, kPpc);
    // 丢失目标：200ms 窗口（约 12.5 帧）内应继续输出
    float in_window = 0.0f;
    for (int i = 0; i < 8; ++i) {  // 8 × 16ms = 128ms < 200ms
        const auto d = rc.update(0x01, false, cfg, kDtMs, kPpc);
        in_window += d.y;
    }
    check(in_window > 0.0f, "丢失 128ms 内仍压枪");
    // 超过窗口后目标门控关闭：继续压足够长时间后输出为 0
    RecoilController rc2;
    RecoilConfig cfg2 = make_cfg();
    for (int i = 0; i < 10; ++i) rc2.update(0x01, true, cfg2, kDtMs, kPpc);
    for (int i = 0; i < 200; ++i) rc2.update(0x01, false, cfg2, kDtMs, kPpc);  // 3.2s 远超窗口
    const auto d = rc2.update(0x01, false, cfg2, kDtMs, kPpc);
    check(d.y == 0.0f && d.x == 0.0f, "远超丢失窗口后零输出");
}

// Case6: trigger_delay 延迟触发
void test_trigger_delay() {
    std::printf("[Case6] trigger_delay：按住 <120ms 不压，>=120ms 开始压\n");
    RecoilController rc;
    RecoilConfig cfg = make_cfg();
    cfg.trigger_delay_enabled = true;
    cfg.trigger_delay_ms = 120.0f;
    // 前 6 帧（96ms < 120ms）不应输出
    float before = 0.0f;
    for (int i = 0; i < 6; ++i) {
        const auto d = rc.update(0x01, true, cfg, kDtMs, kPpc);
        before += d.y;
    }
    check(before == 0.0f, "96ms 内不压枪");
    // 继续按到 8 帧（128ms >= 120ms）开始输出
    float after = 0.0f;
    for (int i = 0; i < 20; ++i) {
        const auto d = rc.update(0x01, true, cfg, kDtMs, kPpc);
        after += d.y;
    }
    check(after > 0.0f, "超过 120ms 后开始压枪");
}

// Case7: strength=0 → 零输出
void test_zero_strength() {
    std::printf("[Case7] strength=0 → 零输出\n");
    RecoilController rc;
    RecoilConfig cfg = make_cfg();
    cfg.strength = 0.0f;
    const auto d = rc.update(0x01, true, cfg, kDtMs, kPpc);
    check(d.y == 0.0f && d.x == 0.0f, "strength=0 时零输出");
}

// Case8: hotkey_mode=all 需双键同时按下
void test_hotkey_all_mode() {
    std::printf("[Case8] hotkey_mode=all：需双键同时按下\n");
    RecoilController rc;
    RecoilConfig cfg = make_cfg();
    cfg.hotkey = 0x01;   // left
    cfg.hotkey2 = 0x04;  // middle
    cfg.hotkey_mode = 2; // all
    // 只按左键 → 不压
    float single = 0.0f;
    for (int i = 0; i < 20; ++i) {
        const auto d = rc.update(0x01, true, cfg, kDtMs, kPpc);
        single += d.y;
    }
    check(single == 0.0f, "只按主键不压");
    // 双键同按 → 压
    RecoilController rc2;
    float both = 0.0f;
    for (int i = 0; i < 20; ++i) {
        const auto d = rc2.update(0x01 | 0x04, true, cfg, kDtMs, kPpc);
        both += d.y;
    }
    check(both > 0.0f, "双键同按开始压");
}

// Case9: 亚像素残差累计（低速不丢精度）
void test_residual() {
    std::printf("[Case9] 亚像素残差累计：低速压枪小数不丢\n");
    RecoilController rc;
    RecoilConfig cfg = make_cfg();
    cfg.strength = 4.0f;   // 4px/s → 每帧 4×0.016/0.65 = 0.098 count，远小于 1
    float total = 0.0f;
    for (int i = 0; i < 100; ++i) {  // 1.6s
        const auto d = rc.update(0x01, true, cfg, kDtMs, kPpc);
        total += d.y;
    }
    // 期望 ≈ 4px/s × 1.6s / 0.65 = 9.85 count
    check(total > 7.0f, "低速 1.6s 累计 > 7 count（残差不丢）");
}

// Case10: 缓入缓出 ramp
void test_ramp() {
    std::printf("[Case10] 缓入缓出 ramp：首帧输出 < 稳态输出\n");
    RecoilController rc;
    RecoilConfig cfg = make_cfg();
    cfg.humanize_curve_strength = 0.5f;  // 较慢 ramp
    const auto first = rc.update(0x01, true, cfg, kDtMs, kPpc);
    // 稳态（ramp 满）：strength=60px/s × 0.016s / 0.65 = 1.48 count/帧
    float steady = 0.0f;
    for (int i = 0; i < 60; ++i) {
        const auto d = rc.update(0x01, true, cfg, kDtMs, kPpc);
        steady = d.y;
    }
    check(first.y < steady, "首帧输出小于稳态（缓入）");
    check(steady > 0.0f, "稳态输出 > 0");
}

// Case11: X 轴微动已删除（2026-09-29）
//   旧实现：压枪时叠加固定 8Hz 正弦 X 微动（humanize_jitter_px，默认 0.25px）。
//   该段属 TTBOX 自研的 4 套固定正弦抖动之一，非 BB 来源，已整段删除。
//   现在的判据：**即使显式给非零 jitter_px，X 也必须恒为 0**（锁死删除行为）。
void test_jitter() {
    std::printf("[Case11] X 轴微动已删除：给非零 jitter_px，X 仍恒为 0\n");
    RecoilController rc;
    RecoilConfig cfg = make_cfg();
    cfg.humanize_jitter_px = 0.25f;   // 字段仍在，但不再有消费点
    float x_sum = 0.0f;
    for (int i = 0; i < 60; ++i) {
        const auto d = rc.update(0x01, true, cfg, kDtMs, kPpc);
        x_sum += d.x;
    }
    check(x_sum == 0.0f, "jitter_px=0.25 时 X 仍为 0（抖动已删除）");
    // 默认档复核
    RecoilController rc2;
    RecoilConfig cfg2 = make_cfg();
    cfg2.humanize_jitter_px = 0.0f;
    float x_sum2 = 0.0f;
    for (int i = 0; i < 60; ++i) {
        const auto d = rc2.update(0x01, true, cfg2, kDtMs, kPpc);
        x_sum2 += d.x;
    }
    check(x_sum2 == 0.0f, "jitter_px=0 时 X 为 0");
}

}  // namespace

int main() {
    std::printf("=== test_recoil 压枪引擎测试 ===\n");
    test_disabled();
    test_basic_pull();
    test_no_hotkey();
    test_no_target();
    test_target_lost_release();
    test_trigger_delay();
    test_zero_strength();
    test_hotkey_all_mode();
    test_residual();
    test_ramp();
    test_jitter();
    std::printf("结果: %d failures\n", failures);
    return failures == 0 ? 0 : 1;
}
