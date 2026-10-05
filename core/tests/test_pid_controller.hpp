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

// 双轴 PID 参数（测试专用，默认 = pid1.cpp main() 原始值）
struct TestPidParams {
    // ★★★ V1.0.38：回归 pid1 原文 runAxis 的原始参数（V1.0.13~V1.0.37 曾是
    //   kp=0.25 / predict_x=1.0 / smooth 恒 0，那是"折算口径"，本次全部撤回）。
    //   判据：与 core/src/mouse/MouseTypes.hpp 的 MouseProfile 默认值保持一致。
    float kp_x = 25.0f;       // X 比例增益（pid1 原始值，未折算）
    float kp_y = 25.0f;
    float kd_x = 25.0f;       // 微分增益（pid1 刹车）
    float kd_y = 25.0f;
    float predict_x = 3.0f;   // X 前馈（pid1 原始值）
    float predict_y = 0.0f;   // Y 不带前馈（pid1 原始值）
    float rate_x = 0.3f;      // 输出速率（pid1 kp_gain_rate）
    float rate_y = 0.3f;
    // ★ smooth 恢复进测试桩：此前 configure() 硬编码传 0.0，等于让所有 PID 行为测试
    //   都跑在「soft-limit 关闭」的口径上，与生产不符（生产 V1.0.38 起传 9900）。
    float smooth_x = 9900.0f; // pid1 soft-limit 强度
    float smooth_y = 9900.0f;
    float sensitivity = 1.0f;     // 全局灵敏度
    float output_scale = 1.0f;    // 输出缩放
    float output_deadzone = 1.0f; // 输出死区（低于此值归零）
    float reference_x = 0.0f;     // 参考点 x
    float reference_y = 0.0f;
};

// 双轴 PID 控制器（测试专用）。行为与 core 的 Pid1Controller 一致，
// ★ V1.0.38：smooth 不再硬编码 0，改由 params 传入（与生产 AimThread 接线同口径）。
class TestPidController {
public:
    TestPidController() {
        // 与 AimThread::start() 构造一致：pid1.cpp main() 原始演示值
        pid_x_.init(25.0, 25.0, 3.0, 0.3, 9900.0);
        pid_y_.init(25.0, 25.0, 0.0, 0.3, 9900.0);
        // ★ V1.0.38：紧接着按默认 params configure 一次。
        //   生产里 AimThread 每帧都会用 frame_profile 调 configure()，从不存在
        //   "只有 init、没 configure"的状态；而本桩的 init 值（pid1 演示用 predict=3.0）
        //   与默认 params（predict_x=3.0 但 kp/kd/smooth 走 params）不一定同源。
        //   不补这一次 ⇒ 不调 configure() 的用例（如 test_tracker 全部场景）
        //   会一直跑在 init 的演示参数上，params_ 里的死区/灵敏度等根本没生效。
        configure(params_);
    }

    void configure(const TestPidParams& params) {
        params_ = params;
        // ★ V1.0.38：第 5 参传真实 smooth（生产 AimThread.cpp:274-277 同款），
        //   不再恒传 0.0 —— 否则本桩跑的是"soft-limit 关闭"口径，
        //   与生产不一致，所有 PID 行为断言都建立在错误前提上。
        pid_x_.configure(params.kp_x, params.kd_x, params.predict_x,
                         params.rate_x, params.smooth_x);
        pid_y_.configure(params.kp_y, params.kd_y, params.predict_y,
                         params.rate_y, params.smooth_y);
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
