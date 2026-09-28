// AimTracker.hpp — 目标跟踪器（第15阶段深化）
//
// 职责：跟踪锁定目标的位置/速度，提供预测位置供 Controller 使用。
// 记录：target_id、上一帧位置、当前帧位置、时间差、vx/vy、prediction_time、predicted_x/y。
// Reset 触发：目标切换（target_id 变化）、LOST 超过 grace（外部调用 reset()）。
//
// 设计（TTBOX 已实现行为）：
//   - 速度 = 帧差位置/时间差，EMA 低通平滑降检测框抖动，clamp 防伪速度尖峰
//   - 预测 = 当前位置 + 速度 × prediction_time（线性外推）
//   - 不依赖 RKNN/V4L2/Mouse/HID，纯数学，可独立测试
#pragma once

#include <cstdint>

#include "mouse/MouseTypes.hpp"   // BoxAdaptiveFilterConfig（V3 阶段 3a）

namespace ttbox::core::aim {

struct TrackedTarget {
    bool valid = false;
    float x = 0.0f;          // 当前帧目标中心（crop 系 px）
    float y = 0.0f;
    float prev_x = 0.0f;     // 上一帧位置
    float prev_y = 0.0f;
    float vx = 0.0f;         // 像素/秒
    float vy = 0.0f;
    int target_id = -1;
    uint64_t timestamp_us = 0;   // 当前帧时间戳
    uint64_t dt_us = 0;          // 与上一帧时间差
    // V3 阶段 3a：目标框高的 **EMA**（慢变量）。驱动 One-Euro 的 min_cutoff。
    // 0 = 还没喂过（此时 cutoff 走既有常量，行为与加此机制前一致）。
    float box_h_ema = 0.0f;
    float prediction_time = 0.0f;    // 预测时域（秒，由调用方设置）
    float predicted_x = 0.0f;        // 预测位置
    float predicted_y = 0.0f;
};

class AimTracker {
public:
    // 每帧更新目标中心。target_id 变化或时间跳跃过大 → 速度清零（避免伪速度）。
    void update(float cx, float cy, int target_id, uint64_t now_us);
    // V3 阶段 3a：喂本帧目标框高（px），内部走 EMA。
    //   box_h <= 0（丢失/无框）⇒ **保持上一帧**，不清零 —— 否则目标一丢 cutoff 就跳变。
    void set_box_h(float box_h);
    // V3 阶段 3a：滤波自适应配置（每帧重读，改配置即时生效）。enabled=false ⇒ 走既有常量。
    void configure(const BoxAdaptiveFilterConfig& cfg) { box_cfg_ = cfg; }
    // 当前生效的位置截止频率（供测试/遥测核对：框越小 ⇒ 越低 ⇒ 滤得越狠）
    float min_cutoff_hz() const;
    // 预测位置：pos + vel × prediction_time（s），并存入 state
    void predict(float prediction_time_s, float* px, float* py);
    // 目标丢失/切换后重置
    void reset();
    const TrackedTarget& state() const { return state_; }
    // 目标切换判定：上次 update 的 target_id 与当前传入不同
    bool target_switched(int new_id) const {
        return state_.valid && state_.target_id != -1 && state_.target_id != new_id;
    }

private:
    static constexpr uint64_t kMaxDtUs = 200000;   // 200ms 视为目标切换/跳跃
    static constexpr float kVelEmaAlpha = 0.4f;    // 速度 EMA 系数（降抖动）
    static constexpr float kMaxVelPxPerSec = 2500.0f;  // 速度 clamp
    // OneEuro 位置平滑（滤检测框抖动，如人体框上边缘 y1 帧间跳变 ±18px）：
    //   - min_cutoff：目标静止（速度≈0）时截止频率压到 0.8Hz，高频框噪声被强衰减
    //   - beta：目标真实移动时截止频率随速度升高（低延迟跟随，稳态滞后 <1.5px@100px/s）
    //   - d_cutoff：速度信号本身的低通截止（防止噪声速度尖峰抬高位置截止）
    // 实测：18.2px 抖动脉冲 → 平滑后逐帧 step ≤3.5px（衰减 80%），静止无滞后
    static constexpr float kPosMinCutoffHz = 0.8f;
    static constexpr float kPosBeta = 0.10f;
    static constexpr float kPosDCutoffHz = 1.0f;
    // OneEuro 平滑器状态（不进 TrackedTarget，state() 只暴露平滑后位置）
    float pos_fx_ = 0.0f;      // 平滑后位置（低通输出）
    float pos_fy_ = 0.0f;
    float pos_fdx_ = 0.0f;     // 平滑后速度（低通输出）
    float pos_fdy_ = 0.0f;
    bool pos_valid_ = false;   // 平滑器是否已建立（目标切换/重置后清 false）
    // V3 阶段 3a：滤波自适应配置（默认 enabled=false ⇒ 完全走上面的常量）
    BoxAdaptiveFilterConfig box_cfg_;
    float box_h_ema_ = 0.0f;   // 框高 EMA（慢变量，驱动 min_cutoff）
    // 原始输入缓存（速度估计专用：raw_vx 必须用原始帧差，
    // 若用平滑后位置帧差会被双重平滑 → 速度收敛慢 → 预测超前不足）
    float raw_prev_x_ = 0.0f;
    float raw_prev_y_ = 0.0f;
    TrackedTarget state_;
};

}  // namespace ttbox::core::aim
