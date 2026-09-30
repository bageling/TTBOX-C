// JitterFeedforward.hpp — V3 阶段 5：拟人化抖动的前馈扣除（延迟对齐环形缓冲）
//
// 小白理解：
//   拟人化给鼠标加的"手抖"，过一会儿会真实出现在采集到的画面里。
//   PID 不知道那是自己抖的，会当成"目标移动了"去追，结果手抖被自己抹平、
//   闭环里还多出一串多余的修正。前馈扣除就是：记下"我刚才抖了多少"，
//   等它真的落到画面上的那一刻，从误差里把它加回去 ⇒ PID 看不见自己的抖动。
//
// 为什么必须是"延迟对齐"而不是"下一帧就扣"：
//   注入的 count 要 response_delay_ms（板端实测 51ms ≈ 7.3 帧 @144fps）之后
//   才落到画面上。提前扣 ⇒ 前馈自己变成高频扰动，比不扣更抖。
//
// 为什么按 dt 累计而不是固定帧数：
//   帧率会漂（板端 130~150），按帧数对齐会累积错位。dt 是实测帧间隔，对齐准。
//
// 用法（AimThread 每帧）：
//   ① 帧首先 advance(dt_ms, delay_ms, &jx, &jy) —— 取出本帧到期的抖动（count）
//   ② 乘 px/count 换算成像素，**加回** control_x / control_y（PID 之前）
//   ③ 帧尾（拟人化链跑完之后）push(本帧注入的抖动 count)
//   ※ 顺序不能反：本帧 push 的样本下一帧才开始计时（早一帧就扣 = 提前扣）。
//
// ★ 纯数据结构、无随机、无时间源 ⇒ 可单测、可复现。
#pragma once

#include <cmath>
#include <cstddef>

namespace ttbox::core::aim {

class JitterFeedforward {
public:
    static constexpr std::size_t kCapacity = 64;   // 51ms @144fps ≈ 7.3 帧，64 帧余量充足

    void reset() {
        head_ = 0;
        size_ = 0;
        dropped_ = 0;
        total_released_ = 0;
        for (std::size_t i = 0; i < kCapacity; ++i) {
            buf_[i].jx = 0.0f;
            buf_[i].jy = 0.0f;
            buf_[i].age_ms = 0.0f;
        }
    }

    // 帧首：把所有在队样本推进 dt_ms，释放已到期的（累加到 out_x/out_y，单位 count）。
    // delay_ms <= 0 ⇒ 立即释放（等价"同一帧就扣"，仅调试用，正常应传实测 delay）。
    void advance(float dt_ms, float delay_ms, float* out_x, float* out_y) {
        if (out_x) *out_x = 0.0f;
        if (out_y) *out_y = 0.0f;
        const float dt = (std::isfinite(dt_ms) && dt_ms > 0.0f) ? dt_ms : 0.0f;
        const float delay = (std::isfinite(delay_ms) && delay_ms > 0.0f) ? delay_ms : 0.0f;
        float sx = 0.0f;
        float sy = 0.0f;
        for (std::size_t n = 0; n < size_; ++n) {
            Sample& s = buf_[(head_ + n) % kCapacity];
            s.age_ms += dt;
        }
        while (size_ > 0 && buf_[head_].age_ms >= delay) {
            sx += buf_[head_].jx;
            sy += buf_[head_].jy;
            buf_[head_].jx = 0.0f;
            buf_[head_].jy = 0.0f;
            buf_[head_].age_ms = 0.0f;
            head_ = (head_ + 1) % kCapacity;
            --size_;
            ++total_released_;
        }
        if (out_x) *out_x = sx;
        if (out_y) *out_y = sy;
    }

    // 帧尾：登记本帧注入的抖动（count 域，与 move_x/move_y 同量纲）。
    void push(float jx_counts, float jy_counts) {
        const float x = std::isfinite(jx_counts) ? jx_counts : 0.0f;
        const float y = std::isfinite(jy_counts) ? jy_counts : 0.0f;
        if (x == 0.0f && y == 0.0f) return;   // 零抖动不占位（省得缓冲被空样本挤满）
        if (size_ == kCapacity) {             // 满了：丢最老的一条（最老的最不可能再用上）
            head_ = (head_ + 1) % kCapacity;
            --size_;
            ++dropped_;
        }
        const std::size_t w = (head_ + size_) % kCapacity;
        buf_[w].jx = x;
        buf_[w].jy = y;
        buf_[w].age_ms = 0.0f;
        ++size_;
    }

    std::size_t pending() const { return size_; }
    std::size_t dropped() const { return dropped_; }
    std::size_t released() const { return total_released_; }

private:
    struct Sample {
        float jx = 0.0f;
        float jy = 0.0f;
        float age_ms = 0.0f;
    };

    Sample buf_[kCapacity];
    std::size_t head_ = 0;
    std::size_t size_ = 0;
    std::size_t dropped_ = 0;
    std::size_t total_released_ = 0;
};

}  // namespace ttbox::core::aim
