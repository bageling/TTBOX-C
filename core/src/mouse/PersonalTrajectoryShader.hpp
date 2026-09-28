// PersonalTrajectoryShader.hpp — 拟人化整形引擎（第 1 项落地）
//
// 作用：把 AimThread 输出链 move_x/move_y（int16 HID count）整形为"接近真人手部动作"的
//       移动轨迹，替代原来"恒定 PID 输出"。核心四件套：
//         1. Fitts 时长模型：目标距离 → 单次移动期望时长
//         2. 速度包络：transport 阶段加速→减速（偏峰铃形 + 可选 16 点经验包络）
//         3. 垂直向 AR(1) 随机游走抖动：模拟人手曲线（非恒定正弦）
//         4. 自适应抑制 + 安全守卫：大误差/目标快/目标老/方向突变自动降强度或停用，
//            幅度不超过 raw+max_extra，保持与误差同向，禁止反向投影
//
// 设计要点（与 TTBOX 输出链完全兼容）：
//   - 状态维护在实例内（时长、进度、抖动游走、方向、余数），线程安全（AimThread 单线程调用）
//   - 未启用 / 未激活 / 零移动 → 原样放行，不产生副作用
//   - 输入 dx,dy = 已量化的 HID count；ex,ey = 控制误差（px）；dt_ms = 帧间隔
//   - 安全：输出永远不"凭空产生",只会在 raw 基础上附加不超过 max_extra 的整形量,
//            且热键 Gate 最终仍能把输出归零（Gate 在 AimThread 层,不在此类内）
//   - 区分于 PersonalMotion（倍率曲线）：本引擎是完整移动轨迹整形
#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>

#include "mouse/MouseTypes.hpp"

namespace ttbox::core::aim {

namespace {
constexpr float kPi = 3.14159265358979323846f;
}

class PersonalTrajectoryShader {
public:
    // 激活一次移动（目标切换/新捕获时调用）。total_distance_px = 当前误差距离。
    // 按 Fitts 定律 + 用户速度系数计算本次移动时长，重置进度/抖动游走。
    void activate(float total_distance_px, const PersonalTrajectoryConfig& cfg) {
        const float dist = std::max(1.0f, total_distance_px);
        // Fitts 定律：index = log2(dist/width + 1)，width = 2×min_error
        const float width = std::max(4.0f, cfg.min_error_px * 2.0f);
        const float index = std::log2(dist / width + 1.0f);
        float predicted = cfg.fitts_intercept_ms + cfg.fitts_slope_ms_per_bit * index;
        // 压缩到运行时范围（95~220ms 之间，受速度系数缩放）
        predicted = clamp_(_runtime_duration_ms(predicted, cfg), 70.0f, 220.0f);
        duration_ms_ = predicted;
        start_distance_px_ = dist;
        elapsed_ms_ = 0.0f;
        active_ = true;
        curve_state_ = 0.0f;
        curve_sign_ = (rng_01_() < 0.5f) ? -1.0f : 1.0f;
        gain_round_x_ = 0.0f;
        gain_round_y_ = 0.0f;
        last_raw_x_ = 0;
        last_raw_y_ = 0;
        goal_progress_ = 0.0f;
        has_last_error_ = false;
        last_error_x_ = 0.0f;
        last_error_y_ = 0.0f;
    }

    // 整形输出：dx,dy 为已量化 HID count；ex,ey 为控制误差(px)；dt_ms 为帧间隔(ms)。
    // 返回整形后的 (out_dx, out_dy)。未启用/未激活/零移动时原样返回。
    void shape(int16_t* out_dx, int16_t* out_dy, float ex, float ey, float dt_ms,
               const PersonalTrajectoryConfig& cfg) {
        const int raw_x = static_cast<int>(*out_dx);
        const int raw_y = static_cast<int>(*out_dy);
        const int dx = raw_x, dy = raw_y;
        // ★ 每帧先清零：本帧没注入抖动就必须是 0（前馈读数不能沿用上一帧的残值）。
        last_jitter_x_ = 0.0f;
        last_jitter_y_ = 0.0f;
        if (dx == 0 && dy == 0) return;
        if (!(cfg.enabled && active_)) return;

        const float dt = clamp_(dt_ms, 0.1f, 40.0f);
        const float err_len = std::hypot(ex, ey);
        const float error_speed = error_speed_px_s_;  // 由外部通过 set_error_speed 更新
        const float target_age = target_age_ms_;      // 由外部通过 set_target_age 更新
        const float response = std::max(0.05f, cfg.response_px_per_count);
        const float radius = target_radius_px_;

        // 方向突变检测
        const bool direction_changed = direction_changed_(ex, ey, err_len, cfg);
        // 硬抑制（大误差/目标快/目标老/方向突变 → 停用整形，直出）
        const bool hard_reason = !cfg.adaptive_enabled ? false : hard_suppression_reason_(
            err_len, error_speed, target_age, direction_changed, cfg);
        elapsed_ms_ = hard_reason ? std::min(cfg.capture_priority_ms, elapsed_ms_ + dt)
                                  : elapsed_ms_ + dt;

        // 进度：取"时间进度"与"距离进度×0.65"的较大值（防时间包络落后快纠偏）
        const float measured_progress = 1.0f - err_len / std::max(start_distance_px_, 1.0f);
        goal_progress_ = std::max(goal_progress_, clamp_(measured_progress, 0.0f, 1.0f));
        const float time_progress = clamp_(elapsed_ms_ / std::max(duration_ms_, 1.0f), 0.0f, 1.0f);
        const float progress = std::max(time_progress, goal_progress_ * 0.65f);

        // 自适应强度（capture 初期/大误差/快目标/老目标/接近目标时降强度）
        float strength = 0.0f;
        std::string soft_reason;
        if (cfg.adaptive_enabled) {
            std::tie(strength, soft_reason) = adaptive_strength_(err_len, error_speed,
                                                                  target_age, cfg);
        } else {
            strength = 1.0f;
        }
        const bool suppress = hard_reason || !soft_reason.empty();

        // 速度包络（偏峰铃形）
        const float envelope = skewed_bell_(progress, peak_time_fraction_);
        const float bell_strength = 0.35f + 0.65f * bell_correlation_;
        const float transport_window = std::pow(std::sin(kPi * progress), 1.35f);
        const float response_gain_scale = std::min(1.0f, 1.5f / std::max(1.0f, response));
        const float gain = 1.0f + cfg.transport_gain * bell_strength * envelope
                                 * transport_window * strength * response_gain_scale;

        // 反向检测：raw 与上一帧方向相反 → 清增益余数（防叠加）
        if (suppress) { gain_round_x_ = 0.0f; gain_round_y_ = 0.0f; }
        if (last_raw_x_ * dx < 0) gain_round_x_ = 0.0f;
        if (last_raw_y_ * dy < 0) gain_round_y_ = 0.0f;
        last_raw_x_ = dx;
        last_raw_y_ = dy;

        // 增益附加（含余数累积，防整数截断丢失）
        const float desired_extra_x = static_cast<float>(dx) * (gain - 1.0f) + gain_round_x_;
        const float desired_extra_y = static_cast<float>(dy) * (gain - 1.0f) + gain_round_y_;
        const int extra_x = static_cast<int>(std::trunc(desired_extra_x));
        const int extra_y = static_cast<int>(std::trunc(desired_extra_y));
        gain_round_x_ = desired_extra_x - static_cast<float>(extra_x);
        gain_round_y_ = desired_extra_y - static_cast<float>(extra_y);
        float out_x = static_cast<float>(dx + extra_x);
        float out_y = static_cast<float>(dy + extra_y);

        // 垂直向 AR(1) 随机游走抖动（垂直于当前移动方向）
        const float mag = std::hypot(static_cast<float>(dx), static_cast<float>(dy));
        float perp_offset = 0.0f;
        const float visual_budget = variation_budget_(cfg, radius);
        if (mag > 0.0f && err_len >= cfg.min_error_px && strength > 0.0f && visual_budget > 0.0f) {
            const float ux = static_cast<float>(dx) / mag;
            const float uy = static_cast<float>(dy) / mag;
            const float profile_sigma = (cfg.curve_rms_px / 10.0f + cfg.jitter_amp_px * 0.10f)
                                        * cfg.variation_scale / std::max(0.70f, cfg.stability_scale);
            const float stationary_sigma = std::min(visual_budget, std::max(0.0f, profile_sigma));
            const float tau_ms = cfg.curve_time_constant_ms * std::max(0.75f, cfg.stability_scale);
            const float decay = std::exp(-dt / std::max(tau_ms, 1.0f));
            const float innovation = stationary_sigma * std::sqrt(std::max(0.0f, 1.0f - decay * decay));
            curve_state_ = curve_state_ * decay + gauss_(0.0f, innovation);
            perp_offset = clamp_(curve_state_ * transport_window * strength * curve_sign_,
                                 -visual_budget, visual_budget);
            // ★ V3 阶段 5：垂直抖动是"随机分量"，报出去给前馈扣除。
            //   注意只报这一项 —— transport 增益（gain-1 那部分）是故意要走的速度
            //   包络，扣掉会让 PID 以为还没到位而继续加力 ⇒ 过冲。
            last_jitter_x_ = -uy * perp_offset;
            last_jitter_y_ = ux * perp_offset;
            out_x += last_jitter_x_;
            out_y += last_jitter_y_;
        } else {
            const float tau = std::max(cfg.curve_time_constant_ms, 1.0f);
            curve_state_ *= std::exp(-dt / tau);
        }

        // 量化 + 安全守卫
        int ix = static_cast<int>(std::lround(out_x));
        int iy = static_cast<int>(std::lround(out_y));
        guard_output_(&ix, &iy, dx, dy, ex, ey, cfg);
        shape_count_++;
        if (ix != dx || iy != dy) applied_count_++;
        else if (suppress) pause_count_++;

        *out_dx = static_cast<int16_t>(ix);
        *out_dy = static_cast<int16_t>(iy);
    }

    // 目标切换 / 瞄准退出时重置状态（不关闭 enabled）
    void reset() {
        active_ = false;
        elapsed_ms_ = 0.0f;
        duration_ms_ = 0.0f;
        start_distance_px_ = 1.0f;
        curve_state_ = 0.0f;
        curve_sign_ = 1.0f;
        gain_round_x_ = 0.0f;
        gain_round_y_ = 0.0f;
        last_raw_x_ = 0;
        last_raw_y_ = 0;
        goal_progress_ = 0.0f;
        has_last_error_ = false;
        last_error_x_ = 0.0f;
        last_error_y_ = 0.0f;
        last_jitter_x_ = 0.0f;
        last_jitter_y_ = 0.0f;
    }

    // 外部运行时参数（每帧由 AimThread 提供）
    void set_error_speed_px_s(float v) { error_speed_px_s_ = v; }
    void set_target_age_ms(float v) { target_age_ms_ = v; }
    void set_target_radius_px(float v) { target_radius_px_ = v; }
    // V3 阶段 5：本帧注入的垂直随机抖动分量（count 域，guard 之前的值）。
    // 未启用 / 未激活 / 被抑制 / 误差太小 ⇒ 恒为 0。
    float last_jitter_x() const { return last_jitter_x_; }
    float last_jitter_y() const { return last_jitter_y_; }

    // 统计（供 Web/诊断）
    uint64_t shape_count() const { return shape_count_; }
    uint64_t applied_count() const { return applied_count_; }
    uint64_t pause_count() const { return pause_count_; }
    bool active() const { return active_; }

private:
    static float clamp_(float v, float lo, float hi) {
        return std::max(lo, std::min(hi, v));
    }
    static float smoothstep_(float v) {
        const float t = clamp_(v, 0.0f, 1.0f);
        return t * t * (3.0f - 2.0f * t);
    }
    static bool same_sign_or_zero_(float a, float b) {
        return a == 0.0f || b == 0.0f || (a > 0) == (b > 0);
    }
    static float skewed_bell_(float progress, float peak) {
        const float t = clamp_(progress, 0.0f, 1.0f);
        const float p = clamp_(peak, 0.22f, 0.78f);
        if (t <= 0.0f || t >= 1.0f) return 0.0f;
        const float u = (t <= p) ? (t / std::max(p, 1e-6f))
                                 : ((1.0f - t) / std::max(1.0f - p, 1e-6f));
        const float uc = clamp_(u, 0.0f, 1.0f);
        return std::pow(std::sin(uc * kPi * 0.5f), 1.35f);
    }
    static float decreasing_factor_(float value, float full_until, float zero_at) {
        if (value <= full_until) return 1.0f;
        if (value >= zero_at) return 0.0f;
        return 1.0f - smoothstep_((value - full_until) / std::max(zero_at - full_until, 1e-6f));
    }

    float _runtime_duration_ms(float predicted, const PersonalTrajectoryConfig& cfg) const {
        const float norm = clamp_((predicted - 80.0f) / 1120.0f, 0.0f, 1.0f);
        const float base = 95.0f + 100.0f * std::sqrt(norm);
        return base / std::max(0.75f, cfg.speed_scale);
    }

    bool direction_changed_(float ex, float ey, float err_len,
                            const PersonalTrajectoryConfig& cfg) {
        bool changed = false;
        if (has_last_error_) {
            const float prev_len = std::hypot(last_error_x_, last_error_y_);
            const float floor_ = std::max(8.0f, cfg.min_error_px * 0.70f);
            if (prev_len >= floor_ && err_len >= floor_) {
                const float cosine = (last_error_x_ * ex + last_error_y_ * ey)
                                     / std::max(prev_len * err_len, 1e-6f);
                changed = cosine < cfg.direction_change_cosine;
            }
        }
        last_error_x_ = ex;
        last_error_y_ = ey;
        has_last_error_ = true;
        if (changed) {
            // 方向突变 → 重置本次移动（重新计时/曲线）
            elapsed_ms_ = 0.0f;
            start_distance_px_ = std::max(1.0f, err_len);
            goal_progress_ = 0.0f;
            curve_state_ = 0.0f;
            gain_round_x_ = 0.0f;
            gain_round_y_ = 0.0f;
            last_raw_x_ = 0;
            last_raw_y_ = 0;
            curve_sign_ = (rng_01_() < 0.5f) ? -1.0f : 1.0f;
        }
        return changed;
    }

    bool hard_suppression_reason_(float err_len, float error_speed, float target_age,
                                  bool direction_changed,
                                  const PersonalTrajectoryConfig& cfg) const {
        if (direction_changed) return true;
        if (err_len >= cfg.urgent_error_px) return true;
        if (error_speed >= cfg.urgent_speed_px_s) return true;
        if (target_age > cfg.max_target_age_ms) return true;
        return false;
    }

    std::pair<float, std::string> adaptive_strength_(
        float err_len, float error_speed, float target_age,
        const PersonalTrajectoryConfig& cfg) {
        if (!cfg.adaptive_enabled) return {1.0f, ""};
        if (elapsed_ms_ <= cfg.capture_priority_ms) return {0.0f, "capture_priority"};
        const float capture_ramp_ms = std::max(8.0f, cfg.capture_priority_ms * 0.65f);
        const float capture_factor = smoothstep_(
            (elapsed_ms_ - cfg.capture_priority_ms) / capture_ramp_ms);
        const float error_full = std::max(cfg.min_error_px + 4.0f, cfg.urgent_error_px * 0.72f);
        const float speed_full = cfg.urgent_speed_px_s * 0.55f;
        const float age_full = cfg.max_target_age_ms * 0.55f;
        const float error_factor = decreasing_factor_(err_len, error_full, cfg.urgent_error_px);
        const float speed_factor = decreasing_factor_(error_speed, speed_full, cfg.urgent_speed_px_s);
        const float age_factor = decreasing_factor_(target_age, age_full, cfg.max_target_age_ms);
        const float proximity_full = std::min(
            error_full, cfg.min_error_px + std::max(10.0f, cfg.min_error_px * 0.75f));
        const float proximity_factor = smoothstep_(
            (err_len - cfg.min_error_px)
            / std::max(proximity_full - cfg.min_error_px, 1.0f));
        const float quality_factor = 0.55f + 0.45f * holdout_similarity_score_;
        const float strength = clamp_(capture_factor * error_factor * speed_factor
                                      * age_factor * proximity_factor * quality_factor,
                                      0.0f, 1.0f);
        if (proximity_factor < 0.05f) return {0.0f, "settle_priority"};
        return {strength, (strength > 1e-4f) ? "" : "adaptive_zero"};
    }

    float variation_budget_(const PersonalTrajectoryConfig& cfg, float target_radius_px) const {
        float visual_budget = cfg.max_visual_variation_px;
        visual_budget *= cfg.variation_scale / std::max(0.70f, cfg.stability_scale);
        if (target_radius_px > 0.0f) visual_budget = std::min(visual_budget, target_radius_px * 0.10f);
        visual_budget = std::max(0.0f, visual_budget);
        const float response = std::max(0.05f, cfg.response_px_per_count);
        // 一个 HID count 已超过视觉预算 → 位置抖动不安全 → 返回 0（禁止抖动）
        if (response > visual_budget + 1e-6f) return 0.0f;
        return std::min(cfg.max_extra_px, visual_budget / response);
    }

    // 安全守卫：幅度限制 + 保持与误差同向 + 禁止反向投影 + 能量不增
    void guard_output_(int* ix, int* iy, int raw_x, int raw_y,
                       float ex, float ey, const PersonalTrajectoryConfig& cfg) {
        const int max_x = std::abs(raw_x) + static_cast<int>(cfg.max_extra_px);
        const int max_y = std::abs(raw_y) + static_cast<int>(cfg.max_extra_px);
        *ix = std::max(-max_x, std::min(max_x, *ix));
        *iy = std::max(-max_y, std::min(max_y, *iy));
        if (*ix != 0 && !same_sign_or_zero_(static_cast<float>(*ix), ex)) *ix = (raw_x == 0) ? 0 : raw_x;
        if (*iy != 0 && !same_sign_or_zero_(static_cast<float>(*iy), ey)) *iy = (raw_y == 0) ? 0 : raw_y;
        if (raw_x != 0 && *ix == 0 && same_sign_or_zero_(static_cast<float>(raw_x), ex))
            *ix = static_cast<int>(std::copysign(1.0f, static_cast<float>(raw_x)));
        if (raw_y != 0 && *iy == 0 && same_sign_or_zero_(static_cast<float>(raw_y), ey))
            *iy = static_cast<int>(std::copysign(1.0f, static_cast<float>(raw_y)));
        const int64_t raw_energy =
            static_cast<int64_t>(raw_x) * raw_x + static_cast<int64_t>(raw_y) * raw_y;
        const int64_t shaped_projection =
            static_cast<int64_t>(*ix) * raw_x + static_cast<int64_t>(*iy) * raw_y;
        if (raw_energy > 0 && shaped_projection < raw_energy) { *ix = raw_x; *iy = raw_y; }
    }

    // 简易确定性随机数（xorshift，缓冲 0.99999 防止 gauss 除 0）
    float rng_01_() {
        x_ ^= x_ << 13; x_ ^= x_ >> 17; x_ ^= x_ << 5;
        return std::min(0.99999f, static_cast<float>(x_ & 0xFFFFFFu) / 16777216.0f);
    }
    // Box-Muller 高斯
    float gauss_(float mean, float stddev) {
        const float u1 = std::max(1e-6f, rng_01_());
        const float u2 = rng_01_();
        const float z = std::sqrt(-2.0f * std::log(u1)) * std::cos(6.2831853f * u2);
        return mean + stddev * z;
    }

    uint32_t x_ = 0x9E3779B9u;  // 固定种子（确定性，便于测试复现）
    bool active_ = false;
    float elapsed_ms_ = 0.0f;
    float duration_ms_ = 0.0f;
    float start_distance_px_ = 1.0f;
    float curve_state_ = 0.0f;
    float curve_sign_ = 1.0f;
    float gain_round_x_ = 0.0f;
    float gain_round_y_ = 0.0f;
    int last_raw_x_ = 0;
    int last_raw_y_ = 0;
    float goal_progress_ = 0.0f;
    float last_error_x_ = 0.0f;
    float last_error_y_ = 0.0f;
    bool has_last_error_ = false;
    float peak_time_fraction_ = 0.50f;   // 由配置/标定填充（默认居中峰）
    float bell_correlation_ = 0.75f;     // 默认经验包络权重相关
    float holdout_similarity_score_ = 0.0f;  // 0=用纯解析包络
    float error_speed_px_s_ = 0.0f;
    float target_age_ms_ = 0.0f;
    float target_radius_px_ = 0.0f;
    // V3 阶段 5：本帧注入的垂直随机抖动（count），供前馈扣除读取。
    float last_jitter_x_ = 0.0f;
    float last_jitter_y_ = 0.0f;
    uint64_t shape_count_ = 0;
    uint64_t applied_count_ = 0;
    uint64_t pause_count_ = 0;
};

}  // namespace ttbox::core::aim
