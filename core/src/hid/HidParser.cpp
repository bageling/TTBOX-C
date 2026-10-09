// HidParser.cpp — A9 HID 解析与坐标转换实现
#include "hid/HidParser.hpp"

namespace ttbox::core {

// 解析鼠标 report：buttons/dx/dy/wheel/hwheel（不足 3 字节返回空状态）
MouseState parse_mouse_report(const HidReport& rep) {
    MouseState m;
    m.timestamp_us = rep.timestamp_us;
    if (rep.size < 3) return m;
    m.buttons = rep.data[0];
    m.dx = static_cast<int8_t>(rep.data[1]);
    m.dy = static_cast<int8_t>(rep.data[2]);
    if (rep.size >= 4) m.wheel = static_cast<int8_t>(rep.data[3]);
    if (rep.size >= 5) m.hwheel = static_cast<int8_t>(rep.data[4]);
    return m;
}

// 解析键盘 boot report：modifier + 6 keys（不足 8 字节返回空状态）
KeyboardState parse_keyboard_report(const HidReport& rep) {
    KeyboardState k;
    k.timestamp_us = rep.timestamp_us;
    if (rep.size < 8) return k;
    k.modifiers = rep.data[0];
    for (size_t i = 0; i < 6; ++i) k.keys[i] = rep.data[2 + i];
    return k;
}

// ---------------------------------------------------------------------------
// CoordinateTransform
// ---------------------------------------------------------------------------

// 屏幕坐标 → Capture ROI 坐标（平移裁剪；越界返回 false）
bool CoordinateTransform::screen_to_roi(float sx, float sy,
                                        uint32_t roi_x, uint32_t roi_y,
                                        uint32_t roi_w, uint32_t roi_h,
                                        float* ox, float* oy) {
    if (roi_w == 0 || roi_h == 0) return false;
    if (sx < static_cast<float>(roi_x) || sy < static_cast<float>(roi_y)) return false;
    const float rx = sx - static_cast<float>(roi_x);
    const float ry = sy - static_cast<float>(roi_y);
    if (rx >= static_cast<float>(roi_w) || ry >= static_cast<float>(roi_h)) return false;
    *ox = rx;
    *oy = ry;
    return true;
}

// ROI 坐标 → 模型输入坐标（等比缩放）
bool CoordinateTransform::roi_to_model(float rx, float ry,
                                       uint32_t roi_w, uint32_t roi_h,
                                       uint32_t model_w, uint32_t model_h,
                                       float* ox, float* oy) {
    if (roi_w == 0 || roi_h == 0 || model_w == 0 || model_h == 0) return false;
    *ox = rx * static_cast<float>(model_w) / static_cast<float>(roi_w);
    *oy = ry * static_cast<float>(model_h) / static_cast<float>(roi_h);
    return true;
}

// 模型输入坐标 → 检测坐标（缩放 + ROI 偏移）
bool CoordinateTransform::model_to_detection(float mx, float my,
                                             uint32_t model_w, uint32_t model_h,
                                             uint32_t roi_x, uint32_t roi_y,
                                             uint32_t roi_w, uint32_t roi_h,
                                             float* ox, float* oy) {
    if (model_w == 0 || model_h == 0) return false;
    if (roi_w == 0 || roi_h == 0) {
        *ox = mx;
        *oy = my;
        return true;
    }
    *ox = mx * static_cast<float>(roi_w) / static_cast<float>(model_w) + static_cast<float>(roi_x);
    *oy = my * static_cast<float>(roi_h) / static_cast<float>(model_h) + static_cast<float>(roi_y);
    return true;
}

// 屏幕坐标 → 检测坐标（Detection 坐标系即原图屏幕坐标系，恒等映射）
bool CoordinateTransform::screen_to_detection(float sx, float sy,
                                              uint32_t roi_x, uint32_t roi_y,
                                              uint32_t roi_w, uint32_t roi_h,
                                              uint32_t model_w, uint32_t model_h,
                                              float* ox, float* oy) {
    // Detection 坐标系 = 原图（屏幕）坐标系：屏幕坐标即检测坐标（恒等）。
    // ROI/模型参数仅用于语义一致性（确认调用方明确坐标系），不参与变换。
    (void)roi_x; (void)roi_y; (void)roi_w; (void)roi_h;
    (void)model_w; (void)model_h;
    *ox = sx;
    *oy = sy;
    return true;
}

}  // namespace ttbox::core
