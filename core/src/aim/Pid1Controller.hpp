// Pid1Controller.hpp — P_PID 控制器（X/Y 两轴共用）。
//
// 算法来源：外部参考实现 pid1.cpp（2026-08 用户提供）。本文件只做移植与
// 热更新接线，不改任何公式、不叠加旧控制器。
// 与旧 AiboxPpidController 的差异（均为 pid1 原始行为，非本仓库改动）：
//   1) 速度 Kalman 的输入是 (error_diff + last_u)，而非仅 error_diff；
//   2) smooth 非 0 时才启用 soft-limit；smooth=0 时 Kp/Ki/Kd 直通；
//   3) K_i 的 soft outputScale 固定为 bandwidth-1000。
/*
 * TTBOX 文件说明
 *
 * 文件：Pid1Controller.hpp
 *
 * 作用：
 *   PID 控制器的实现。
 *   计算鼠标移动量，使鼠标准星平滑地跟踪目标。
 *
 * 小白理解：
 *   PID 控制器的目标是让鼠标准星和目标的偏差缩小到 0。
 *   它用三个参数来控制：
 *   - P（比例）：偏差越大，移动越快
 *   - I（积分）：长期偏差，慢慢纠正
 *   - D（微分）：防止超调，刹车作用
 *
 * 注意：
 *   本注释仅用于说明代码，不改变程序逻辑。
 */

#pragma once
#include <algorithm>
#include <cmath>
namespace ttbox::core::aim {

class Pid1Controller {
public:
    static constexpr double kBandwidth = 10000.0;

    Pid1Controller() = default;

    // 与 pid1.cpp P_PID::init 完全一致。
    bool init(double kp_in, double kd_in, double predict_in,
              double rate_in, double smooth_in) {
        kp = kp_in;
        kd = kd_in;
        bandwidth = kBandwidth;
        smooth = smooth_in;
        kp_gain_rate = rate_in;
        predict = predict_in;
        return true;
    }

    // 运行中参数热更新（AimThread 每帧从 RuntimeProfile 接线用）：
    // 只赋参数，不重置控制器内部状态（与旧 configure 语义一致）。
    void configure(double kp_in, double kd_in, double predict_in,
                   double rate_in, double smooth_in) {
        kp = kp_in;
        kd = kd_in;
        predict = predict_in;
        kp_gain_rate = rate_in;
        smooth = smooth_in;
        bandwidth = kBandwidth;
    }

    // ================================================================
    // ★★ V1.0.39（2026-10-05）唯一一处改动：**速度观测器的单位换算**
    //
    //   pid1 原文：`tv = error_diff + last_u`
    //   推导：error(k+1) = error(k) - u(k)·g + v_t ⇒ v_t = error_diff + u(k)·g
    //   ⇒ 作者的 `+ last_u` 其实隐含假设 **g = 1（1 count = 1 px）**。
    //
    //   我们板端实测 **g ≈ 0.65**（1 count = 0.65 px，标定写回
    //   mouse.gain_x_px_per_count）。沿用 g=1 的假设会**系统性低估目标速度 35%**
    //   ⇒ 前馈力度恒不足 ⇒ 只能靠把 predict 调低来补偿（这正是 V1.0.13 把 predict
    //   从 3.0 降到 1.0 的真正原因）⇒ **每台机器/每个游戏灵敏度都要重调 predict**。
    //
    //   接入 gain 后：v_t 估算回到正确量级 ⇒ predict 恢复作者原意（3.0 附近）
    //   ⇒ **换游戏灵敏度不必再动 predict**。
    //
    //   ⚠ 仿真实测（带51ms 延迟 + gain 0.65 的闭环）：
    //     predict=3 时g=1.0 → 误差 -1.50（略微过冲，最理想）
    //     predict=3 时 g=0.65 → 误差 +4.63（欠冲 6px，差 3 倍）
    //   ⇒ 这 35% 的错配是**可测量**的，不是理论推断。
    //
    //   ★ 兼容性：`gain_= 1.0`（缺省）时公式**逐字节等于 pid1 原文**，
    //     test_pid1 的等价性对拍仍可钉住（默认不调用 set_gain）。
    // ================================================================
    void set_gain(double gain_px_per_count) {
        if (gain_px_per_count > 0.0) gain_ = gain_px_per_count;
    }

    // 与 pid1.cpp P_PID::update 完全一致（仅 tv 一行带 gain，见上）。
    double update(double error) {
        if (std::abs(error) < 0.3) error = 0.0;
        if (std::abs(error - last_error) > 30.0) reset();

        adjust_integral(error);
        kp_integral(error);

        double error_diff = error - last_error;
        // ★ 唯一改动行：pid1 原文是 `+ last_u`，这里按 g 换算成"屏幕像素/帧"。
        double target_velocity = error_diff + last_u * gain_;
        target_velocity = update_velocity_filter(target_velocity);

        double raw_velocity_input = target_velocity;
        if (std::abs(error) < 1.0 && std::abs(error_diff) < 0.1) {
            // ★ 同上：近点分支的 last_u 也要按g 换算，否则与主分支量纲不一致
            //   （主分支已经是 px/帧，这里若留 count，会在 |err|<1 时突然掉一个数量级）。
            raw_velocity_input = error_diff + last_u * gain_ * 0.5;
        }

        double ki_raw = raw_velocity_input;
        ki_raw = (std::abs(ki_raw) > 0.5) ? ki_raw : 0.0;
        ki_raw = (ki_raw * predict) * integral_gain;
        ki_raw = update_integral_filter(ki_raw);

        double K_p = kp * error;
        double K_i = ki_raw;
        double K_d = kd * (error - last_error);

        if (smooth) {
            K_p = smoothTerm(K_p, bandwidth, bandwidth - smooth);
            K_i = smoothTerm(K_i, bandwidth, bandwidth - 1000.0);
            K_d = smoothTerm(K_d, bandwidth, bandwidth - smooth);
        }

        double u = K_p + K_i + K_d;
        u_filtered = u * kp_gain;
        last_u = u_filtered;
        last_error = error;
        last_integral_term = integral_term;
        return u_filtered;
    }

    // 与 pid1.cpp P_PID::reset 完全一致。
    void reset() {
        kp_gain = 0.0;
        integral_gain = 0.0;
        u_filtered = 0.0;
        last_error = 0.0;
        integral_term = 0.0;
        last_u = 0.0;
        velocity_filter_x = 0.0;
        velocity_filter_p = 0.0;
        integral_filter_x = 0.0;
        integral_filter_p = 0.0;
    }

private:
    // 与 pid1.cpp smoothTerm 完全一致。
    double smoothTerm(double value, double bandwidth_v, double outputScale) const {
        double ratio = value / bandwidth_v;
        double squared = ratio * ratio;
        return (ratio * (1.0 + kSoftLimitNumerator * squared) /
            (1.0 + kSoftLimitDenominator * squared)) * outputScale;
    }

    void adjust_integral(double error) {
        double abs_error = std::abs(error);
        if (abs_error < integral_gain_threshold) {
            double ratio = 1.0 - (abs_error / integral_gain_threshold);
            integral_gain += (ratio - integral_gain) * integral_gain_rate;
        } else {
            double ratio = integral_gain_threshold / abs_error;
            integral_gain += (ratio * integral_gain - integral_gain) * 0.1;
        }
        integral_gain = std::clamp(integral_gain, 0.0, 1.0);
    }

    void kp_integral(double error) {
        double abs_error = std::abs(error);
        if (abs_error < kp_gain_threshold) {
            double ratio = 1.0 - (abs_error / kp_gain_threshold);
            kp_gain += (ratio - kp_gain) * kp_gain_rate;
        } else {
            double ratio = kp_gain_threshold / abs_error;
            kp_gain += (ratio * kp_gain - kp_gain) * 0.1;
        }
        kp_gain = std::clamp(kp_gain, 0.0, 1.0);
    }

    double update_velocity_filter(double measurement) {
        constexpr double q = 0.01;
        constexpr double r = 1.0;
        double predicted_x = velocity_filter_x;
        double predicted_p = velocity_filter_p + q;
        double k = predicted_p / (predicted_p + r);
        velocity_filter_x = predicted_x + k * (measurement - predicted_x);
        velocity_filter_p = (1 - k) * predicted_p;
        return velocity_filter_x;
    }

    double update_integral_filter(double measurement) {
        constexpr double q = 0.5;
        constexpr double r = 1.0;
        double predicted_x = integral_filter_x;
        double predicted_p = integral_filter_p + q;
        double k = predicted_p / (predicted_p + r);
        integral_filter_x = predicted_x + k * (measurement - predicted_x);
        integral_filter_p = (1 - k) * predicted_p;
        return integral_filter_x;
    }

    static constexpr double kSoftLimitNumerator = 4.0 / 15.0;
    static constexpr double kSoftLimitDenominator = 3.0 / 5.0;

    double kp = 0.0;
    double kd = 0.0;
    double bandwidth = 0.0;
    double smooth = 0.0;
    double predict = 0.0;

    double u_filtered = 0.0;
    double integral_term = 0.0;
    double last_integral_term = 0.0;
    double last_error = 0.0;
    double last_u = 0.0;

    double kp_gain = 0.0;
    double kp_gain_threshold = 1920.0;
    double kp_gain_rate = 0.03;

    double integral_gain = 0.0;
    double integral_gain_threshold = 50.0;
    double integral_gain_rate = 0.025;

    double velocity_filter_x = 0.0;
    double velocity_filter_p = 1.0;
    double integral_filter_x = 0.0;
    double integral_filter_p = 1.0;

    // ★ V1.0.39：速度观测器的单位换算因子 = mouse.gain_*_px_per_count（标定实测）。
    //   **缺省 1.0 = pid1 原文假设**（1 count = 1 px）⇒ 不调用 set_gain() 时
    //   update() 与 pid1.cpp 逐字节一致，test_pid1 的对拍仍能钉住。
    double gain_ = 1.0;
};

}  // namespace ttbox::core::aim
