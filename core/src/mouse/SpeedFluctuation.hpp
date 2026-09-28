// SpeedFluctuation.hpp — BB 移动速度波动（照搬 BB 927 原版 applySpeedFluctuation）
//
// 小白理解：
//   真人把手移向目标时不是匀速的：起步慢、中途快、快到了收力。这个模块就是给
//   每帧的位移乘一个"速度倍率"，把匀速输出改成先慢后快再慢。
//
// ★ 全部照搬 BB 927 原版 main.lua:5265-5272，公式与边界一字不改：
//     p  = 1 - dtt/total_distance            （0~1 夹紧）
//     起步段 p < accel_ratio   ⇒ sf = start_speed + (1-start_speed) × (p/accel_ratio)
//     收尾段 p > 1-decel_ratio ⇒ sf = 1 - (1-start_speed) × ((p-(1-decel_ratio))/decel_ratio)
//     其余 sf = 1
//     随机 sf *= 1 + (rand-0.5)×2×intensity
//
// ★ 原版口径，不要"顺手优化"：
//   1. 独立开关，不受 humanize.enabled 管（原版 :5266）。
//   2. 只在新锁定目标的第一帧生效一次（原版 :5943 的一次性标志 first_lock）。
//      这是原版行为 —— 看着像 bug，但照搬就照搬，改了就不是 BB 的手感了。
//   3. td（全程参考距离）原版用 sqrt(Centre²+Centre²)，Centre=瞄准范围半径。
//
// ★ 默认 enabled=false ⇒ 与本模块加入前逐字节一致。
#pragma once

#include <cmath>

#include "mouse/MouseTypes.hpp"

namespace ttbox::core::aim {

class SpeedFluctuation {
public:
    // mx/my：本帧位移（count 域，就地乘倍率）；dtt：准星到目标距离（px）；
    // first_lock：是否"刚锁定目标的第一帧"（false ⇒ 原样返回，与 BB 一致）。
    void apply(float* mx, float* my, float dtt, bool first_lock,
               const SpeedFluctuationConfig& cfg) {
        if (!cfg.enabled || !first_lock) return;
        const float td = cfg.total_distance_px;
        if (!(td > 0.0f)) return;
        float p = 1.0f - dtt / td;
        if (p < 0.0f) p = 0.0f;
        if (p > 1.0f) p = 1.0f;
        float sf = 1.0f;
        if (cfg.accel_ratio > 0.0f && p < cfg.accel_ratio) {
            sf = cfg.start_speed + (1.0f - cfg.start_speed) * (p / cfg.accel_ratio);
        } else if (cfg.decel_ratio > 0.0f && p > (1.0f - cfg.decel_ratio)) {
            sf = 1.0f - (1.0f - cfg.start_speed) *
                            ((p - (1.0f - cfg.decel_ratio)) / cfg.decel_ratio);
        }
        if (cfg.intensity > 0.0f) {
            sf *= 1.0f + (rand_unit() - 0.5f) * 2.0f * cfg.intensity;
        }
        *mx *= sf;
        *my *= sf;
    }

    void reset() { rng_ = kSeed; }

private:
    // xorshift32：自带种子 ⇒ 不依赖全局 rand，行为可复现（与其他模块同款）。
    float rand_unit() {
        rng_ ^= rng_ << 13;
        rng_ ^= rng_ >> 17;
        rng_ ^= rng_ << 5;
        return static_cast<float>(rng_ & 0xFFFFFFu) / 16777216.0f;
    }

    static constexpr uint32_t kSeed = 0x2545F491u;
    uint32_t rng_ = kSeed;
};

}  // namespace ttbox::core::aim
