// OutputScale.hpp — A10 输出缩放
//
// 严格拆分：fov_range 只影响目标选择；output_scale 只影响最终 AI 移动量。
// 最终流程：P output → rate → sensitivity → output_scale（X/Y 独立）。
// 禁止把 FOV 乘到输出。
#pragma once

#include "mouse/MouseTypes.hpp"

namespace ttbox::core::aim {

inline float output_scale_x(float out_x, const MouseProfile& p) {
    return out_x * p.rate_x * p.sensitivity * p.output_scale;
}

inline float output_scale_y(float out_y, const MouseProfile& p) {
    return out_y * p.rate_y * p.sensitivity * p.output_scale;
}

}  // namespace ttbox::core::aim
