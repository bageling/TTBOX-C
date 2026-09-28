// HumanizeShaper.hpp — BB 拟人化整形链（输出后滤波），BB 对标移植（2026-09-24）
//
// 小白理解：
//   纯 PID 的输出是"每帧等量"的，机器味很重。拟人化就是在真正把位移发出去之前
//   再过一道"人手滤镜"：起手慢一点、中途冲过头一点、快到目标时收力、
//   再叠一点点手抖噪声。
//
// 固定顺序（对齐 bb-port/03 号 §3.1，顺序不能换）：
//   ① 一阶低通（smooth_factor）
//   ② 反应延迟（随机 [base-rand, base+rand] 内完全不动）
//   ③ 过冲   factor = 1 + overshoot × min(1, dtt/200)      —— 越远冲得越多
//   ④ 制动   factor = (dtt / brake_distance)^0.5           —— 越近压得越狠
//   ⑤ 高斯噪声（Box-Muller，各轴独立）
//
// ★ 与 BB 的差异（有意为之，写清楚免得以后被当 bug 改回去）：
//   1. BB 的一阶低通是「无开关、aim_smooth_factor>0 就永久生效」（默认 0.35）。
//      本实现把它放进 enabled 门内、默认值给 0.0f，保证"默认行为零变化"这条底线。
//   2. BB 的 Box-Muller 直接用 math.random() 当 u1，可能抽到 0 ⇒ ln(0) = -inf
//      把后续若干帧污染成 inf/nan。本实现给 u1 加下限保护（1e-6）。
//   3. BB 的 human_rest_*（随机休息）是**死功能**，全仓无人读取，按文档要求不实现。
//
// ★ 默认 enabled=false ⇒ 不跑即零输出，输出链与本模块加入前逐字节一致。
#pragma once

#include <cmath>
#include <cstdint>

#include "mouse/MouseTypes.hpp"

namespace ttbox::core::aim {

class HumanizeShaper {
public:
    struct Context {
        float dtt = 0.0f;        // 准星到目标距离（px）
        uint32_t now_ms = 0;
        bool aiming = true;      // 是否处于瞄准中（非瞄准时跳过延迟/过冲/制动）
    };

    // 就地滤波。x/y 单位是 count（量化前），传入传出同一量纲。
    void apply(float* x, float* y, const HumanizeShaperConfig& cfg, const Context& ctx) {
        // ★ 每帧先清零：本帧没注入抖动就必须是 0（前馈读数不能沿用上一帧的残值）。
        last_jitter_x_ = 0.0f;
        last_jitter_y_ = 0.0f;
        if (!cfg.enabled) return;

        // ① 一阶低通
        const float s = (cfg.smooth_factor > 0.99f) ? 0.99f : cfg.smooth_factor;
        if (s > 0.0f) {
            if (!has_last_) {
                last_x_ = *x;
                last_y_ = *y;
                has_last_ = true;
            }
            *x = *x * (1.0f - s) + last_x_ * s;
            *y = *y * (1.0f - s) + last_y_ * s;
            last_x_ = *x;
            last_y_ = *y;
        }

        // ② 反应延迟随机化（仅瞄准中）
        if (ctx.aiming && (cfg.delay_ms > 0.0f || cfg.delay_random_ms > 0.0f)) {
            if (!has_aim_time_) {
                has_aim_time_ = true;
                last_aim_ms_ = ctx.now_ms;
                // ★ 2026-09-26：抽样区间应是 [delay−rand, delay+rand]，
                //   旧实现 lo + rand*(lo+hi) ⇒ 上界实际是 2×delay（delay=100,rand=50
                //   时抽到 [50,250] 而非 [50,150]），均值也整体偏大 rand/2。
                const float lo = cfg.delay_ms - cfg.delay_random_ms;
                const float hi = cfg.delay_ms + cfg.delay_random_ms;
                if (lo > 0.0f) {
                    delay_cached_ = lo + rand_unit() * (hi - lo);
                } else {
                    delay_cached_ = rand_unit() * hi;
                }
            }
            const float aim_timer = static_cast<float>(ctx.now_ms - last_aim_ms_);
            if (aim_timer < delay_cached_) {
                *x = 0.0f;
                *y = 0.0f;
                return;
            }
        } else if (!ctx.aiming) {
            has_aim_time_ = false;
            delay_cached_ = 0.0f;
        }

        // ③ 过冲
        if (cfg.overshoot > 0.0f && ctx.aiming && ctx.dtt > 10.0f) {
            const float ratio = (ctx.dtt < 200.0f) ? (ctx.dtt / 200.0f) : 1.0f;
            const float f = 1.0f + cfg.overshoot * ratio;
            *x *= f;
            *y *= f;
        }

        // ④ 制动
        if (cfg.brake_distance > 0.0f && ctx.aiming && ctx.dtt < cfg.brake_distance) {
            const float b = std::sqrt(ctx.dtt / cfg.brake_distance);
            *x *= b;
            *y *= b;
        }

        // ⑤ 高斯噪声（各轴独立）
        // ★ V3 阶段 5：噪声是"随机抖动"，要报出去给前馈扣除（速度包络/制动不报，
        //   那是故意要走的一段位移，扣掉会让 PID 以为没到 ⇒ 过冲）。
        if (cfg.noise_sigma > 0.0f) {
            const float nx = gauss() * cfg.noise_sigma;
            const float ny = gauss() * cfg.noise_sigma;
            *x += nx;
            *y += ny;
            last_jitter_x_ = nx;
            last_jitter_y_ = ny;
        }
    }

    // V3 阶段 5：本帧注入的**随机抖动**分量（与 x/y 同量纲 = count）。
    // 未启用 / 被反应延迟整帧压掉 / noise_sigma=0 ⇒ 恒为 0。
    float last_jitter_x() const { return last_jitter_x_; }
    float last_jitter_y() const { return last_jitter_y_; }

    // ★ 2026-09-28：speed_fluctuation / accuracy_sim 已从本类移出，按 BB 927 原版
    //   口径独立成模块（SpeedFluctuation.hpp / AccuracySim.hpp），且**不受
    //   humanize.enabled 管**（原版 main.lua:5943 / :6445 都是独立判断）。
    //   此前它们挂在本配置里且从未被调用 ⇒ 面板上那 9 个格子全是假开关。

    void reset() {
        has_last_ = false;
        last_x_ = 0.0f;
        last_y_ = 0.0f;
        last_jitter_x_ = 0.0f;
        last_jitter_y_ = 0.0f;
        has_aim_time_ = false;
        last_aim_ms_ = 0;
        delay_cached_ = 0.0f;
    }

private:
    // xorshift32：[0,1) 均匀分布。自带种子 ⇒ 不依赖全局 rand，行为可复现。
    float rand_unit() {
        rng_ ^= rng_ << 13;
        rng_ ^= rng_ >> 17;
        rng_ ^= rng_ << 5;
        return static_cast<float>(rng_ & 0xFFFFFFu) / 16777216.0f;
    }

    // Box-Muller 标准正态。★ u1 加下限保护：避免 ln(0) = -inf 污染后续帧。
    float gauss() {
        float u1 = rand_unit();
        if (u1 < 1e-6f) u1 = 1e-6f;
        const float u2 = rand_unit();
        return std::sqrt(-2.0f * std::log(u1)) * std::cos(6.2831853f * u2);
    }

    bool has_last_ = false;
    float last_x_ = 0.0f;
    float last_y_ = 0.0f;
    // V3 阶段 5：本帧注入的随机抖动（count），供前馈扣除读取。
    float last_jitter_x_ = 0.0f;
    float last_jitter_y_ = 0.0f;
    bool has_aim_time_ = false;
    uint32_t last_aim_ms_ = 0;
    float delay_cached_ = 0.0f;
    uint32_t rng_ = 0x2545F491u;
};

}  // namespace ttbox::core::aim
