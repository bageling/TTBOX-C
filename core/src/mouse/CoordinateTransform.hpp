// CoordinateTransform.hpp — A10 坐标变换（ROI/crop 坐标系）
//
// 输入：DetectionResult(DetectionBox, crop 系) + AimPointProfile + ROI 尺寸
// 输出：pixel_error_x / pixel_error_y（像素）
//
// 全部在 ROI/crop 坐标系计算，不依赖固定 320，支持 192/256/320/416/640。
// ROI 尺寸改变时调用方传入新 roi_w/roi_h 即自动重算。
#pragma once

#include <cstdint>

#include "common/Types.hpp"
#include "mouse/MouseTypes.hpp"

namespace ttbox::core::aim {

// CoordinateTransform — 坐标变换：把目标框位置换算成"与准星的像素偏差"。
// 输入：检测框(DetectionBox, crop 系) + 瞄准点配置(AimPointProfile) + ROI 尺寸
// 输出：pixel_error_x / pixel_error_y（正值 = 目标在准星右/下方）
// 谁用：AimThread（瞄准线程）每帧调用，计算控制输入
struct CoordinateTransform {
    // 像素误差 = 瞄准点(target_point) - 瞄准参考点(roi 中心 + aim_offset)。
    //   box：目标框（crop 系）；class_id：类别
    //   prof：aim point 配置（含 aim_offset_x/y，crop 系 px）
    //   roi_w/roi_h：当前 ROI/crop 尺寸
    // 输出 err_x/err_y（像素，正值 = 目标在准星右/下方）。
    static bool pixel_error(const DetectionBox& box, int class_id,
                            const AimPointProfile& prof,
                            float roi_w, float roi_h,
                            float* err_x, float* err_y);

    // 瞄准参考点（准星）：roi 中心 + aim_offset（crop 系 px）。
    static void reference_point(float roi_w, float roi_h, const AimPointProfile& prof,
                                float* rx, float* ry);
};

}  // namespace ttbox::core::aim
