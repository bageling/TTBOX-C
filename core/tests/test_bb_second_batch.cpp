// test_bb_second_batch.cpp — BB 对标第二批模块单元测试（2026-09-24）
//
// 覆盖：
//   C. Lead2 积分累积（开关/冷却/钳制/死区衰减/Y 抑制）
//   D. HumanizeShaper（开关/过冲单调/制动/噪声有界/低通）
//   E. AntiOvershoot（开关/内圈衰减帧数/越界冷却复位）
//   F. SpeedAdaptiveKp（开关/静止乘子/移动乘子）
//   G. GlobalWave（开关/幅度有界）
//   H. RuntimeProfile 新键往返（lead2 / humanize /
//      anti_overshoot / speed_adaptive_kp / global_wave）
//
// ★ 全默认（enabled=false）零输出是硬约束：逐模块都有 "默认关 ⇒ 行为零变化" 用例。
#include <cmath>
#include <cstdio>

#include "mouse/AntiOvershoot.hpp"
#include "mouse/GlobalWave.hpp"
#include "mouse/HumanizeShaper.hpp"
#include "mouse/LeadPredictor.hpp"
#include "mouse/SpeedAdaptiveKp.hpp"
#include "mouse/SpeedFluctuation.hpp"
#include "mouse/AccuracySim.hpp"
#include "model/RuntimeProfile.hpp"

using namespace ttbox::core::aim;

namespace {

int failures = 0;

void check(bool cond, const char* msg) {
    if (!cond) {
        std::printf("  FAIL: %s\n", msg);
        ++failures;
    } else {
        std::printf("  PASS: %s\n", msg);
    }
}

constexpr float kDt = 10.0f;     // 帧间隔 10ms（100Hz）
constexpr float kPpc = 0.65f;    // px/count

// ============================ C. Lead2 ============================

void test_lead2_disabled_zero() {
    std::printf("[C1] Lead2 默认关 ⇒ 零输出\n");
    Lead2 l;
    Lead2Config cfg;  // enabled=false
    Lead2::Input in;
    in.has_target = true;
    in.target_x = 200.0f;
    in.crosshair_x = 100.0f;
    in.now_ms = 1000;
    check(l.update(cfg, in) == 0.0f, "lead2.enabled=false ⇒ 零输出");
}

void test_lead2_cooldown_then_integrate() {
    std::printf("[C2] Lead2 冷却 → 积分累积 → 上限钳制\n");
    Lead2 l;
    Lead2Config cfg;
    cfg.enabled = true;
    cfg.gain = 0.05f;
    cfg.max_offset = 25.0f;
    cfg.decay = 0.95f;
    cfg.activation_distance = 100.0f;
    cfg.dead_zone = 1.0f;
    cfg.hold_ms = 0.0f;        // 关保持窗，便于逐帧观察
    cfg.cooldown_ms = 250.0f;
    cfg.y_suppress_enabled = false;

    uint32_t t = 0;
    float first = 0.0f;
    for (int i = 0; i < 20; ++i) {   // 前 200ms：冷却中
        Lead2::Input in;
        in.has_target = true;
        in.target_x = 130.0f;        // 误差 +30px
        in.target_y = 100.0f;
        in.crosshair_x = 100.0f;
        in.crosshair_y = 100.0f;
        in.now_ms = t;
        t += 10;
        first = l.update(cfg, in);
    }
    check(first == 0.0f, "冷却期内零输出");

    float last = 0.0f;
    for (int i = 0; i < 200; ++i) {  // 冷却过后持续积分
        Lead2::Input in;
        in.has_target = true;
        in.target_x = 130.0f;
        in.target_y = 100.0f;
        in.crosshair_x = 100.0f;
        in.crosshair_y = 100.0f;
        in.now_ms = t;
        t += 10;
        last = l.update(cfg, in);
    }
    check(last > 0.0f, "持续正误差 ⇒ 正偏移");
    check(std::fabs(last - cfg.max_offset) < 1e-3f, "积分被钳制在 max_offset");
}

void test_lead2_deadzone_decay() {
    std::printf("[C3] Lead2 死区内按 decay 衰减\n");
    Lead2 l;
    Lead2Config cfg;
    cfg.enabled = true;
    cfg.gain = 0.5f;
    cfg.max_offset = 100.0f;
    cfg.decay = 0.5f;
    cfg.activation_distance = 1000.0f;
    cfg.dead_zone = 30.0f;      // 误差 30px 落在死区内
    cfg.hold_ms = 0.0f;
    cfg.cooldown_ms = 0.0f;     // 无冷却
    cfg.y_suppress_enabled = false;
    uint32_t t = 0;
    float prev = 0.0f;
    bool decaying = true;
    bool started = false;
    for (int i = 0; i < 20; ++i) {
        Lead2::Input in;
        in.has_target = true;
        in.target_x = 130.0f;   // 误差恒 30 ⇒ 死区内
        in.target_y = 100.0f;
        in.crosshair_x = 100.0f;
        in.crosshair_y = 100.0f;
        in.now_ms = t;
        t += 10;
        const float cur = l.update(cfg, in);
        if (started && cur > prev + 1e-6f) decaying = false;
        prev = cur;
        started = true;
    }
    check(decaying, "死区内积分单调不增（decay 衰减）");
}

void test_lead2_y_suppress() {
    std::printf("[C4] Lead2 Y 轴抑制（垂直输出大 ⇒ 横向提前量归零）\n");
    Lead2 l;
    Lead2Config cfg;
    cfg.enabled = true;
    cfg.gain = 0.05f;
    cfg.max_offset = 25.0f;
    cfg.activation_distance = 100.0f;
    cfg.dead_zone = 1.0f;
    cfg.hold_ms = 0.0f;
    cfg.cooldown_ms = 0.0f;
    cfg.y_suppress_enabled = true;
    cfg.y_suppress_min = 0.5f;
    cfg.y_suppress_max = 2.0f;
    uint32_t t = 0;
    float last = 0.0f;
    for (int i = 0; i < 60; ++i) {
        Lead2::Input in;
        in.has_target = true;
        in.target_x = 130.0f;
        in.target_y = 100.0f;
        in.crosshair_x = 100.0f;
        in.crosshair_y = 100.0f;
        in.last_move_y = 5.0f;   // ≥ y_suppress_max ⇒ yScale = 0
        in.now_ms = t;
        t += 10;
        last = l.update(cfg, in);
    }
    check(std::fabs(last) < 1e-6f, "last_move_y ≥ max ⇒ 横向提前量被完全抑制");
}

// ============================ D. HumanizeShaper ============================

void test_humanize_disabled() {
    std::printf("[D1] Humanize 默认关 ⇒ 原样透传\n");
    HumanizeShaper h;
    HumanizeShaperConfig cfg;  // enabled=false
    float x = 3.5f, y = -2.25f;
    HumanizeShaper::Context ctx;
    ctx.dtt = 50.0f;
    ctx.aiming = true;
    h.apply(&x, &y, cfg, ctx);
    check(x == 3.5f && y == -2.25f, "humanize.enabled=false ⇒ 零改动");
}

void test_humanize_overshoot_monotonic() {
    std::printf("[D2] 过冲：离目标越远放大越多（封顶 1+overshoot）\n");
    HumanizeShaperConfig cfg;
    cfg.enabled = true;
    cfg.smooth_factor = 0.0f;
    cfg.overshoot = 0.5f;
    cfg.brake_distance = 0.0f;
    cfg.noise_sigma = 0.0f;
    auto run = [&](float dtt) {
        HumanizeShaper h;
        float x = 10.0f, y = 10.0f;
        HumanizeShaper::Context ctx;
        ctx.dtt = dtt;
        ctx.aiming = true;
        h.apply(&x, &y, cfg, ctx);
        return x;
    };
    const float a = run(10.0f);    // dtt<=10 ⇒ 不放大
    const float b = run(100.0f);   // 中间
    const float c = run(200.0f);   // 封顶 1.5
    check(std::fabs(a - 10.0f) < 1e-4f, "dtt<=10 不过冲");
    check(b > a && c > b, "过冲随 dtt 单调增");
    check(std::fabs(c - 15.0f) < 1e-3f, "dtt>=200 ⇒ 封顶 1+overshoot");
}

void test_humanize_brake() {
    std::printf("[D3] 制动：越近压得越狠（dtt→0 时 factor→0）\n");
    HumanizeShaperConfig cfg;
    cfg.enabled = true;
    cfg.smooth_factor = 0.0f;
    cfg.overshoot = 0.0f;
    cfg.brake_distance = 50.0f;
    cfg.noise_sigma = 0.0f;
    auto run = [&](float dtt) {
        HumanizeShaper h;
        float x = 10.0f, y = 0.0f;
        HumanizeShaper::Context ctx;
        ctx.dtt = dtt;
        ctx.aiming = true;
        h.apply(&x, &y, cfg, ctx);
        return x;
    };
    check(std::fabs(run(0.0f)) < 1e-6f, "dtt=0 ⇒ 完全制动");
    check(std::fabs(run(50.0f) - 10.0f) < 1e-4f, "dtt=brake_distance ⇒ 不制动");
    const float mid = run(12.5f);
    check(mid > 0.0f && mid < 10.0f, "制动区间内 0<factor<1");
}

void test_humanize_noise_bounded() {
    std::printf("[D4] 高斯噪声有界（不出现 inf/nan）\n");
    HumanizeShaper h;
    HumanizeShaperConfig cfg;
    cfg.enabled = true;
    cfg.smooth_factor = 0.0f;
    cfg.overshoot = 0.0f;
    cfg.brake_distance = 0.0f;
    cfg.noise_sigma = 0.2f;
    float max_dev = 0.0f;
    bool finite = true;
    for (int i = 0; i < 5000; ++i) {
        float x = 0.0f, y = 0.0f;
        HumanizeShaper::Context ctx;
        ctx.dtt = 100.0f;
        ctx.aiming = true;
        h.apply(&x, &y, cfg, ctx);
        if (!std::isfinite(x) || !std::isfinite(y)) finite = false;
        const float dev = std::fabs(x);
        if (dev > max_dev) max_dev = dev;
    }
    check(finite, "5000 次采样无 inf/nan（u1 下限保护有效）");
    check(max_dev < 2.0f, "σ=0.2 ⇒ 偏差限制在合理范围");
}

void test_humanize_lowpass() {
    std::printf("[D5] 一阶低通：输出被往历史值拉\n");
    HumanizeShaper h;
    HumanizeShaperConfig cfg;
    cfg.enabled = true;
    cfg.smooth_factor = 0.8f;
    cfg.overshoot = 0.0f;
    cfg.brake_distance = 0.0f;
    cfg.noise_sigma = 0.0f;
    HumanizeShaper::Context ctx;
    ctx.dtt = 50.0f;
    ctx.aiming = true;
    float x = 100.0f, y = 0.0f;
    h.apply(&x, &y, cfg, ctx);   // 首帧：has_last 用当前值 ⇒ x 不变
    check(std::fabs(x - 100.0f) < 1e-4f, "首帧建立历史，值不变");
    float x2 = 0.0f, y2 = 0.0f;
    h.apply(&x2, &y2, cfg, ctx); // 第二帧：0*0.2 + 100*0.8 = 80
    check(std::fabs(x2 - 80.0f) < 1e-3f, "第二帧被历史值拉到 80");
}

// ============================ E. AntiOvershoot ============================

void test_antiover_disabled() {
    std::printf("[E1] 抗过冲默认关 ⇒ 零改动\n");
    AntiOvershoot a;
    AntiOvershootConfig cfg;  // enabled=false
    float x = 10.0f, y = 10.0f;
    a.apply(&x, &y, 5.0f, 1000, cfg);
    check(x == 10.0f && y == 10.0f, "anti_overshoot.enabled=false ⇒ 零改动");
}

void test_antiover_inner_frames() {
    std::printf("[E2] 内圈衰减帧数封顶\n");
    AntiOvershoot a;
    AntiOvershootConfig cfg;
    cfg.enabled = true;
    cfg.inner_distance = 10.0f;
    cfg.inner_strength = 90.0f;
    cfg.inner_frames = 3;
    cfg.outer_distance = 20.0f;
    cfg.outer_strength = 50.0f;
    cfg.outer_frames = 3;
    cfg.reset_cooldown_ms = 500.0f;
    // 前 3 帧（dtt<=inner）每帧 ×0.1
    for (int i = 0; i < 3; ++i) {
        float x = 10.0f, y = 10.0f;
        a.apply(&x, &y, 5.0f, static_cast<uint32_t>(1000 + i * 10), cfg);
        check(std::fabs(x - 1.0f) < 1e-4f, "内圈每帧 ×(1-90%)");
    }
    // ★ BB 的两段判定是「先内圈 return，内圈用满后落进外圈分支」（不是互斥跳过）：
    //   所以内圈帧数用满后、dtt 仍在外圈半径内 ⇒ 改按外圈强度衰减（×0.5）。
    float x = 10.0f, y = 10.0f;
    a.apply(&x, &y, 5.0f, 1040, cfg);
    check(std::fabs(x - 5.0f) < 1e-4f, "内圈帧数用满后落进外圈分支（×(1-50%)）");
    float x2 = 10.0f, y2 = 10.0f;
    a.apply(&x2, &y2, 5.0f, 1050, cfg);   // 外圈第 2 帧
    a.apply(&x2, &y2, 5.0f, 1060, cfg);   // 外圈第 3 帧 → 内外都满 ⇒ cycle_done
    check(a.cycle_done(), "内外圈帧数都用满 ⇒ 本轮标记完成");
    float x3 = 10.0f, y3 = 10.0f;
    a.apply(&x3, &y3, 5.0f, 1070, cfg);
    check(std::fabs(x3 - 10.0f) < 1e-4f, "本轮完成后不再衰减（等越界冷却才复位）");
}

void test_antiover_reset_cooldown() {
    std::printf("[E3] 越界持续超冷却 ⇒ 整轮复位\n");
    AntiOvershoot a;
    AntiOvershootConfig cfg;
    cfg.enabled = true;
    cfg.inner_distance = 10.0f;
    cfg.inner_strength = 90.0f;
    cfg.inner_frames = 99;    // 不封顶，方便观察复位
    cfg.outer_distance = 20.0f;
    cfg.outer_strength = 50.0f;
    cfg.outer_frames = 99;
    cfg.reset_cooldown_ms = 100.0f;
    float x = 10.0f, y = 10.0f;
    a.apply(&x, &y, 5.0f, 1000, cfg);
    check(a.inner_frames() == 1, "先累计 1 帧内圈衰减");
    // 越界（dtt=100 > outer=20）：第 1 帧记起点
    float dummy_x = 1.0f, dummy_y = 1.0f;
    a.apply(&dummy_x, &dummy_y, 100.0f, 2000, cfg);
    check(a.inner_frames() == 1, "刚越界不立即复位");
    // 越界持续 100ms ⇒ 复位
    a.apply(&dummy_x, &dummy_y, 100.0f, 2100, cfg);
    check(a.inner_frames() == 0, "越界持续 ≥ reset_cooldown ⇒ 状态复位");
}

// ============================ F. SpeedAdaptiveKp ============================

void test_speed_kp() {
    std::printf("[F1] 速度自适应 Kp\n");
    SpeedAdaptiveKp s;
    SpeedAdaptiveKpConfig cfg;  // enabled=false
    check(s.multiplier(cfg, true, 0.0f, 0.0f) == 1.0f, "默认关 ⇒ 1.0");

    cfg.enabled = true;
    cfg.frames = 5;
    cfg.threshold = 3.0f;
    cfg.move_mult = 1.5f;
    cfg.static_mult = 0.8f;
    // 静止目标：位置不变
    float m = 1.0f;
    for (int i = 0; i < 8; ++i) m = s.multiplier(cfg, true, 100.0f, 100.0f);
    check(std::fabs(m - cfg.static_mult) < 1e-6f, "静止目标 ⇒ static_mult");

    SpeedAdaptiveKp s2;
    float m2 = 1.0f;
    for (int i = 0; i < 8; ++i) m2 = s2.multiplier(cfg, true, static_cast<float>(i) * 20.0f, 0.0f);
    check(std::fabs(m2 - cfg.move_mult) < 1e-6f, "移动目标 ⇒ move_mult");

    SpeedAdaptiveKp s3;
    check(s3.multiplier(cfg, false, 0.0f, 0.0f) == 1.0f, "无目标 ⇒ 不干预");
}

// ============================ G. GlobalWave ============================

void test_global_wave() {
    std::printf("[G1] 全局正弦扰动\n");
    GlobalWave g;
    GlobalWaveConfig cfg;  // enabled=false
    float x = 5.0f, y = -5.0f;
    g.apply(&x, &y, 1234, cfg);
    check(x == 5.0f && y == -5.0f, "默认关 ⇒ 零改动");

    GlobalWave g2;
    cfg.enabled = true;
    cfg.amp_x = 0.10f;
    cfg.amp_y = 0.10f;
    cfg.freq = 1.0f;
    cfg.smooth = 0.5f;
    float max_extra = 0.0f;
    bool finite = true;
    for (int i = 0; i < 2000; ++i) {
        float px = 0.0f, py = 0.0f;
        g2.apply(&px, &py, static_cast<uint32_t>(i * 10), cfg);
        if (!std::isfinite(px) || !std::isfinite(py)) finite = false;
        const float e = std::fabs(px);
        if (e > max_extra) max_extra = e;
    }
    check(finite, "长跑无 inf/nan");
    check(max_extra <= cfg.amp_x + 1e-4f, "扰动幅度不超过 amp_x");
}

// ============================ H. RuntimeProfile 往返 ============================

void test_profile_roundtrip() {
    std::printf("[H1] RuntimeProfile 新键序列化/解析往返\n");
    ttbox::core::RuntimeProfile p;
    p.mouse.lead2.enabled = true;
    p.mouse.lead2.gain = 0.075f;
    p.mouse.lead2.decay = 0.9f;
    p.mouse.humanize.enabled = true;
    p.mouse.humanize.smooth_factor = 0.35f;
    p.mouse.humanize.noise_sigma = 0.33f;
    // BB 927 原版：accuracy_sim 是独立段（不归 humanize.enabled 管）
    p.mouse.accuracy_sim.enabled = true;
    p.mouse.accuracy_sim.direction = 2;
    p.mouse.accuracy_sim.perfect_rate = 77.0f;
    p.mouse.speed_fluctuation.enabled = true;
    p.mouse.speed_fluctuation.start_speed = 0.65f;
    p.mouse.anti_overshoot.enabled = true;
    p.mouse.anti_overshoot.inner_frames = 4;
    p.mouse.speed_adaptive_kp.enabled = true;
    p.mouse.speed_adaptive_kp.move_mult = 1.8f;
    p.mouse.global_wave.enabled = true;
    p.mouse.global_wave.freq = 1.7f;
    // 选靶四项机制（2026-09-24 补的配置通路）
    p.mouse.lock_hold_ms = 1500.0f;
    p.mouse.priority_scoring = true;
    p.mouse.weight_dist = 1.2f;
    p.mouse.weight_size = 0.4f;
    p.mouse.stickiness = 0.8f;
    p.mouse.switch_threshold_px = 75.0f;
    p.mouse.head_body_stable = true;
    p.mouse.hb_body1 = 2;
    p.mouse.hb_head1 = 3;
    p.mouse.hb_body2 = 4;
    p.mouse.hb_head2 = 5;

    const auto json = p.to_json();
    const auto q = ttbox::core::RuntimeProfile::from_json(json);

    check(q.mouse.lead2.enabled && std::fabs(q.mouse.lead2.gain - 0.075f) < 1e-5f &&
              std::fabs(q.mouse.lead2.decay - 0.9f) < 1e-5f,
          "lead2 键往返一致");
    check(q.mouse.humanize.enabled && std::fabs(q.mouse.humanize.smooth_factor - 0.35f) < 1e-4f &&
              std::fabs(q.mouse.humanize.noise_sigma - 0.33f) < 1e-4f,
          "humanize 键往返一致");
    // ★ 独立段必须能独立落盘：关掉 humanize.enabled 也照样生效（BB 原版口径）
    check(q.mouse.accuracy_sim.enabled && q.mouse.accuracy_sim.direction == 2 &&
              std::fabs(q.mouse.accuracy_sim.perfect_rate - 77.0f) < 1e-4f,
          "accuracy_sim 独立段往返一致");
    check(q.mouse.speed_fluctuation.enabled &&
              std::fabs(q.mouse.speed_fluctuation.start_speed - 0.65f) < 1e-4f,
          "speed_fluctuation 独立段往返一致");
    check(q.mouse.anti_overshoot.enabled && q.mouse.anti_overshoot.inner_frames == 4,
          "anti_overshoot 键往返一致");
    check(q.mouse.speed_adaptive_kp.enabled &&
              std::fabs(q.mouse.speed_adaptive_kp.move_mult - 1.8f) < 1e-4f,
          "speed_adaptive_kp 键往返一致");
    check(q.mouse.global_wave.enabled && std::fabs(q.mouse.global_wave.freq - 1.7f) < 1e-4f,
          "global_wave 键往返一致");
    check(std::fabs(q.mouse.lock_hold_ms - 1500.0f) < 1e-3f && q.mouse.priority_scoring &&
              std::fabs(q.mouse.weight_dist - 1.2f) < 1e-4f &&
              std::fabs(q.mouse.weight_size - 0.4f) < 1e-4f &&
              std::fabs(q.mouse.stickiness - 0.8f) < 1e-4f &&
              std::fabs(q.mouse.switch_threshold_px - 75.0f) < 1e-4f,
          "选靶四项（锁定期/打分制/三项权重）往返一致");
    check(q.mouse.head_body_stable && q.mouse.hb_body1 == 2 && q.mouse.hb_head1 == 3 &&
              q.mouse.hb_body2 == 4 && q.mouse.hb_head2 == 5,
          "选靶头身稳定过滤（两组组合）往返一致");

    std::string err;
    check(q.validate(&err), "往返后的配置通过校验");
}

void test_profile_defaults_zero_behavior() {
    std::printf("[H2] 默认配置：第二批全部关闭 + 校验通过\n");
    ttbox::core::RuntimeProfile p;
    std::string err;
    check(p.validate(&err), "RuntimeProfile 默认值校验通过");
    check(!p.mouse.lead2.enabled &&
              !p.mouse.humanize.enabled && !p.mouse.anti_overshoot.enabled &&
              !p.mouse.speed_adaptive_kp.enabled && !p.mouse.global_wave.enabled,
          "第二批模块默认全关（输出链逐字节不变的前提）");
    check(p.mouse.lock_hold_ms == 0.0f && !p.mouse.priority_scoring &&
              !p.mouse.head_body_stable,
          "选靶四项默认全关（选靶行为逐字节不变的前提）");
    check(p.mouse.hb_body1 == 0 && p.mouse.hb_head1 == 1 &&
              p.mouse.hb_body2 == -1 && p.mouse.hb_head2 == -1,
          "头身稳定过滤默认组合回落到 bb-port/01 的标定值");

    // 反例：选靶四项的负数被拒（负锁定期/负权重的语义不成立）
    ttbox::core::RuntimeProfile bad_s;
    bad_s.mouse.lock_hold_ms = -1.0f;
    check(!bad_s.validate(&err), "lock_hold_ms=-1 ⇒ 校验拒绝");
    ttbox::core::RuntimeProfile bad_s2;
    bad_s2.mouse.weight_size = -0.5f;
    check(!bad_s2.validate(&err), "选靶评分权重为负 ⇒ 校验拒绝");

    // 反例：越界值被拒绝
    ttbox::core::RuntimeProfile bad;
    bad.mouse.global_wave.smooth = 1.5f;
    check(!bad.validate(&err), "global_wave.smooth=1.5 ⇒ 校验拒绝");
    ttbox::core::RuntimeProfile bad2;
    bad2.mouse.lead2.gain = -0.1f;
    check(!bad2.validate(&err), "lead2.gain<0 ⇒ 校验拒绝");
    ttbox::core::RuntimeProfile bad3;
    bad3.mouse.anti_overshoot.inner_strength = 150.0f;
    check(!bad3.validate(&err), "anti_overshoot.inner_strength=150 ⇒ 校验拒绝");
}

}  // namespace

// ===========================================================================
// BB 927 原版照搬的两个独立模块（2026-09-28）
// 口径全部对齐 main.lua：applySpeedFluctuation(:5265) / applyAccuracySim(:5274)
// ===========================================================================

// 速度波动：起步慢（sf<1）→ 中段匀速（sf=1）→ 收尾减速（sf<1）
void test_speed_fluctuation_shape() {
    SpeedFluctuationConfig cfg;
    cfg.enabled = true;
    cfg.start_speed = 0.80f;
    cfg.accel_ratio = 0.20f;
    cfg.decel_ratio = 0.20f;
    cfg.intensity = 0.0f;          // 关随机，先看骨架
    cfg.total_distance_px = 100.0f;
    SpeedFluctuation sf;
    sf.reset();

    // dtt=95 ⇒ p=0.05 < 0.20 ⇒ 起步段：sf = 0.8 + 0.2×(0.05/0.20) = 0.85
    {
        float mx = 10.0f, my = 10.0f;
        sf.apply(&mx, &my, 95.0f, true, cfg);
        check(std::fabs(mx - 8.5f) < 1e-3f, "起步段：位移被压到 0.85 倍");
    }
    // dtt=50 ⇒ p=0.5 ⇒ 中段：sf = 1.0
    {
        float mx = 10.0f, my = 10.0f;
        sf.apply(&mx, &my, 50.0f, true, cfg);
        check(std::fabs(mx - 10.0f) < 1e-3f, "中段：位移不变（倍率 1.0）");
    }
    // dtt=5 ⇒ p=0.95 > 0.80 ⇒ 收尾段：sf = 1 - 0.2×((0.95-0.8)/0.2) = 0.85
    {
        float mx = 10.0f, my = 10.0f;
        sf.apply(&mx, &my, 5.0f, true, cfg);
        check(std::fabs(mx - 8.5f) < 1e-3f, "收尾段：位移被压到 0.85 倍");
    }
}

// 原版口径：只在"刚锁定目标的第一帧"生效一次（first_lock=false ⇒ 完全不动）
void test_speed_fluctuation_first_lock_only() {
    SpeedFluctuationConfig cfg;
    cfg.enabled = true;
    cfg.intensity = 0.0f;
    cfg.total_distance_px = 100.0f;
    SpeedFluctuation sf;
    sf.reset();
    float mx = 10.0f, my = 10.0f;
    sf.apply(&mx, &my, 95.0f, false, cfg);
    check(mx == 10.0f && my == 10.0f, "非首帧：原版口径下完全不作用");

    // 默认关 ⇒ 哪怕首帧也不动
    SpeedFluctuationConfig off;
    float ox = 10.0f, oy = 10.0f;
    sf.apply(&ox, &oy, 95.0f, true, off);
    check(ox == 10.0f && oy == 10.0f, "默认关 ⇒ 零变化");
}

// 命中率随机：完美命中概率内不动；必偏时偏移量 = 框半径 × 强度
void test_accuracy_sim() {
    AccuracySimConfig cfg;
    cfg.enabled = true;
    cfg.perfect_rate = 100.0f;      // 永远完美 ⇒ 永不偏移
    cfg.offset_strength = 0.5f;
    AccuracySim as;
    as.reset();
    {
        float tx = 100.0f, ty = 100.0f;
        for (int i = 0; i < 50; ++i) as.apply(&tx, &ty, 60.0f, 80.0f, cfg);
        check(tx == 100.0f && ty == 100.0f, "完美命中概率 100% ⇒ 瞄准点不动");
    }
    cfg.perfect_rate = 0.0f;        // 必偏
    cfg.direction = 0;              // 四角优先
    {
        bool moved = false;
        bool in_box = true;
        for (int i = 0; i < 200; ++i) {
            float tx = 100.0f, ty = 100.0f;
            as.apply(&tx, &ty, 60.0f, 80.0f, cfg);
            const float dx = tx - 100.0f, dy = ty - 100.0f;
            if (dx != 0.0f || dy != 0.0f) moved = true;
            // 偏移上限 = 半宽×强度 = 15（X）/ 半高×强度 = 20（Y）
            if (std::fabs(dx) > 15.0f + 1e-3f || std::fabs(dy) > 20.0f + 1e-3f) in_box = false;
        }
        check(moved, "完美命中概率 0% ⇒ 每帧都偏");
        check(in_box, "偏移量不超过 框半径 x 强度");
    }
    // 默认关 ⇒ 不动
    {
        AccuracySimConfig off;
        float tx = 7.0f, ty = 7.0f;
        as.apply(&tx, &ty, 60.0f, 80.0f, off);
        check(tx == 7.0f && ty == 7.0f, "默认关 ⇒ 零变化");
    }
}

int main() {
    std::printf("=== test_bb_second_batch：BB 对标第二批模块测试 ===\n");

    test_lead2_disabled_zero();
    test_lead2_cooldown_then_integrate();
    test_lead2_deadzone_decay();
    test_lead2_y_suppress();

    test_humanize_disabled();
    test_humanize_overshoot_monotonic();
    test_humanize_brake();
    test_humanize_noise_bounded();
    test_humanize_lowpass();

    test_antiover_disabled();
    test_antiover_inner_frames();
    test_antiover_reset_cooldown();

    test_speed_kp();
    test_global_wave();

    test_speed_fluctuation_shape();
    test_speed_fluctuation_first_lock_only();
    test_accuracy_sim();

    test_profile_roundtrip();
    test_profile_defaults_zero_behavior();

    std::printf("结果: %d failures\n", failures);
    return failures == 0 ? 0 : 1;
}
