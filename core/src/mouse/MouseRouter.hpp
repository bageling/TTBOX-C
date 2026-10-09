// MouseRouter.hpp — A10 物理鼠标报告解析（路由到 PhysicalMotion）
//
// 与 hid/HidParser::parse_mouse_report 不同：支持 ReportID + int16 轴布局
// （罗技 c53f 鼠标 input1：ReportID 0x02 + buttons(2B) + X(2B) + Y(2B) + wheel + pan）。
// 布局由参数指定，不硬编码。
#pragma once

#include <cstdint>

#include "mouse/MouseTypes.hpp"

namespace ttbox::core::aim {

struct MouseLayout {
    uint8_t report_id = 0;      // 期望的 ReportID（0 = 无 ReportID）
    uint16_t buttons_offset = 1;  // buttons 字节偏移
    uint8_t buttons_size = 2;     // buttons 字节数（1 或 2）
    uint16_t axis_offset = 3;     // X 轴字节偏移
    uint8_t axis_size = 2;        // 轴字节数（1=int8 / 2=int16 LE）
    uint8_t wheel_offset = 7;     // wheel 字节偏移（255=无）
    bool wheel_signed = true;     // wheel 有符号
};

class MouseRouter {
public:
    // 解析原始报告为 PhysicalMotion。布局不匹配（ReportID 不符/长度不足）→ 返回 false。
    bool parse(const uint8_t* data, size_t size, uint64_t timestamp_us,
               const MouseLayout& layout, PhysicalMotion* out) const;
};

}  // namespace ttbox::core::aim
