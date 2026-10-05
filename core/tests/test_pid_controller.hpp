// test_pid_controller.hpp — 测试专用：双轴 SmoothAimController + 输出链。
//
// 生产真源：AimThread 直接用 aim/SmoothAimController（EMA+比例+限幅+框高死区）+ 自己的输出链
// （sens×scale → 死区 → 余数累积 → int16 clamp）。本 helper 只给"纯算法"测试
// （test_tracker / test_pipeline / test_real_model / test_win_e2e / pipeline_bench）
// 提供同款闭环末端，避免测试直接复制输出链逻辑。
//
// ★ 参数默认 = 生产 MouseTypes.hpp 真实值（V1.0.41 起 pid1 删除，换 SmoothAimController）。
#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>

#include "aim/SmoothAimController.hpp"
#include "common/CoreContracts.hpp"
#include "pipeline/Target.hpp"

namespace ttbox::core::aim {

// 双轴瞄准控制器参数（测试专用，默认 = MouseTypes.hpp 的 MouseProfile 默认值）
struct TestPidParams {
    float aim_alpha = 0.5f;          // 平滑系数
    float aim_gain = 0.15f;          // 比例增益（误差 px → 移动 count）
    float aim_max_move = 30.0f;      // 单帧最大移动（count）
    float aim_deadzone_ratio = 0.05f; // 死区 = 框高 × ratio
    float sensitivity = 1.0f;        // 全局灵敏度
    float output_scale = 1.0f;       // 输出缩放
    float output_deadzone = 1.0f;    // 输出死区（低于此值归零）
    float reference_x = 0.0f;        // 参考点 x
    float reference_y = 0.0f;        // 参考点 y
};

// 双轴瞄准控制器（测试专用）。行为与 core 的 SmoothAimController 一致。
class TestPidController {
public:
    TestPidController() { configure(params_); }

    void configure(const TestPidParams& params) {
        params_ = params;
        aim_x_.configure(params.aim_alpha, params.aim_gain, params.aim_max_move,
                         params.aim_deadzone_ratio);
        aim_y_.configure(params.aim_alpha, params.aim_gain, params.aim_max_move,
                         params.aim_deadzone_ratio);
    }

    void set_reference(float rx, float ry) {
        params_.reference_x = rx;
        params_.reference_y = ry;
    }

    // 尺寸自适应死区的框高（测试默认 0 ⇒ 死区退化为纯比例，不卡死）。
    // 要测死区行为时显式设一个框高。
    void set_box_h(float box_h) { box_h_ = box_h > 0.0f ? box_h : 0.0f; }

    // 目标点 → 鼠标命令（只计算，绝不写设备）。
    // 输出链与生产 AimThread 一致：控制器 × sens × scale → 死区 → 余数累积 → int16 clamp。
    MouseCommand update(const TargetPoint& point) {
        MouseCommand cmd;
        cmd.valid = false;
        cmd.dx = 0;
        cmd.dy = 0;
        if (!point.valid) {
            remainder_x_ = 0.0f;
            remainder_y_ = 0.0f;
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
        const float aim_x = aim_x_.update(ex, box_h_);
        const float aim_y = aim_y_.update(ey, box_h_);
        const float out_gain = params_.sensitivity * params_.output_scale;
        float scaled_x = aim_x * out_gain;
        float scaled_y = aim_y * out_gain;
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
            aim_x_.reset();
            aim_y_.reset();
        }
        cmd.dx = move_x;
        cmd.dy = move_y;
        cmd.valid = true;
        return cmd;
    }

    void reset() {
        aim_x_.reset();
        aim_y_.reset();
        remainder_x_ = 0.0f;
        remainder_y_ = 0.0f;
    }

private:
    SmoothAimController aim_x_;
    SmoothAimController aim_y_;
    TestPidParams params_;
    float remainder_x_ = 0.0f;
    float remainder_y_ = 0.0f;
    float box_h_ = 0.0f;  // 尺寸自适应死区的框高（0 = 退化为纯比例）
};

}  // namespace ttbox::core::aim
