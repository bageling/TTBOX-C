// inject_clock.cpp — 见 inject_clock.hpp 的设计说明。纯逻辑，无系统调用。
#include "inject_clock.hpp"

#include <cstring>

namespace ttbox_usbproxy {
namespace {

// 单轴限幅：返回本拍投递的量，rest 为顺延余量，dropped 为因上限丢弃的量。
void split_axis(int32_t in, int32_t step_max, int32_t backlog_max,
                int32_t* step, int32_t* rest, int32_t* dropped) {
    *step = 0;
    *rest = 0;
    *dropped = 0;
    if (step_max < 0) step_max = 0;
    if (backlog_max < 0) backlog_max = 0;

    if (in > step_max) *step = step_max;
    else if (in < -step_max) *step = -step_max;
    else *step = in;

    int64_t r = static_cast<int64_t>(in) - static_cast<int64_t>(*step);
    if (r > backlog_max) {
        *dropped = static_cast<int32_t>(r - backlog_max);
        r = backlog_max;
    } else if (r < -backlog_max) {
        *dropped = static_cast<int32_t>(r + backlog_max);  // 负值（被丢弃的量）
        r = -backlog_max;
    }
    *rest = static_cast<int32_t>(r);
}

}  // namespace

// 把三轴累计位移各自切成"本拍投递 + 顺延余量 + 丢弃量"。
InjectStep inject_clock_plan(const InjectClockConfig& cfg, int32_t dx, int32_t dy, int32_t wheel) {
    InjectStep st;
    split_axis(dx, cfg.step_max_x, cfg.backlog_max, &st.dx, &st.rest_x, &st.dropped_x);
    split_axis(dy, cfg.step_max_y, cfg.backlog_max, &st.dy, &st.rest_y, &st.dropped_y);
    split_axis(wheel, cfg.step_max_wheel, cfg.step_max_wheel, &st.wheel, &st.rest_wheel,
               &st.dropped_wheel);
    return st;
}

// 按真实布局构造一份 HID 报告（写 buttons/X/Y/wheel，其余字节清零）；无法构造返回 false。
bool inject_clock_build_report(const HidMouseDescriptor& desc,
                               uint8_t buttons,
                               const InjectStep& step,
                               uint8_t* out, uint32_t cap, uint32_t* out_len) {
    if (out == nullptr || out_len == nullptr) return false;
    if (!desc.parsed) return false;

    // 选布局：优先"位移报告"（含 X/Y），退而求其次"按键报告"。
    const HidReportLayout* lay = desc.xy_layout();
    if (lay == nullptr) lay = desc.button_layout();
    if (lay == nullptr || !lay->valid) return false;

    const bool with_rid = desc.uses_report_ids;
    const uint32_t body_len = static_cast<uint32_t>(lay->total_bytes());
    const uint32_t total = body_len + (with_rid ? 1u : 0u);
    if (total == 0 || total > cap) return false;

    std::memset(out, 0, total);
    uint8_t* body = out;
    if (with_rid) {
        out[0] = static_cast<uint8_t>(lay->report_id);
        body = out + 1;
    }

    // 按键：写成"当前已知的物理按键状态"（HID 报告每份都携带完整状态）。
    if (lay->has_buttons()) {
        if (!hid_field_write_mask(body, body_len, lay->buttons, buttons)) return false;
    }
    // 位移/滚轮：本拍的量（已限幅），零位移也照写（就是"无移动"）。
    if (lay->has_xy()) {
        if (!hid_field_is_safe(lay->x) || !hid_field_is_safe(lay->y)) return false;
        if (hid_fields_overlap_bytes(lay->x, lay->buttons) ||
            hid_fields_overlap_bytes(lay->y, lay->buttons) ||
            hid_fields_overlap_bytes(lay->x, lay->wheel) ||
            hid_fields_overlap_bytes(lay->y, lay->wheel)) {
            return false;
        }
        if (!hid_field_write_signed(body, body_len, lay->x, step.dx)) return false;
        if (!hid_field_write_signed(body, body_len, lay->y, step.dy)) return false;
    }
    if (lay->wheel.present) {
        if (!hid_field_write_signed(body, body_len, lay->wheel, step.wheel)) return false;
    }

    *out_len = total;
    return true;
}

}  // namespace ttbox_usbproxy
