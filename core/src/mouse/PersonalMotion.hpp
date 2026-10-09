// PersonalMotion.hpp — TTBOX 个人移动曲线运行时采样器。
// 只消费已校验的归一化 knots；无效/关闭模型返回单位倍率，保持默认控制链。
#pragma once

#include <cstdint>
#include <vector>
#include "mouse/MouseTypes.hpp"

namespace ttbox::core::aim {

class PersonalMotion {
public:
    // 根据当前误差距离（像素）返回默认输出倍率与个人曲线的混合倍率。
    // knots 按误差距离归一化到 0~128 像素均匀采样。
    static float scale(float error_distance, const PersonalMotionConfig& config);

    // ★ 内置默认曲线（knots 为空时用它）：5 点均匀铺在 0~128px，
    //   形状取「近处收力修得细、远处加力拉得快」（0.75 → 1.25 单调）。
    //   为什么要有它：配置的 knots 从来没有面板编辑入口（vector 类型，表驱动不支持），
    //   默认就是空的 ⇒ 旧代码 valid() 一票否决、scale 恒 1.0 ⇒ 开关开了也是假开关。
    //   有这条兜底后开关才真的有意义；想自定义仍可手填 knots（填了就用你的）。
    static const std::vector<float>& default_knots();

private:
    static bool valid(const PersonalMotionConfig& config);
};

}  // namespace ttbox::core::aim
