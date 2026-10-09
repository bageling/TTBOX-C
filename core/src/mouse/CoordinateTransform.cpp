// CoordinateTransform.cpp — A10 坐标变换实现
#include "mouse/CoordinateTransform.hpp"

#include "mouse/AimPointProfile.hpp"

namespace ttbox::core::aim {

void CoordinateTransform::reference_point(float roi_w, float roi_h,
                                          const AimPointProfile& prof,
                                          float* rx, float* ry) {
    // V1.0.13（2026-09-30）：原先这里再加 prof.aim_offset_x/y（准星像素偏移），
    // 已按业主口径删除 —— 准星就是裁剪区正中心，落点只剩「瞄点」一个入口。
    (void)prof;
    *rx = roi_w * 0.5f;
    *ry = roi_h * 0.5f;
}

bool CoordinateTransform::pixel_error(const DetectionBox& box, int class_id,
                                      const AimPointProfile& prof,
                                      float roi_w, float roi_h,
                                      float* err_x, float* err_y) {
    if (roi_w <= 0.0f || roi_h <= 0.0f) return false;
    float tx = 0.0f, ty = 0.0f;
    if (!aim_point_at(box, class_id, prof, &tx, &ty)) return false;
    float rx = 0.0f, ry = 0.0f;
    reference_point(roi_w, roi_h, prof, &rx, &ry);
    *err_x = tx - rx;
    *err_y = ty - ry;
    return true;
}

}  // namespace ttbox::core::aim
