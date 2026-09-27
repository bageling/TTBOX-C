// hid_report_layout.cpp — HID 报告描述符解析实现
//
// 只做一件事：把描述符里每个 Input 项的「用法 → 位偏移/位宽」算出来。
// 不做任何"猜布局"的启发式 —— 解析不出来就是不可用，由调用方 fail-closed。
#include "hid_report_layout.hpp"

#include <cstring>

namespace ttbox_usbproxy {
namespace {

// HID 用法页 / 用法编号
constexpr uint32_t kPageGenericDesktop = 0x01;
constexpr uint32_t kPageButton = 0x09;
constexpr uint32_t kUsageX = 0x30;
constexpr uint32_t kUsageY = 0x31;
constexpr uint32_t kUsageWheel = 0x38;

// 一次 Input 项最多容纳的位数（防畸形描述符把游标推到天上去）
constexpr int kMaxFieldBits = 4096;

struct Usage {
    uint32_t page = 0;
    uint32_t id = 0;
};

struct ParserState {
    uint32_t usage_page = 0;
    int report_size = 0;
    int report_count = 0;
    int32_t logical_min = 0;
    int report_id = 0;

    static constexpr int kMaxUsages = 16;
    Usage usages[kMaxUsages];
    int usage_count = 0;

    bool has_usage_range = false;
    uint32_t usage_min = 0;
    uint32_t usage_max = 0;

    void clear_local() {
        usage_count = 0;
        has_usage_range = false;
        usage_min = 0;
        usage_max = 0;
    }
};

void assign_field(HidReportLayout& lay, uint32_t page, uint32_t usage,
                  int bit_off, int bits, bool is_signed) {
    if (bits <= 0) return;
    if (page == kPageButton) {
        if (!lay.buttons.present) {
            lay.buttons.present = true;
            lay.buttons.bit_offset = bit_off;
            lay.buttons.bit_size = bits;
            lay.buttons.is_signed = false;
        }
        return;
    }
    if (page != kPageGenericDesktop) return;
    switch (usage) {
        case kUsageX:
            if (!lay.x.present) { lay.x = HidField{true, bit_off, bits, is_signed}; }
            break;
        case kUsageY:
            if (!lay.y.present) { lay.y = HidField{true, bit_off, bits, is_signed}; }
            break;
        case kUsageWheel:
            if (!lay.wheel.present) { lay.wheel = HidField{true, bit_off, bits, is_signed}; }
            break;
        default:
            break;
    }
}

void set_buttons(HidReportLayout& lay, int bit_off, int bits) {
    if (bits <= 0) return;
    if (!lay.buttons.present) {
        lay.buttons.present = true;
        lay.buttons.bit_offset = bit_off;
        lay.buttons.bit_size = bits;
        lay.buttons.is_signed = false;
    }
}

// 处理一个 Input 项：把当期 globals/locals 落到布局上，并推进该 report_id 的位游标。
void handle_input(ParserState& st, uint32_t flags, int* bit_cursor, bool* seen_input,
                  HidMouseDescriptor* out) {
    const int size = st.report_size;
    const int count = st.report_count;
    if (size <= 0 || count <= 0 || size > kMaxFieldBits || count > kMaxFieldBits ||
        size * count > kMaxFieldBits) {
        st.clear_local();
        return;
    }
    const int rid = (st.report_id >= 0 && st.report_id < HidMouseDescriptor::kMaxReportId)
                        ? st.report_id
                        : 0;
    HidReportLayout& lay = out->layouts[rid];
    lay.valid = true;
    lay.report_id = rid;
    seen_input[rid] = true;

    const bool is_constant = (flags & 0x01) != 0;   // Constant = 填充位
    const bool is_variable = (flags & 0x02) != 0;   // Variable = 独立字段（Array 不是）
    const bool is_signed = st.logical_min < 0;

    const int base = bit_cursor[rid];

    if (!is_constant && is_variable) {
        if (st.usage_count >= 1 && st.usage_count >= count) {
            // ① 显式列出 count 个用法：按顺序每个占 size 位（X/Y/Wheel 典型写法）
            for (int k = 0; k < count; ++k) {
                assign_field(lay, st.usages[k].page, st.usages[k].id,
                             base + k * size, size, is_signed);
            }
        } else if (st.has_usage_range && st.usage_page == kPageButton) {
            // ② Usage Minimum/Maximum（按键典型写法：Min=1 Max=5 Count=5 Size=1）
            set_buttons(lay, base, size * count);
        } else if (st.usage_count == 1) {
            // ③ 单个用法配 count>1：整段都算这一个字段（极少见）
            assign_field(lay, st.usages[0].page, st.usages[0].id,
                         base, size * count, is_signed);
        } else if (st.usage_page == kPageButton) {
            // ④ 用法页是 Button 但既无列表也无范围的兜底写法
            set_buttons(lay, base, size * count);
        }
    }

    bit_cursor[rid] = base + size * count;
    st.clear_local();
}

}  // namespace

const HidReportLayout* HidMouseDescriptor::find(int report_id) const {
    if (report_id < 0 || report_id >= kMaxReportId) return nullptr;
    const HidReportLayout& lay = layouts[report_id];
    return lay.valid ? &lay : nullptr;
}

const HidReportLayout* HidMouseDescriptor::xy_layout() const {
    for (int r = 0; r < kMaxReportId; ++r) {
        if (layouts[r].valid && layouts[r].has_xy()) return &layouts[r];
    }
    return nullptr;
}

const HidReportLayout* HidMouseDescriptor::button_layout() const {
    for (int r = 0; r < kMaxReportId; ++r) {
        if (layouts[r].valid && layouts[r].has_buttons()) return &layouts[r];
    }
    return nullptr;
}

bool hid_parse_report_descriptor(const uint8_t* desc, size_t len, HidMouseDescriptor* out) {
    if (desc == nullptr || out == nullptr || len == 0) return false;
    *out = HidMouseDescriptor{};

    ParserState st;
    int bit_cursor[HidMouseDescriptor::kMaxReportId] = {0};
    bool seen_input[HidMouseDescriptor::kMaxReportId] = {false};

    size_t i = 0;
    while (i < len) {
        const uint8_t b = desc[i++];

        // Long Item：0xFE | bDataSize | bLongItemTag | data...
        if (b == 0xFE) {
            if (i + 2 > len) return false;
            const int dsize = desc[i];
            i += 2;
            if (i + static_cast<size_t>(dsize) > len) return false;
            i += static_cast<size_t>(dsize);
            continue;
        }

        int b_size = b & 0x03;
        if (b_size == 3) b_size = 4;
        const int b_type = (b >> 2) & 0x03;
        const int b_tag = (b >> 4) & 0x0F;

        if (i + static_cast<size_t>(b_size) > len) return false;

        uint32_t u = 0;
        for (int k = 0; k < b_size; ++k) {
            u |= static_cast<uint32_t>(desc[i + k]) << (8 * k);
        }
        i += static_cast<size_t>(b_size);

        int32_t s = 0;
        if (b_size > 0) {
            const uint32_t shift = 32u - 8u * static_cast<uint32_t>(b_size);
            s = static_cast<int32_t>(u << shift) >> shift;
        }

        if (b_type == 1) {  // Global
            switch (b_tag) {
                case 0x0: st.usage_page = u; break;
                case 0x1: st.logical_min = s; break;
                case 0x7: st.report_size = static_cast<int>(u); break;
                case 0x8:
                    st.report_id = static_cast<int>(u & 0xFF);
                    out->uses_report_ids = true;
                    break;
                case 0x9: st.report_count = static_cast<int>(u); break;
                default: break;  // Logical Max / Unit / Push / Pop 等：本解析用不到
            }
            continue;
        }

        if (b_type == 2) {  // Local
            if (b_tag == 0x0) {
                uint32_t page = st.usage_page;
                uint32_t id = u;
                if (b_size == 4) {  // 32 位 usage：高 16 位是 usage page
                    page = (u >> 16) & 0xFFFF;
                    id = u & 0xFFFF;
                }
                if (st.usage_count < ParserState::kMaxUsages) {
                    st.usages[st.usage_count].page = page;
                    st.usages[st.usage_count].id = id;
                    st.usage_count++;
                }
            } else if (b_tag == 0x1) {
                st.usage_min = u;
                st.has_usage_range = true;
            } else if (b_tag == 0x2) {
                st.usage_max = u;
                st.has_usage_range = true;
            }
            continue;
        }

        if (b_type == 0) {  // Main
            if (b_tag == 0x8) {
                handle_input(st, u, bit_cursor, seen_input, out);
            } else if (b_tag == 0xA || b_tag == 0xC) {  // Collection / End Collection
                st.clear_local();
            }
            // Output(0x9) / Feature(0xB) 与 IN 报告布局无关，不占位
            continue;
        }
    }

    out->parsed = true;
    out->layout_count = 0;
    for (int r = 0; r < HidMouseDescriptor::kMaxReportId; ++r) {
        if (!seen_input[r]) continue;
        out->layouts[r].valid = true;
        out->layouts[r].report_id = r;
        out->layouts[r].total_bits = bit_cursor[r];
        out->layout_count++;
    }
    return out->layout_count > 0;
}

bool hid_field_is_safe(const HidField& f) {
    if (!f.present || f.bit_offset < 0) return false;
    // 2026-09-27：放开两处限制（用户反馈「有些鼠标不识别」的主力成因）：
    //   ① bit_size 增加 **12** —— 罗技 G 系等大量游戏鼠标的 X/Y 是 12 位；
    //   ② 不再要求 bit_offset 字节对齐 —— 12 位 X/Y 几乎必然跨字节
    //      （buttons 5bit + pad 3bit 后 X@bit8..19、Y@bit20..31）。
    // 读写实现已改为通用位窗口（RMW），对齐/非对齐同一条路。
    return f.bit_size == 8 || f.bit_size == 12 || f.bit_size == 16 || f.bit_size == 32;
}

namespace {

// 覆盖 [bit_offset, bit_offset+bit_size) 的字节窗口：首字节下标与窗口字节数。
// HID 位序为小端（bit0 = 报告首字节的 bit0），窗口内 bit_offset%8 是字段 LSB。
inline size_t field_first_byte(const HidField& f) { return static_cast<size_t>(f.bit_offset / 8); }
inline size_t field_span_bytes(const HidField& f) {
    return static_cast<size_t>((f.bit_offset % 8 + f.bit_size + 7) / 8);
}
inline uint64_t field_bit_mask(int bit_size) {
    return (bit_size >= 64) ? ~0ull : ((1ull << bit_size) - 1ull);
}

}  // namespace

bool hid_field_read_signed(const uint8_t* report, size_t report_len,
                           const HidField& f, int32_t* out) {
    if (report == nullptr || out == nullptr || !hid_field_is_safe(f)) return false;
    const size_t first = field_first_byte(f);
    const size_t span = field_span_bytes(f);
    if (first + span > report_len) return false;

    uint64_t window = 0;
    for (size_t k = 0; k < span; ++k) {
        window |= static_cast<uint64_t>(report[first + k]) << (8 * k);
    }
    uint64_t raw = (window >> (f.bit_offset % 8)) & field_bit_mask(f.bit_size);
    if (f.is_signed) {
        // 符号扩展：把字段的最高位顶到 int64 的符号位再算术右移。
        const int sh = 64 - f.bit_size;
        *out = static_cast<int32_t>(static_cast<int64_t>(raw << sh) >> sh);
    } else {
        *out = static_cast<int32_t>(raw);
    }
    return true;
}

bool hid_field_write_signed(uint8_t* report, size_t report_len,
                            const HidField& f, int32_t value) {
    if (report == nullptr || !hid_field_is_safe(f)) return false;
    const size_t first = field_first_byte(f);
    const size_t span = field_span_bytes(f);
    if (first + span > report_len) return false;

    // 按位宽做饱和钳制（写不进去的空间宁可钳，不要回绕成反向巨量）
    int64_t lo = 0;
    int64_t hi = 0;
    switch (f.bit_size) {
        case 8:  lo = -128; hi = 127; break;
        case 12: lo = -2048; hi = 2047; break;
        case 16: lo = -32768; hi = 32767; break;
        default: lo = INT32_MIN; hi = INT32_MAX; break;
    }
    if (value < lo) value = static_cast<int32_t>(lo);
    if (value > hi) value = static_cast<int32_t>(hi);

    // 读-改-写窗口字节：字段自己的位被替换，**同字节里别的字段（buttons/Y/…）原样保留**。
    // 字节对齐时窗口 == 字段自己的字节，与旧的整字节写结果一致。
    const uint64_t mask = field_bit_mask(f.bit_size);
    const int lsb = f.bit_offset % 8;
    uint64_t window = 0;
    for (size_t k = 0; k < span; ++k) {
        window |= static_cast<uint64_t>(report[first + k]) << (8 * k);
    }
    window &= ~(mask << lsb);
    window |= (static_cast<uint64_t>(static_cast<uint32_t>(value)) & mask) << lsb;
    for (size_t k = 0; k < span; ++k) {
        report[first + k] = static_cast<uint8_t>((window >> (8 * k)) & 0xFF);
    }
    return true;
}

bool hid_field_read_mask(const uint8_t* report, size_t report_len,
                         const HidField& f, uint32_t* out) {
    if (report == nullptr || out == nullptr) return false;
    if (!f.present || f.bit_offset < 0) return false;
    if (f.bit_size <= 0 || f.bit_size > 16) return false;
    const size_t first = field_first_byte(f);
    const size_t span = field_span_bytes(f);
    if (first + span > report_len) return false;

    uint64_t window = 0;
    for (size_t k = 0; k < span; ++k) {
        window |= static_cast<uint64_t>(report[first + k]) << (8 * k);
    }
    *out = static_cast<uint32_t>((window >> (f.bit_offset % 8)) & field_bit_mask(f.bit_size));
    return true;
}

bool hid_field_write_mask(uint8_t* report, size_t report_len,
                          const HidField& f, uint32_t mask_value) {
    if (report == nullptr) return false;
    if (!f.present || f.bit_offset < 0) return false;
    if (f.bit_size <= 0 || f.bit_size > 16) return false;
    const size_t first = field_first_byte(f);
    const size_t span = field_span_bytes(f);
    if (first + span > report_len) return false;

    const uint64_t mask = field_bit_mask(f.bit_size);
    const int lsb = f.bit_offset % 8;
    // 与 hid_field_read_mask 完全对称：小端、位窗口 RMW，字段外的位保持原值。
    uint64_t window = 0;
    for (size_t k = 0; k < span; ++k) {
        window |= static_cast<uint64_t>(report[first + k]) << (8 * k);
    }
    window &= ~(mask << lsb);
    window |= (static_cast<uint64_t>(mask_value) & mask) << lsb;
    for (size_t k = 0; k < span; ++k) {
        report[first + k] = static_cast<uint8_t>((window >> (8 * k)) & 0xFF);
    }
    return true;
}

bool hid_fields_overlap_bytes(const HidField& a, const HidField& b) {
    if (!a.present || !b.present) return false;
    if (a.bit_offset < 0 || b.bit_offset < 0) return false;
    const int a_lo = a.bit_offset / 8;
    const int a_hi = (a.bit_offset + a.bit_size - 1) / 8;
    const int b_lo = b.bit_offset / 8;
    const int b_hi = (b.bit_offset + b.bit_size - 1) / 8;
    return a_lo <= b_hi && b_lo <= a_hi;
}

}  // namespace ttbox_usbproxy
