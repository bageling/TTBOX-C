// SmoothAimController.hpp — 简单瞄准控制器（替代 pid1）。
//
// 算法：指数平滑(EMA) + 比例 + 单帧限幅 + 框高×ratio 尺寸自适应死区。
//
// 设计目标（业主 2026-10-05 定）：**调参简单、没有那么多问题**。
// 对比 pid1 的失败教训（见 .workbuddy/memory/2026-10-05.md）：
//   pid1 是「前馈主导」：smooth=9900 把 P/D 压 100 倍、predict 前馈绝对主导，
//   对 gain 极敏感；三个门限(0.3/1920/50)全是绝对像素，换灵敏度/距离就全变样；
//   6 个耦合参数。⇒ 换来换去要么乱飞要么跟不上。
//
// 本控制器 4 个**互相独立**的参数，每个都直白：
//   alpha          平滑系数（越大越跟手、越小越稳；0~1）
//   gain           比例增益（误差 px → 移动 count）
//   max_move       单帧最大移动（count，防一帧打飞）
//   deadzone_ratio 死区 = 目标框高 × ratio（尺寸自适应 ⇒ 远近距离手感一致）
//
// 参考开源共识（2026-10-05 调研）：
//   · cod-ai-aim-assist（github.com/Devloop7）：EMA 平滑 + 比例 + MAX_MOVE 限幅 + MIN_MOVE 死区
//   · 多个 YOLO 脚本共识：死区 = 框尺寸 × 0.4（尺寸自适应，非绝对像素）
#pragma once
#include <algorithm>
#include <cmath>
namespace ttbox::core::aim {

class SmoothAimController {
public:
    SmoothAimController() = default;

    // 每帧从 RuntimeProfile 热更新参数（只赋参数，不重置内部状态）。
    void configure(float alpha, float gain, float max_move, float deadzone_ratio) {
        alpha_ = std::clamp(alpha, 0.0f, 1.0f);
        gain_ = gain;
        max_move_ = max_move > 0.0f ? max_move : 0.0f;
        deadzone_ratio_ = deadzone_ratio > 0.0f ? deadzone_ratio : 0.0f;
    }

    // 单轴更新。
    //   error = 像素误差（瞄准点 − 准星）
    //   box_h = 目标框高（像素；尺寸自适应死区用）
    // 返回本帧鼠标移动量（count 域，方向与误差一致）。
    float update(float error, float box_h) {
        // 1. 绝对死区：误差本身极小 → 锁定（EMA 平滑有惯性，不归零会「到了还继续动」）。
        if (std::abs(error) < kAbsoluteDeadzonePx) {
            last_error_ = 0.0f;
            return 0.0f;
        }
        // 2. 尺寸自适应死区：误差落进「框高×ratio」内 ⇒ 锁定（不追微抖动），平滑归零。
        //    box_h<=0（异常/未框）时跳过此死区 ⇒ 只剩绝对死区，不卡死。
        if (box_h > 0.0f && std::abs(error) < box_h * deadzone_ratio_) {
            last_error_ = 0.0f;
            return 0.0f;
        }
        // 3. 指数平滑（滤检测框帧间抖动，如 y1 ±18px）
        const float smoothed = last_error_ * (1.0f - alpha_) + error * alpha_;
        last_error_ = smoothed;
        // 4. 比例：移动 = 平滑误差 × 增益
        float move = smoothed * gain_;
        // 5. 单帧限幅（防一帧打飞）
        move = std::clamp(move, -max_move_, max_move_);
        return move;
    }

    void reset() { last_error_ = 0.0f; }

private:
    static constexpr float kAbsoluteDeadzonePx = 0.5f;  // 误差 < 0.5px 直接锁定（防 EMA 惯性）
    float alpha_ = 0.5f;          // 平滑系数（越大越跟手，越小越稳）
    float gain_ = 0.15f;          // 比例增益（误差 px → 移动 count）
    float max_move_ = 30.0f;      // 单帧最大移动（count）
    float deadzone_ratio_ = 0.05f; // 死区 = 框高 × ratio
    float last_error_ = 0.0f;     // 上一帧平滑后误差
};

}  // namespace ttbox::core::aim
