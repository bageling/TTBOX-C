// MouseRouter.cpp — A10 物理鼠标报告解析实现
#include "mouse/MouseRouter.hpp"

namespace ttbox::core::aim {

bool MouseRouter::parse(const uint8_t* data, size_t size, uint64_t timestamp_us,
                        const MouseLayout& layout, PhysicalMotion* out) const {
    if (!data || !out) return false;
    // ReportID 校验（偏移字段均为"绝对偏移"，含 ReportID）
    if (layout.report_id != 0) {
        if (size < 1 || data[0] != layout.report_id) return false;
    }
    // ★ 轴要读 2 个分量（X、Y），每个 axis_size 字节 ⇒ 实际需 2*axis_size 字节。
    //   原判据只算了 1 个分量（2026-09-23 审查复核 #19）⇒ 长度落在
    //   [axis_offset+axis_size, axis_offset+2*axis_size) 时越界读 1~2 字节：
    //   默认 INT16 布局（axis_size=2, axis_offset=3）短 2 字节、INT8 布局短 1 字节。
    //   注：MouseRouter/MouseLayout 目前**全仓无生产调用方**（只有测试），属"接口已开放
    //   没人用"的那类，但越界读是 UB，先收紧；将来接调用方时不必再回来补。
    if (layout.axis_size != 1 && layout.axis_size != 2) return false;  // 只支持 int8 / int16 LE
    const size_t axis_need =
        static_cast<size_t>(layout.axis_offset) + 2u * static_cast<size_t>(layout.axis_size);
    if (size < axis_need) return false;
    // ★ buttons_size 上限校验（2026-10-06 补）。下面按字节拼 32 位再截成 uint16：
    //   buttons_size ≥ 3 时高位被静默截断（丢按键）；≥ 5 时 `<< (8u * i)` 的移位量
    //   达到 32，**移位量 ≥ 宽度是 UB**。两处都是"配置写错就出事"，入口直接拒掉。
    //   上限取 2：与 m.buttons 的 uint16_t 宽度对齐，多余字节本就装不下。
    if (layout.buttons_size > 2) return false;
    if (size < static_cast<size_t>(layout.buttons_offset) + layout.buttons_size) return false;

    PhysicalMotion m;
    m.timestamp_us = timestamp_us;
    // buttons（1B 或 2B LE）
    uint32_t btns = 0;
    for (uint8_t i = 0; i < layout.buttons_size; ++i) {
        btns |= static_cast<uint32_t>(data[layout.buttons_offset + i]) << (8u * i);
    }
    m.buttons = static_cast<uint16_t>(btns);
    // 轴（1B int8 或 2B int16 LE）
    const uint8_t* ap = data + layout.axis_offset;
    if (layout.axis_size == 1) {
        m.dx = static_cast<int8_t>(ap[0]);
        m.dy = static_cast<int8_t>(ap[1]);
    } else {
        const int16_t dx = static_cast<int16_t>(ap[0] | (static_cast<uint16_t>(ap[1]) << 8));
        const int16_t dy = static_cast<int16_t>(ap[2] | (static_cast<uint16_t>(ap[3]) << 8));
        m.dx = dx;
        m.dy = dy;
    }
    // wheel（可选）
    if (layout.wheel_offset != 255 && size > layout.wheel_offset) {
        const uint8_t w = data[layout.wheel_offset];
        m.wheel = layout.wheel_signed ? static_cast<int8_t>(w) : static_cast<int8_t>(w & 0x7F);
    }
    *out = m;
    return true;
}

}  // namespace ttbox::core::aim
