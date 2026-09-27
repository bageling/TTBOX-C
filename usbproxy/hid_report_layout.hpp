// hid_report_layout.hpp — 从 HID 报告描述符里解析出「字段到底在第几个字节」
//
// 为什么必须有它（2026-09-24 现场事故）：
//   mouse_control 原先的 report_id / x_offset / y_offset / report_len 是**写死的
//   罗技 c53f 常量**（rid=2、buttons@1..2、X@3..4、Y@5..6、len=9），全仓没有任何代码
//   从物理鼠标的 report descriptor 里解。1.5.44 给长度加了自适应（9→实测长度），
//   但**偏移没自适应** ⇒ 对一只 X@1..2 / Y@3..4 / wheel@5 的鼠标：
//     · 写 X 时落到真实的 Y 上；
//     · 写 Y 时落到真实的 **wheel** 上 ⇒ 主机每帧收到一个滚轮事件 ⇒ 游戏疯狂切枪；
//     · 读按钮掩码时把 X 位移当成了按键位 ⇒ mask 恒被污染（现场实测 0xff），
//       自瞄热键闸门形同虚设。
//   所以修法不是"再猜一个偏移"，而是**解析描述符，按真实布局读写**；
//   解析不出来就一个字都不写（宁可自瞄不生效，也绝不乱动鼠标）。
//
// 术语：
//   · bit_offset 一律相对**报告数据起点**（若该报告以 report_id 打头，则指它之后的那一字节）。
//   · report_id = 0 表示该报告没有 report_id 前缀（boot 式单报告）。
#pragma once

#include <cstddef>
#include <cstdint>

namespace ttbox_usbproxy {

// 一个字段在报告里的位置
struct HidField {
    bool present = false;
    int bit_offset = -1;      // 相对报告数据起点的位偏移；-1 = 未找到
    int bit_size = 0;         // 位宽（buttons 可能是 3/5/8 位，X/Y 通常是 8 或 16）
    bool is_signed = false;   // 逻辑最小值 < 0（相对位移必为 signed）
};

// 单个 report_id 的布局
struct HidReportLayout {
    bool valid = false;       // 该 report_id 至少有一个 Input 项
    int report_id = 0;        // 0 = 该报告没有 report_id 前缀
    int total_bits = 0;       // 该报告所有 Input 项占用的总位宽
    HidField buttons;         // Button Page 的位掩码区
    HidField x;
    HidField y;
    HidField wheel;

    bool has_xy() const { return x.present && y.present; }
    bool has_buttons() const { return buttons.present && buttons.bit_size > 0; }
    int total_bytes() const { return (total_bits + 7) / 8; }
};

// 整份描述符的解析结果（按 report_id 索引，上限 15）
struct HidMouseDescriptor {
    static constexpr int kMaxReportId = 16;

    bool parsed = false;              // 描述符语法是否走通（false = 畸形，调用方按"不可用"处理）
    bool uses_report_ids = false;     // 出现过 Report ID 项 ⇒ 该设备的每份报告都以 1 字节 report_id 打头
    int layout_count = 0;             // 有效的 report_id 个数
    HidReportLayout layouts[kMaxReportId];

    const HidReportLayout* find(int report_id) const;
    // 第一份同时含 X/Y 的布局（= 位移报告）
    const HidReportLayout* xy_layout() const;
    // 第一份含 buttons 的布局（= 按键报告，可能和位移报告不是同一份 report_id）
    const HidReportLayout* button_layout() const;
    // 该描述符整体是否"有可用信息"（至少找到 X/Y 或 buttons 之一）
    bool usable() const { return parsed && (xy_layout() != nullptr || button_layout() != nullptr); }
};

// 解析 HID report descriptor（不含前导的 bLength/bDescriptorType，直接给描述符体）。
// 返回 false = 畸形/越界/没有任何我们认识的用法 ⇒ 调用方**必须**按不可用处理。
bool hid_parse_report_descriptor(const uint8_t* desc, size_t len, HidMouseDescriptor* out);

// 读取字段值（bit_size ∈ {8,12,16,32}；2026-09-27 起支持 12 位与非字节对齐 ——
// 罗技系游戏鼠标的 X/Y 常为 12 位且跨字节，旧实现直接拒 ⇒ 该鼠标注入/热键全废）。
// report/report_len 指向**报告数据起点**（若该报告有 report_id，调用方需自行 +1）。
bool hid_field_read_signed(const uint8_t* report, size_t report_len,
                           const HidField& f, int32_t* out);

// 写入字段值（位宽/越界拒写；写法 = 位窗口 RMW，同字节里相邻字段位原样保留）。
bool hid_field_write_signed(uint8_t* report, size_t report_len,
                            const HidField& f, int32_t value);

// 读取位掩码（buttons 用；1~16 位，支持非对齐 —— 位窗口读）。
bool hid_field_read_mask(const uint8_t* report, size_t report_len,
                         const HidField& f, uint32_t* out);

// 写入位掩码（buttons 用；只取 mask 的低 bit_size 位，字段外的位保持原样）。
// 越界、位宽不支持（>16）一律拒写并返回 false。
bool hid_field_write_mask(uint8_t* report, size_t report_len,
                          const HidField& f, uint32_t mask);

// 两个字段占用的**字节区间**是否重叠 —— 用于"写 X/Y 绝不能碰到 buttons/wheel"的最后一道闸。
bool hid_fields_overlap_bytes(const HidField& a, const HidField& b);

// 字段的位宽是否可安全读写（8/16/32 且字节对齐）
bool hid_field_is_safe(const HidField& f);

}  // namespace ttbox_usbproxy
