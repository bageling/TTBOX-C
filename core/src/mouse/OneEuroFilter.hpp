// OneEuroFilter.hpp — 单变量 One-Euro 低通滤波器（公共组件）
//
// 作用：滤除检测框/瞄准点的高频抖动（如模型头顶边界导致的 y1 帧间跳变）。
//       与 AimTracker 内置的位置平滑同参数同算法，本类供 AimThread 等
//       对「显示框」等其它信号做同样平滑时复用，避免各处重复实现。
//
// 小白理解：
//   - 目标不动时：把高频小抖动滤掉（画面/坐标稳定）
//   - 目标真在移动时：自动提高跟随速度，不产生明显拖影
//   - 每个变量各用自己一个滤波器（本类只管一个数值）
//
// 算法：Casiez 2009 One Euro Filter（业界标准低延迟平滑）
//   1. 先估计原始速度 dx（带一阶低通 d_cutoff，防噪声速度尖峰）
//   2. 截止频率 = min_cutoff + beta × |平滑速度|（速度越快截止越高，越跟手）
//   3. 位置一阶低通（截止随速度自适应）
//
// 注意：dt <= 0（首帧/时间跳跃）时直通返回原值并重建平滑状态，
//       避免停顿后恢复时产生长时间拖尾。

#pragma once

#include <cmath>

namespace ttbox::core::aim {

class OneEuroFilter {
public:
    OneEuroFilter() = default;

    // 参数（与 AimTracker 位置平滑常量一致）：
    //   min_cutoff_hz : 静止目标截止频率（0.8Hz：强平滑，压住 18px 级框抖动）
    //   beta          : 目标速度影响系数（0.10：移动目标仍低延迟跟随）
    //   d_cutoff_hz   : 速度估计截止频率（1.0Hz：滤噪声速度尖峰，防截止被抬高）
    OneEuroFilter(float min_cutoff_hz, float beta, float d_cutoff_hz)
        : min_cutoff_hz_(min_cutoff_hz), beta_(beta), d_cutoff_hz_(d_cutoff_hz) {}

    // 送入一个新观测值，返回平滑后的值。dt_s 为距上次观测的秒数。
    // dt_s <= 0（首帧/时间跳跃/无效时间）→ 直通并重建状态。
    float update(float x, float dt_s) {
        if (dt_s <= 0.0f || !valid_) {
            fx_ = x;
            fdx_ = 0.0f;
            valid_ = true;
            return x;
        }
        const float raw_dx = (x - fx_) / dt_s;
        const float ad = alpha(d_cutoff_hz_, dt_s);
        fdx_ += ad * (raw_dx - fdx_);          // 速度一阶低通（初值 0，压首帧尖峰）
        const float cutoff = min_cutoff_hz_ + beta_ * std::fabs(fdx_);
        const float ax = alpha(cutoff, dt_s);
        fx_ += ax * (x - fx_);                 // 位置一阶低通（截止随速度自适应）
        return fx_;
    }

    // 清空平滑状态（目标切换/退出时调用，下一次 update 直通重建）。
    void reset() {
        valid_ = false;
        fx_ = 0.0f;
        fdx_ = 0.0f;
    }

    // 平滑器是否已建立初值。
    bool valid() const { return valid_; }

    // 一阶低通系数：alpha = 1 / (1 + tau/dt)，tau = 1/(2π·cutoff)
    static float alpha(float cutoff_hz, float dt_s) {
        constexpr float kTwoPi = 6.283185307179586f;
        const float tau = 1.0f / (kTwoPi * cutoff_hz);
        return 1.0f / (1.0f + tau / dt_s);
    }

private:
    float min_cutoff_hz_ = 0.8f;
    float beta_ = 0.10f;
    float d_cutoff_hz_ = 1.0f;
    float fx_ = 0.0f;      // 平滑后的值
    float fdx_ = 0.0f;     // 平滑后的速度
    bool valid_ = false;   // 是否已建立初值
};

}  // namespace ttbox::core::aim
