// test_pid_controller.hpp — 测试专用：双轴 Pid1Controller + 输出链。
//
// 生产真源：AimThread 直接用 aim/Pid1Controller（pid1.cpp 移植）+ 自己的输出链
// （sens×scale → 死区 → 余数累积 → int16 clamp）。本 helper 只给"纯算法"测试
// （test_tracker / test_pipeline / test_real_model / test_win_e2e / pipeline_bench）
// 提供同款闭环末端，避免测试直接复制输出链逻辑。
//
// ★ 参数默认 = 生产 MouseTypes.hpp 真实值（V1.0.13 换域后）：
//   kp=0.25 / kd=0.25 / predict_x=1.0 / predict_y=0.0 / rate=0.3 / smooth=0（直通）。
//   旧 controller/PidController 的默认值（kp_x=17 / predict_x=0.008 / smooth_x=9900）
//   是过时的出场默认值，已随死封装一并删除。
#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>

#include "aim/Pid1Controller.hpp"
#include "common/CoreContracts.hpp"
#include "pipeline/Target.hpp"

namespace ttbox::core::aim {

// 双轴 PID 参数（测试专用，默认 = 生产真实值）
struct TestPidParams {
    float kp_x = 0.25f;       // X 比例增益（生产真实值）
    float kp_y = 0.25f;
    float kd_x = 0.25f;       // 微分增益（pid1 刹车）
    float kd_y = 0.25f;
    float predict_x = 1.0f;   // X 前馈（追左右移动目标）
    float predict_y = 0.0f;   // Y 不带前馈
    float rate_x = 0.3f;      // 输出速率（pid1 kp_gain_rate）
    float rate_y = 0.3f;
    float sensitivity = 1.0f;     // 全局灵敏度
    float output_scale = 1.0f;    // 输出缩放
    float output_deadzone = 1.0f; // 输出死区（低于此值归零）
    float reference_x = 0.0f;     // 参考点 x
    float reference_y = 0.0f;
};

// 双轴 PID 控制器（测试专用）。行为与旧 controller/PidController 一致，
// 仅参数默认换生产真实值 + smooth 恒 0（V1.0.13 已折叠进 kp/kd）。
class TestPidController {
public:
    TestPidController() {
        // 与 AimThread 构造一致：init 用 pid1.cpp 原始演示值（马上被 configure 覆盖）
        pid_x_.init(25.0, 25.0, 3.0, 0.3, 9900.0);
        pid_y_.init(25.0, 25.0, 0.0, 0.3, 9900.0);
    }

    void configure(const TestPidParams& params) {
        params_ = params;
        // smooth 恒 0：直通（V1.0.13 后 smooth 已从参数面删除，折叠进 kp/kd）
        pid_x_.configure(params.kp_x, params.kd_x, params.predict_x,
                         params.rate_x, 0.0);
        pid_y_.configure(params.kp_y, params.kd_y, params.predict_y,
                         params.rate_y, 0.0);
    }

    void set_reference(float rx, float ry) {
        params_.reference_x = rx;
        params_.reference_y = ry;
    }

    // 目标点 → 鼠标命令（只计算，绝不写设备）。
    // 输出链与生产 AimThread 一致：P_PID × sens × scale → 死区 → 余数累积 → int16 clamp。
    MouseCommand update(const TargetPoint& point) {
        MouseCommand cmd;
        cmd.valid = false;
        cmd.dx = 0;
        cmd.dy = 0;
        if (!point.valid) {
            remainder_x_ = 0.0f;
            remainder_y_ = 0.0f;
            last_error_x_ = 0.0f;
            last_error_y_ = 0.0f;
            return cmd;
        }
        if (!std::isfinite(point.x) || !std::isfinite(point.y)) {
            reset();
            return cmd;
        }
        const float ex = point.x - params_.reference_x;
        const float ey = point.y - params_.reference_y;
        if (!std::isfinite(ex) || !std::isfinite(ey)) {
            reset();
            return cmd;
        }
        last_error_x_ = ex;
        last_error_y_ = ey;
        const float pid_x = static_cast<float>(pid_x_.update(ex));
        const float pid_y = static_cast<float>(pid_y_.update(ey));
        const float out_gain = params_.sensitivity * params_.output_scale;
        float scaled_x = pid_x * out_gain;
        float scaled_y = pid_y * out_gain;
        if (std::abs(scaled_x) < params_.output_deadzone) scaled_x = 0.0f;
        if (std::abs(scaled_y) < params_.output_deadzone) scaled_y = 0.0f;
        remainder_x_ += scaled_x;
        remainder_y_ += scaled_y;
        constexpr float kHidMax = 32767.0f;
        constexpr float kHidMin = -32768.0f;
        const float cx_f = std::clamp(remainder_x_, kHidMin, kHidMax);
        const float cy_f = std::clamp(remainder_y_, kHidMin, kHidMax);
        const int16_t move_x = static_cast<int16_t>(cx_f);
        const int16_t move_y = static_cast<int16_t>(cy_f);
        remainder_x_ -= static_cast<float>(move_x);
        remainder_y_ -= static_cast<float>(move_y);
        if (!std::isfinite(remainder_x_) || !std::isfinite(remainder_y_)) {
            remainder_x_ = 0.0f;
            remainder_y_ = 0.0f;
            pid_x_.reset();
            pid_y_.reset();
        }
        cmd.dx = move_x;
        cmd.dy = move_y;
        cmd.valid = true;
        return cmd;
    }

    void reset() {
        pid_x_.reset();
        pid_y_.reset();
        remainder_x_ = 0.0f;
        remainder_y_ = 0.0f;
        last_error_x_ = 0.0f;
        last_error_y_ = 0.0f;
    }

private:
    Pid1Controller pid_x_;
    Pid1Controller pid_y_;
    TestPidParams params_;
    float remainder_x_ = 0.0f;
    float remainder_y_ = 0.0f;
    float last_error_x_ = 0.0f;
    float last_error_y_ = 0.0f;
};

}  // namespace ttbox::core::aim
