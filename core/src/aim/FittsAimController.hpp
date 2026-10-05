// FittsAimController.hpp — Fitts 定律瞄准控制器（V1.0.43，治「追着怪/停不住」）
//
// 来源：dat58/aimbot（Rust）的 Fitts 定律时间模型 —— 用人手移动的科学模型
// （Fitts's Law）替代 PID。核心思想：移动耗时 MT 由「距离/目标宽度」的难度指数决定，
// 天然近慢远快（远处快拉、近处精调），且只追误差本身、不追误差变化率 ——
// 这正是 pid1「前馈主导、对变化率敏感 → 追着怪」的解药。
//
// 与 pid1 的本质区别：
//   pid1：输出 ∝ 误差变化率（Ki 前馈主导）→ 目标一加速就猛追、一减速就衰减 ⇒ 追着怪。
//   Fitts：输出 ∝ 误差 / MT(误差) → 速度由「当前离目标多远」决定，与目标怎么动无关 ⇒ 稳。
//
// 公式（dat58 实现 + 板端仿真定案）：
//   死区 min_zone = max(deadzone_px, 框高 × deadzone_box_ratio)   （远处精确、近处不抖）
//   难度指数 ID = log2(2 × |error| / W + 1)，W = 2 × min_zone
//   移动耗时 MT = clamp(a_ms + b_ms × ID, kMinMs, kMaxMs)
//   本帧移动(px) = error × (kFrameDtMs / MT)
//   输出(count) = 本帧移动(px) / gain_px_per_count                 （与 pid1 的 u 同域）
//
// 板端仿真定案（gain=0.65 + 51ms + 噪声±2px，见 pid_sim 扫描）：
//   A=20 / B=20 最优 —— 不规则目标（速度随机游走）翻转 4（pid1 是 14，少 71%）、
//   输出波动 σ=0.9（pid1 是 1.1）、匀速/静态/急停全翻转 0（停得住）。
//   dat58 默认 A=35/B=55 在板端太慢（跟不上），已实测调小到 20/20。
#pragma once

#include <algorithm>
#include <cmath>

namespace ttbox::core::aim {

class FittsAimController {
public:
    // 参数（默认 = 板端仿真定案 A=20 / B=20 / deadzone_px=3 / ff_gain=0.6）
    void configure(float a_ms, float b_ms, float deadzone_px, float deadzone_box_ratio = 0.05f,
                   float ff_gain = 0.6f) {
        if (a_ms > 0.0f) a_ms_ = a_ms;
        if (b_ms > 0.0f) b_ms_ = b_ms;
        if (deadzone_px > 0.0f) deadzone_px_ = deadzone_px;
        if (deadzone_box_ratio > 0.0f) deadzone_box_ratio_ = deadzone_box_ratio;
        if (ff_gain >= 0.0f) ff_gain_ = ff_gain;
    }

    // 单轴更新。
    //   error = 像素误差（瞄准点 − 准星，px）
    //   box_h = 目标框高（px；尺寸自适应死区用，>0）
    //   gain_px_per_count = 标定增益（px/count；px → count 换算用，<=0 时用兜底 0.65）
    //   vel_px_s = 目标速度估计（px/s，AimTracker 的 vx/vy；0 = 静止/未知）
    //   dt_ms = **本帧实测间隔**（毫秒）。Fitts 是时间模型（MT 以 ms 计），
    //          dt 必须用实测值：写死 7.5 会在帧率抖动时系统性失配（4ms 时输出只 53%、
    //          12ms 时 160% ⇒ 表现为「跟不上」或「过冲/停不住」）。
    //          传 <=0 或超范围时按 kDefaultDtMs / 钳到 [kMinDtMs, kMaxDtMs] 兜底。
    // 返回本帧鼠标移动量（count 域，与 pid1 的 u 同域，直接进 AimThread 输出链）。
    float update(float error, float box_h, float gain_px_per_count, float vel_px_s, float dt_ms) {
        // 0. ★非有限输入一律当「无输出」：error/box_h/vel/NaN/Inf 任一为病态时，
        //    abs(NaN)<min_zone 恒假会穿过死区，log2(NaN)=NaN 一路泄漏到 HID 输出
        //    （int16 转换 NaN 是未定义行为 ⇒ 整条输出链被毁）。上游 tracker 偶发吐 NaN
        //    （除零/时间跳变）时必须在这里挡住，而不是指望上游永远干净。
        if (!std::isfinite(error) || !std::isfinite(box_h) ||
            !std::isfinite(vel_px_s)) {
            return 0.0f;
        }
        // 1. 尺寸自适应死区：max(绝对像素下限, 框高×比例)
        const float min_zone = box_h > 0.0f
            ? std::max(deadzone_px_, box_h * deadzone_box_ratio_)
            : deadzone_px_;
        if (std::abs(error) < min_zone) {
            return 0.0f;
        }
        // 2. Fitts 定律：难度指数 ID = log2(2·|error|/W + 1)
        const float W = 2.0f * min_zone;
        const float id = std::log2(2.0f * std::abs(error) / W + 1.0f);
        // 3. 移动耗时 MT = A + B·ID，钳 [kMinMs, kMaxMs]
        const float mt = std::clamp(a_ms_ + b_ms_ * id, kMinMs, kMaxMs);
        // 4. ★速度前馈（补 Fitts 模型的固有滞后）：
        //    Fitts 只对**误差**做比例响应，匀速移动目标必然滞后 ≈ 速度×MT
        //    （实测 err=100px 时 MT=108ms ⇒ 50px/s 目标隐含滞后 5.4px；叠加 51ms 回路
        //     延迟后总滞后约 110ms，表现为"追着慢"）。
        //    这里按「等效移动 MT 毫秒后的误差」把速度项加进控制量：
        //        control = error + ff_gain × vel × MT
        //    ff_gain 是欠补偿系数：0.6 = 仿真甜点（移动 200px/s 滞后 19.5→8.5px，降 56%，
        //    翻转仍为 0）；≥0.9 会因速度估计噪声被放大而开始抖（实测翻转 2~18）⇒ 上限钳 0.85。
        //    ★符号护栏：前馈**只能加速同向收敛，不能把输出推到反方向**。
        //      目标急速反向（|vel×MT| > |err|）时纯线性叠加会让输出反向 ⇒ 准星往回跑，
        //      比"跟慢一点"更糟（实测 err=+50px/vel=-2000px/s 时原始叠加输出 -6.9）。
        //      ⇒ 反向前馈**保留原符号、只钳幅度**到 |err| 以内：效果是「减速」而非「加速」。
        //      ⚠ 曾经的 bug：用 copysign(..., error) 把反向前馈翻成了同号（输出反而变大，
        //        err=+100/vel=-200 时输出 11.28 > 无前馈 9.99）。正确做法是钳 |ff| 不动符号。
        const float dt = (dt_ms > 0.0f) ? std::clamp(dt_ms, kMinDtMs, kMaxDtMs) : kDefaultDtMs;
        float ff = std::min(ff_gain_, 0.85f) * vel_px_s * (mt * 0.001f);
        if ((error > 0.0f && ff < 0.0f) || (error < 0.0f && ff > 0.0f)) {
            // 反向前馈：钳幅度到 |err| 以内（保持负号 ⇒ 输出减小但绝不反向）
            const float capped = std::min(std::fabs(ff), std::fabs(error));
            ff = (ff < 0.0f) ? -capped : capped;
        }
        // 5. 本帧移动（px）= 控制量 × (本帧实测间隔 / MT)
        const float move_px = (error + ff) * (dt / mt);
        // 6. px → count（标定增益；gain 缺失时兜底 0.65，避免除零）
        const float g = gain_px_per_count > 1e-4f ? gain_px_per_count : 0.65f;
        const float out = move_px / g;
        // 7. 输出再兜一层：结果非有限（增益病态等）时归零，绝不把 NaN/Inf 交给下游。
        return std::isfinite(out) ? out : 0.0f;
    }

    void reset() {}  // Fitts 无内部状态（每帧纯函数，无积分/无惯性）

private:
    static constexpr float kMinMs = 20.0f;      // MT 下限（最小移动耗时）
    static constexpr float kMaxMs = 320.0f;     // MT 上限（最大移动耗时）
    static constexpr float kDefaultDtMs = 6.9f; // dt 缺失兜底（144fps ≈ 6.9ms）
    static constexpr float kMinDtMs = 1.0f;     // dt 下限（防 0 除/防病态小值放大）
    static constexpr float kMaxDtMs = 40.0f;    // dt 上限（长卡顿/掉帧时不让一帧打飞）
    float a_ms_ = 20.0f;                        // Fitts 时间常数 A（基础反应时间）
    float b_ms_ = 20.0f;                        // Fitts 时间常数 B（难度系数）
    float deadzone_px_ = 3.0f;                  // 死区绝对像素下限
    float deadzone_box_ratio_ = 0.05f;          // 死区框高比例（死区 = max(px, 框高×比例)）
    float ff_gain_ = 0.6f;                      // 速度前馈欠补偿系数（仿真甜点；上限钳 0.85 防抖）
};

}  // namespace ttbox::core::aim
