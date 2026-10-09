// calibration_math.hpp — 标定拟合数学（纯函数，无 IPC / 无文件 / 无线程）。
//
// 历史：曾自 ttbox_motion/calibration.py 逐行移植，并以该 Python 版作为「对拍金丝雀」。
//       2026-10-07 已彻底去 Python ⇒ ttbox_motion 与 calibration_math_canary.{py,cpp}
//       一并删除（该 canary 的 C++ 侧从未进过 CMake，对拍实际从未运行，形同虚设）。
//       下列口径说明即原 Python 版 V1.0.38 注释的要点，已内联在此以免口径随文件丢失。
//
// 移植时的两条规则（口径本身没变）：
//   · median 与 Python statistics.median 同义（偶数取中间两值平均）。
//   · derive_pid_params 内部按「生效值」做物理推导，返回前**折回「名义值」**
//     （÷ SMOOTH_FACTOR），因为面板/配置层与 core 之间以 smooth 为刻度，
//     而推导是在 smooth≡1 的名义空间里做的。折回规则漏掉会让 kp 被放大 smooth 倍，
//     且不报错、只表现为「瞄得贼快但压不住」——这是该折回存在的原因，勿删。
#pragma once

#include <algorithm>
#include <cmath>
#include <stdexcept>
#include <string>
#include <vector>

namespace ttbox::core::web {

// 标定轴枚举（X / Y）。
enum class CalibrationAxis { kX, kY };

// 轴枚举 → 单字符名（'x' / 'y'）。
inline const char* axis_value(CalibrationAxis a) {
    return a == CalibrationAxis::kX ? "x" : "y";
}

// 响应延迟验证门（ms）。板端 144fps 实测 ≈51ms，旧上限 50ms 会把正常链路判死。
inline constexpr double kResponseDelayMaxMs = 120.0;

// 自动调参锚点常量（物理口径已内联于文件头，原与 ttbox_motion/calibration.py 同值）。
inline constexpr double kKpFractionPerFrame = 0.07;
inline constexpr double kKdRatioBase = 1.0;
inline constexpr double kKdRatioDelayDiv = 25.0;
inline constexpr double kKpEffMin = 0.04;
inline constexpr double kKpMax = 1.0;
inline constexpr double kKdMin = 0.04;
inline constexpr double kKdMax = 0.5;
inline constexpr double kBoardSmooth = 9900.0;
inline constexpr double kSmoothFactor = (10000.0 - kBoardSmooth) / 10000.0;  // 0.01

// 标定幅度表（px 参考点偏置）：正负交替 + 分量程。
inline const double kCalibAmplitudes[] = {12.0, -12.0, 24.0, -24.0, 48.0, -48.0, 96.0, -96.0};
inline constexpr int kCalibAmplitudeCount = 8;
inline constexpr double kCalibAmpTrackRatio = 0.6;
inline constexpr int kCalibAmpMissLimit = 2;

// 温和档 PID（名义值域，V1.0.38 口径）。
inline constexpr double kCalibPidKpMax = 10.0;
inline constexpr double kCalibPidKdRatio = 3.0;
// 单个样本最低信号门槛。
inline constexpr double kCalibMinCounts = 6;
inline constexpr double kCalibMinDeltaPx = 0.5;

// 单次标定观测：注入 count 与实测位移 px、响应延迟、目标身份、是否有效。
struct CalibrationObservation {
    CalibrationAxis axis = CalibrationAxis::kX;
    double injected_count = 0.0;
    double measured_delta_px = 0.0;
    double response_delay_ms = 0.0;
    std::string target_id;
    bool valid = true;
};

// 单轴拟合结果：增益 px/count、响应延迟、样本/剔除数、一致性、是否收敛。
struct AxisFit {
    CalibrationAxis axis = CalibrationAxis::kX;
    double gain_px_per_count = 0.0;
    double response_delay_ms = 0.0;
    int sample_count = 0;
    int rejected_count = 0;
    double consistency = 0.0;
    bool converged = false;
    std::string failure_reason;
};

// 推导出的 PID 名义值（kp / kd / predict）。
struct PidParams {
    double kp = 0.0;
    double kd = 0.0;
    double predict = 0.0;
};

// Python round(x, n) 等价（对非 .5 边界的物理量，half-away 与 half-even 结果一致）。
inline double round_to(double v, int decimals) {
    const double scale = std::pow(10.0, static_cast<double>(decimals));
    return std::round(v * scale) / scale;
}

// 中位数（对齐 statistics.median：奇数取中间，偶数取中间两值平均）。
inline double median_of(std::vector<double> data) {
    if (data.empty()) return 0.0;
    std::sort(data.begin(), data.end());
    const size_t n = data.size();
    if (n % 2 == 1) return data[n / 2];
    return (data[n / 2 - 1] + data[n / 2]) / 2.0;
}

// 按轴估计 px/count 与响应延迟（中位数/MAD 排离群）。逐行对齐 fit_axis_measurements。
inline AxisFit fit_axis_measurements(const std::vector<CalibrationObservation>& observations,
                                     CalibrationAxis axis, double max_relative_mad = 0.35,
                                     int min_samples = 5) {
    AxisFit result;
    result.axis = axis;

    std::vector<CalibrationObservation> items;
    for (const auto& o : observations) {
        if (o.axis == axis && o.valid) items.push_back(o);
    }
    result.sample_count = static_cast<int>(items.size());
    if (static_cast<int>(items.size()) < min_samples) {
        result.failure_reason = std::string(axis_value(axis)) + "轴有效样本不足";
        return result;
    }

    std::vector<std::string> target_ids;
    for (const auto& o : items) {
        if (o.target_id.empty()) continue;
        if (std::find(target_ids.begin(), target_ids.end(), o.target_id) == target_ids.end()) {
            target_ids.push_back(o.target_id);
        }
    }
    if (target_ids.size() > 1) {
        result.failure_reason = "目标身份在标定过程中发生变化";
        return result;
    }

    std::vector<double> ratios;
    for (const auto& item : items) {
        if (item.injected_count <= 0) continue;
        ratios.push_back(item.measured_delta_px / item.injected_count);
    }
    if (static_cast<int>(ratios.size()) < min_samples) {
        result.failure_reason = std::string(axis_value(axis)) + "轴有效输入不足";
        return result;
    }

    const double center = median_of(ratios);
    std::vector<double> deviations;
    deviations.reserve(ratios.size());
    for (double v : ratios) deviations.push_back(std::abs(v - center));
    const double mad = median_of(deviations);
    const double threshold = std::max(std::abs(center) * max_relative_mad, 1e-6);

    std::vector<double> kept;
    for (double v : ratios) {
        if (std::abs(v - center) <= threshold) kept.push_back(v);
    }
    result.rejected_count = static_cast<int>(ratios.size() - kept.size());
    if (static_cast<int>(kept.size()) < min_samples - 1) {
        result.failure_reason = std::string(axis_value(axis)) + "轴测量一致性不足";
        return result;
    }

    result.gain_px_per_count = median_of(kept);
    const double denom = std::max(std::abs(center), 1e-6);
    result.consistency = std::max(0.0, 1.0 - mad / denom);
    if (mad / denom > max_relative_mad) {
        result.failure_reason = std::string(axis_value(axis)) + "轴测量一致性不足";
        return result;
    }

    std::vector<double> delays;
    for (const auto& o : items) delays.push_back(o.response_delay_ms);
    result.response_delay_ms = median_of(delays);

    if (!(0.03 <= result.gain_px_per_count && result.gain_px_per_count <= 8.0)) {
        result.failure_reason = std::string(axis_value(axis)) + "轴增益超出范围";
        return result;
    }
    if (!(0.0 <= result.response_delay_ms && result.response_delay_ms <= kResponseDelayMaxMs)) {
        result.failure_reason = std::string(axis_value(axis)) + "轴响应延迟超出范围";
        return result;
    }
    result.converged = true;
    return result;
}

// 由实测 gain/延迟推导 PID（自动调参核心）。内部按生效值推导、返回前折回名义值。
// gain 非法（<=0）时抛 std::invalid_argument（对齐 Python ValueError）。
inline PidParams derive_pid_params(double gain_x_px_per_count, double gain_y_px_per_count,
                                   double response_delay_ms) {
    if (gain_x_px_per_count <= 0 || gain_y_px_per_count <= 0) {
        throw std::invalid_argument("增益必须 > 0");
    }
    const double gain = std::min(gain_x_px_per_count, gain_y_px_per_count);
    const double delay = std::max(0.0, response_delay_ms);

    double kp = kKpFractionPerFrame / gain;
    if (gain >= 1.2 && delay >= 50.0) kp *= 0.4;
    kp = std::max(kKpEffMin, std::min(kKpMax, kp));

    const double kd_ratio = kKdRatioBase + delay / kKdRatioDelayDiv;
    const double kd = std::max(kKdMin, std::min(kKdMax, kp * kd_ratio));

    const double predict = std::max(0.1, std::min(0.35, 0.35 - delay / 300.0));

    PidParams out;
    out.kp = round_to(kp / kSmoothFactor, 4);
    out.kd = round_to(kd / kSmoothFactor, 4);
    out.predict = round_to(predict, 3);
    return out;
}

}  // namespace ttbox::core::web
