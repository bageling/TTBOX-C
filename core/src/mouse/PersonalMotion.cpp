// PersonalMotion.cpp — TTBOX 个人移动曲线确定性插值。
#include "mouse/PersonalMotion.hpp"

#include <algorithm>
#include <cmath>

namespace ttbox::core::aim {

const std::vector<float>& PersonalMotion::default_knots() {
    // 5 点：0px（贴脸）→ 128px（远距离）。1.0 = 不放大不缩小。
    static const std::vector<float> kKnots{0.75f, 0.88f, 1.0f, 1.12f, 1.25f};
    return kKnots;
}

bool PersonalMotion::valid(const PersonalMotionConfig& config) {
    if (!config.enabled || config.curve_blend < 0.0f || config.curve_blend > 1.0f) {
        return false;
    }
    for (const float knot : config.knots) {
        if (!std::isfinite(knot) || knot < 0.0f || knot > 1.0f) return false;
    }
    return true;
}

float PersonalMotion::scale(float error_distance, const PersonalMotionConfig& config) {
    if (!valid(config)) return 1.0f;
    // knots 空 ⇒ 用内置默认曲线（否则开关形同虚设，见 .hpp 的说明）
    const std::vector<float>& knots = config.knots.empty() ? default_knots() : config.knots;
    const float normalized = std::clamp(std::fabs(error_distance) / 128.0f, 0.0f, 1.0f);
    const float position = normalized * static_cast<float>(knots.size() - 1);
    const size_t left = static_cast<size_t>(position);
    const size_t right = std::min(left + 1, knots.size() - 1);
    const float fraction = position - static_cast<float>(left);
    const float personal = knots[left] + (knots[right] - knots[left]) * fraction;
    return 1.0f + (personal - 1.0f) * config.curve_blend;
}

}  // namespace ttbox::core::aim
