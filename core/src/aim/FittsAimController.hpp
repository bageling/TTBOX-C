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
    // 参数（默认 = 闭环仿真定案 A=20 / B=20 / dz=3 / ratio=0.02 / ff_gain=0.85）
    // ★ ratio=0.02 不是拍脑袋：闭环实测（板端实测 gain=0.686/51ms/噪声±2px，box_h=100）
    //   ratio=0.05 ⇒ 死区被放大到 5px ⇒ 静止稳态误差 0.47→2.96px（6.3×）、急跑抖动 10→24 次。
    //   ratio ≤0.03（死区仍由 dz=3px 主导）三项全最优 ⇒ 定 0.02。
    void configure(float a_ms, float b_ms, float deadzone_px, float deadzone_box_ratio = 0.02f,
                   float ff_gain = 0.85f, float ff_tau_ms = 51.0f) {
        if (a_ms > 0.0f) a_ms_ = a_ms;
        if (b_ms > 0.0f) b_ms_ = b_ms;
        if (deadzone_px > 0.0f) deadzone_px_ = deadzone_px;
        // ★ 守卫用 >= 0：ratio=0（只要绝对像素死区）是合法配置，
        //   `> 0.0` 会让属性根本没被赋值 ⇒ 首次访问是未定义值。
        if (deadzone_box_ratio >= 0.0f) deadzone_box_ratio_ = deadzone_box_ratio;
        if (ff_gain >= 0.0f) ff_gain_ = ff_gain;
        if (ff_tau_ms > 0.0f) ff_tau_ms_ = ff_tau_ms;
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
        // 4. ★速度前馈（补「移动目标落后」）：
        //    回路里目标在动时，准星要追上就得提前往他要去的方向走。
        //    补偿量 = 目标速度 × **前馈时延 τ**（该走多远 = 速度 × 滞后时间）。
        //    ★ τ 必须是**固定物理量**（回路延迟 response_delay_ms，板端实测 51ms），
        //      **不能拿 MT 代替**。MT = A + B·ID 是误差的函数（误差越大 MT 越长），
        //      用 MT 当 τ 会形成正反馈：越跟不上 → MT 越长 → 前馈越猛 → 过冲 → 误差更大。
        //      闭环实测（板端实测 gain=0.686/51ms/噪声±2px）两种写法天差地别：
        //        τ=MT  ：150px/s 滞后 7.08→13.99px、300px/s 19.81→39.95px（**恶化一倍**）
        //        τ=51ms：150px/s 9.41→5.57px、  300px/s 25.93→9.36px（**降 64%**）
        //    ff_gain 是欠补偿系数（0.85 = 仿真甜点，实测继续加大收益已饱和）。
        //    ★符号护栏：前馈**不能把输出推到反方向**（准星往回跑比跟慢一点更糟）。
        const float dt = (dt_ms > 0.0f) ? std::clamp(dt_ms, kMinDtMs, kMaxDtMs) : kDefaultDtMs;
        float ff = std::min(ff_gain_, kFfHardCap) * vel_px_s * (ff_tau_ms_ * 0.001f);
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
    static constexpr float kFfHardCap = 0.85f;  // ff_gain 硬钳（闭环实测收益在此饱和）
    float a_ms_ = 20.0f;                        // Fitts 时间常数 A（基础反应时间）
    float b_ms_ = 20.0f;                        // Fitts 时间常数 B（难度系数）
    float deadzone_px_ = 3.0f;                  // 死区绝对像素下限
    float deadzone_box_ratio_ = 0.02f;          // 死区框高比例（闭环实测定案，见 configure 注释）
    float ff_gain_ = 0.85f;                     // 速度前馈欠补偿系数（闭环实测甜点）
    // ★ 前馈时延 τ：补偿的是**回路延迟**（物理常量），不是 MT。
    //   取 51ms = 板端实测 response_delay_ms（AimThread 用它算抖动前馈扣除）。
    float ff_tau_ms_ = 51.0f;
};

}  // namespace ttbox::core::aim
