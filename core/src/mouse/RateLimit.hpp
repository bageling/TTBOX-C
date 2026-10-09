// RateLimit.hpp — A10 速率限制 + HID 拆包
//
// 最终输出 clamp ±127（int8 HID report）。一次 AI 移动超过范围时拆成多个
// report：本 step 输出最多 ±limit，剩余量累积 pending，下次继续输出。
// X/Y 独立限制。
#pragma once

#include <cstdint>

namespace ttbox::core::aim {

struct RateLimitStep {
    int16_t dx = 0;  // 本次输出（|dx| ≤ limit）
    int16_t dy = 0;
    int32_t pending_dx = 0;  // 未消费溢出（下次 step 继续）
    int32_t pending_dy = 0;
    bool saturated = false;  // 本次是否发生 clamp
};

class RateLimiter {
public:
    // in_dx/in_dy：待输出的移动量（可超 ±limit）。limit 默认 127（int8 HID）。
    RateLimitStep step(int32_t in_dx, int32_t in_dy, int16_t limit = 127) {
        RateLimitStep r;
        const int32_t total_dx = pending_dx_ + in_dx;
        const int32_t total_dy = pending_dy_ + in_dy;
        if (total_dx > limit) { r.dx = limit; r.saturated = true; }
        else if (total_dx < -limit) { r.dx = -limit; r.saturated = true; }
        else { r.dx = static_cast<int16_t>(total_dx); }
        if (total_dy > limit) { r.dy = limit; r.saturated = true; }
        else if (total_dy < -limit) { r.dy = -limit; r.saturated = true; }
        else { r.dy = static_cast<int16_t>(total_dy); }
        pending_dx_ = total_dx - r.dx;
        pending_dy_ = total_dy - r.dy;
        r.pending_dx = pending_dx_;
        r.pending_dy = pending_dy_;
        return r;
    }
    // 清空未消费的溢出量（重置限速器状态）。
    void reset() { pending_dx_ = 0; pending_dy_ = 0; }

private:
    int32_t pending_dx_ = 0;
    int32_t pending_dy_ = 0;
};

}  // namespace ttbox::core::aim
