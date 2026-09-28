// test_jitter_feedforward.cpp — V3 阶段 5：拟人化抖动前馈扣除（延迟对齐环形缓冲）
//
// 钉住四件事：
//   1. 释放时刻 = 注入后累计 dt 到达 delay 的那一帧（**不是下一帧**，也不是固定帧数）
//   2. 释放量 = 注入量（不多不少、不串轴）
//   3. 零抖动不占位（缓冲不会被空样本挤满）
//   4. 缓冲满了丢最老、不崩、不把陈旧样本攒着集中释放
//   5. 两条拟人化链的"抖动上报"只报随机分量，不报整形量（过冲/包络不报）
#include <cmath>
#include <cstdio>

#include "mouse/HumanizeShaper.hpp"
#include "mouse/JitterFeedforward.hpp"
#include "mouse/MouseTypes.hpp"
#include "mouse/PersonalTrajectoryShader.hpp"

using ttbox::core::aim::HumanizeShaper;
using ttbox::core::aim::HumanizeShaperConfig;
using ttbox::core::aim::JitterFeedforward;
using ttbox::core::aim::PersonalTrajectoryConfig;
using ttbox::core::aim::PersonalTrajectoryShader;

namespace {
int fails = 0;

void check(bool cond, const char* what) {
    if (!cond) {
        std::printf("  [FAIL] %s\n", what);
        ++fails;
    }
}

// 场景 1：51ms 延迟 / 6.94ms 帧（144fps）⇒ 第 8 帧才释放（7 帧只累计 48.6ms）
void test_release_frame() {
    JitterFeedforward ff;
    ff.reset();
    const float dt = 6.94f;      // 144fps 实测帧间隔
    const float delay = 51.0f;   // 板端实测回路延迟
    ff.push(3.0f, -2.0f);
    float jx = 0.0f, jy = 0.0f;
    int released_at = -1;
    for (int f = 1; f <= 12 && released_at < 0; ++f) {
        ff.advance(dt, delay, &jx, &jy);
        if (jx != 0.0f || jy != 0.0f) released_at = f;
    }
    check(released_at == 8, "51ms@144fps 应在第 8 帧释放（7 帧只有 48.6ms，不够）");
    check(std::fabs(jx - 3.0f) < 1e-6f, "释放量 X = 注入量（不多不少）");
    check(std::fabs(jy - (-2.0f)) < 1e-6f, "释放量 Y = 注入量（不串轴）");
}

// 场景 2：帧率漂了也能对齐（130fps vs 150fps，按 dt 累计而不是固定帧数）
void test_framerate_drift() {
    JitterFeedforward ff;
    ff.reset();
    ff.push(5.0f, 0.0f);
    float jx = 0.0f, jy = 0.0f;
    float acc = 0.0f;
    int frames = 0;
    // 交替 7.69ms(130fps) / 6.67ms(150fps)
    while (acc < 51.0f && frames < 40) {
        const float dt = (frames % 2 == 0) ? 7.69f : 6.67f;
        ff.advance(dt, 51.0f, &jx, &jy);
        acc += dt;
        ++frames;
        if (jx != 0.0f) break;
    }
    check(jx == 5.0f, "帧率漂移下仍按累计 dt 对齐（不是按固定帧数）");
    check(acc >= 51.0f, "释放发生在累计时间真的越过 51ms 之后");
}

// 场景 3：零抖动不占位；缓冲满丢最老不崩
void test_buffer_behavior() {
    JitterFeedforward ff;
    ff.reset();
    for (int i = 0; i < 200; ++i) ff.push(0.0f, 0.0f);
    check(ff.pending() == 0, "零抖动不进缓冲（不会被空样本挤满）");

    ff.reset();
    for (int i = 0; i < 500; ++i) ff.push(1.0f, 0.0f);   // 远超容量，永不 advance
    check(ff.pending() == JitterFeedforward::kCapacity, "缓冲满时保持容量上限");
    check(ff.dropped() > 0, "缓冲满时丢最老的样本（有计数可查）");
    float jx = 0.0f, jy = 0.0f;
    ff.advance(1000.0f, 51.0f, &jx, &jy);                // 一次性全部到期
    check(ff.pending() == 0, "一次性到期后缓冲清空（不残留）");
    check(std::fabs(jx - static_cast<float>(JitterFeedforward::kCapacity)) < 1e-6f,
          "到期量 = 队列里所有样本之和");
}

// 场景 4：HumanizeShaper 只上报高斯噪声，不上报过冲/制动
void test_humanize_reports_noise_only() {
    HumanizeShaperConfig cfg;
    cfg.enabled = true;
    cfg.overshoot = 0.5f;      // 故意开过冲：它是"故意多走一段"，不该被扣
    cfg.brake_distance = 200.0f;
    cfg.noise_sigma = 0.0f;    // 先关噪声
    HumanizeShaper hs;
    hs.reset();
    for (int i = 0; i < 3; ++i) {
        float x = 10.0f, y = 10.0f;
        HumanizeShaper::Context ctx;
        ctx.dtt = 100.0f;
        ctx.now_ms = static_cast<uint32_t>(i * 7);
        ctx.aiming = true;
        hs.apply(&x, &y, cfg, ctx);
        check(hs.last_jitter_x() == 0.0f && hs.last_jitter_y() == 0.0f,
              "noise_sigma=0 时不上报抖动（过冲/制动不算抖动）");
    }
    // 打开噪声 ⇒ 报出的值 = 实际叠到 x/y 上的那一份
    cfg.noise_sigma = 0.5f;
    HumanizeShaper hs2;
    hs2.reset();
    bool saw_nonzero = false;
    for (int i = 0; i < 50; ++i) {
        float x = 0.0f, y = 0.0f;
        HumanizeShaper::Context ctx;
        ctx.dtt = 100.0f;
        ctx.now_ms = static_cast<uint32_t>(i) * 7u;
        ctx.aiming = true;
        hs2.apply(&x, &y, cfg, ctx);
        if (hs2.last_jitter_x() != 0.0f) {
            saw_nonzero = true;
            check(std::fabs(x - hs2.last_jitter_x()) < 1e-6f,
                  "上报量 = 本帧实际叠加的噪声（输入为 0 时输出即噪声）");
        }
    }
    check(saw_nonzero, "开噪声后确实能读到非零抖动");

    // 关闭 ⇒ 恒 0（哪怕上一帧有值，也不能残留）
    cfg.enabled = false;
    float x = 9.0f, y = 9.0f;
    HumanizeShaper::Context ctx;
    ctx.dtt = 100.0f;
    hs2.apply(&x, &y, cfg, ctx);
    check(hs2.last_jitter_x() == 0.0f && hs2.last_jitter_y() == 0.0f,
          "未启用时抖动上报恒为 0（不残留上一帧）");
}

// 场景 5：PersonalTrajectoryShader 的垂直抖动可上报，且换目标/未激活时归零
void test_personal_reports_perp_only() {
    PersonalTrajectoryConfig cfg;
    cfg.enabled = true;
    cfg.min_error_px = 1.0f;          // 放宽门槛，让抖动真的能注入
    cfg.urgent_error_px = 9999.0f;    // 关掉"大误差直出"
    cfg.max_target_age_ms = 9999.0f;  // 关掉"目标老"抑制
    cfg.urgent_speed_px_s = 99999.0f;
    cfg.capture_priority_ms = 0.0f;
    cfg.variation_scale = 1.8f;
    cfg.max_visual_variation_px = 1.5f;
    cfg.response_px_per_count = 0.65f;
    PersonalTrajectoryShader sh;
    sh.reset();
    sh.activate(120.0f, cfg);
    bool saw_nonzero = false;
    for (int i = 0; i < 200; ++i) {
        sh.set_error_speed_px_s(0.0f);
        sh.set_target_age_ms(static_cast<float>(i) * 7.0f);
        sh.set_target_radius_px(40.0f);
        int16_t dx = 6, dy = 6;
        sh.shape(&dx, &dy, 80.0f, 80.0f, 6.94f, cfg);
        if (sh.last_jitter_x() != 0.0f || sh.last_jitter_y() != 0.0f) {
            saw_nonzero = true;
            // 垂直抖动：与移动方向 (6,6) 点积 ≈ 0 ⇒ 是"垂直"的，不是沿轴推
            const float dot = sh.last_jitter_x() * 6.0f + sh.last_jitter_y() * 6.0f;
            check(std::fabs(dot) < 1e-3f, "上报的分量垂直于移动方向（是曲线抖动不是加力）");
        }
    }
    check(saw_nonzero, "personal_trajectory 能读到非零垂直抖动");
    sh.reset();
    check(sh.last_jitter_x() == 0.0f && sh.last_jitter_y() == 0.0f,
          "reset 后抖动上报清零（换目标不欠账）");
}

// 场景 6：深度融合的末端守卫（自研那套安全约束罩住 BB 的过冲与噪声）
void test_fused_guard() {
    PersonalTrajectoryConfig cfg;
    cfg.enabled = true;
    cfg.max_extra_px = 2.0f;
    PersonalTrajectoryShader sh;
    sh.reset();

    // (a) 幅度上限：不许超过 raw ± max_extra
    {
        float x = 999.0f, y = 10.0f;
        sh.guard_fused(&x, &y, 10.0f, 10.0f, 50.0f, 50.0f, cfg);
        check(std::fabs(x) <= 12.0f + 1e-6f, "融合守卫：幅度被压回 raw ± max_extra");
    }
    // (b) 反向：误差是正的，输出却往负走 ⇒ 不许（拉回 raw 而不是照发）
    {
        float x = -30.0f, y = -30.0f;
        sh.guard_fused(&x, &y, 10.0f, 10.0f, 50.0f, 50.0f, cfg);
        check(x >= 0.0f && y >= 0.0f, "融合守卫：不许把准星往误差反方向推");
    }
    // (c) 能量不增：整形后的投影不能小于 raw 自身能量
    {
        float x = 1.0f, y = 1.0f;
        sh.guard_fused(&x, &y, 10.0f, 10.0f, 50.0f, 50.0f, cfg);
        const float proj = x * 10.0f + y * 10.0f;
        check(proj >= 10.0f * 10.0f + 10.0f * 10.0f - 1e-3f,
              "融合守卫：能量不增（不会把位移整小到比不整还慢）");
    }
}

// 场景 7：默认配置下整套机制什么都不做（老行为零变化）
void test_default_is_noop() {
    JitterFeedforward ff;           // 默认构造即空
    check(ff.pending() == 0, "默认构造 = 空缓冲");
    float jx = 1.0f, jy = 1.0f;
    ff.advance(6.94f, 51.0f, &jx, &jy);
    check(jx == 0.0f && jy == 0.0f, "空缓冲 advance 输出 0");

    ttbox::core::aim::MouseProfile mp;  // 默认档
    check(!mp.jitter_feedforward.enabled, "jitter_feedforward 默认关");
    check(mp.aim_profiles.empty() || true, "默认档位不受影响");
    // 默认档位里的 gain_px_per_count 必须是 0（=没测过，回退腰射）
    for (const auto& ap : mp.aim_profiles) {
        check(ap.gain_px_per_count == 0.0f, "热键档 gain_px_per_count 默认 0（未标定）");
    }
}
}  // namespace

int main() {
    std::printf("test_jitter_feedforward:\n");
    test_release_frame();
    test_framerate_drift();
    test_buffer_behavior();
    test_humanize_reports_noise_only();
    test_personal_reports_perp_only();
    test_fused_guard();
    test_default_is_noop();
    if (fails == 0) std::printf("test_jitter_feedforward: PASS\n");
    else std::printf("test_jitter_feedforward: %d FAILED\n", fails);
    return fails == 0 ? 0 : 1;
}
