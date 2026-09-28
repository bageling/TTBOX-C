// AccuracySim.hpp — BB 命中率随机（照搬 BB 927 原版 applyAccuracySim）
//
// 小白理解：
//   真人不可能枪枪打在正中间。这个模块按概率把**瞄准点**从框中心推开一点
//   （推到框的四角或边缘），复现"偶尔打偏"的手感。
//
// ★ 全部照搬 BB 927 原版 main.lua:5274-5283，公式一字不改：
//     命中 rand(1,100) <= perfect_rate ⇒ 不动（完美命中）
//     否则取角度 a：
//       四角优先 = {45,135,225,315} 随机一个 + 随机 ±15°
//       边缘随机 = {0,90,180,270}   随机一个 + 随机 ±30°
//       全随机   = 0~360°
//     偏移 = 框半宽/半高 × strength × (cos a, sin a)
//
// ★ 原版口径，不要"顺手优化"：
//   1. 独立开关，不受 humanize.enabled 管（原版 :5275）。
//   2. 作用在**瞄准点**（选靶之后、进 PID 之前），不是在输出位移上加抖动
//      —— 原版 :6445 改的是 locked_target，PID 会老老实实往这个偏了的点瞄。
//      这条很关键：它是"瞄歪一点"，不是"手抖一下"。
//   3. 框尺寸为 0 时原版用 50 兜底。
//
// ★ 默认 enabled=false ⇒ 与本模块加入前逐字节一致。
#pragma once

#include <cmath>
#include <cstdint>

#include "mouse/MouseTypes.hpp"

namespace ttbox::core::aim {

class AccuracySim {
public:
    // tx/ty：瞄准点（px，就地偏移）；box_w/box_h：目标框尺寸（px）。
    void apply(float* tx, float* ty, float box_w, float box_h,
               const AccuracySimConfig& cfg) {
        if (!cfg.enabled) return;
        if (rand_percent() <= cfg.perfect_rate) return;   // 完美命中，不偏
        const float hw = (box_w > 0.0f ? box_w : 50.0f) * 0.5f;
        const float hh = (box_h > 0.0f ? box_h : 50.0f) * 0.5f;
        float a = 0.0f;
        const int k = static_cast<int>(rand_unit() * 4.0f) & 3;
        if (cfg.direction == 0) {              // 四角优先 ±15°
            constexpr float corners[4] = {45.0f, 135.0f, 225.0f, 315.0f};
            a = corners[k] + (rand_unit() * 30.0f - 15.0f);
        } else if (cfg.direction == 1) {       // 边缘随机 ±30°
            constexpr float edges[4] = {0.0f, 90.0f, 180.0f, 270.0f};
            a = edges[k] + (rand_unit() * 60.0f - 30.0f);
        } else {                                // 全随机 0~360°
            a = rand_unit() * 360.0f;
        }
        const float r = a * 3.14159265f / 180.0f;
        *tx += std::cos(r) * hw * cfg.offset_strength;
        *ty += std::sin(r) * hh * cfg.offset_strength;
    }

    void reset() { rng_ = kSeed; }

private:
    float rand_unit() {
        rng_ ^= rng_ << 13;
        rng_ ^= rng_ >> 17;
        rng_ ^= rng_ << 5;
        return static_cast<float>(rng_ & 0xFFFFFFu) / 16777216.0f;
    }
    // BB 原式是 math.random(1,100)（整数 1..100）；这里映射到 [1,100] 实数，
    // 与"<= perfect_rate" 的比较在整数 perfect_rate 下判定完全一致。
    float rand_percent() { return 1.0f + rand_unit() * 99.0f; }

    static constexpr uint32_t kSeed = 0x9E3779B9u;
    uint32_t rng_ = kSeed;
};

}  // namespace ttbox::core::aim
