// controller_params.cpp — 见 .hpp 头注释。
#include "web/translate/controller_params.hpp"

#include <cmath>
#include <string>
#include <utility>

#include "web/translate/hotkeys.hpp"

namespace ttbox::core::web {

namespace {

JsonValue num(double v) { return JsonValue::number(v); }
JsonValue integer(int64_t v) { return JsonValue::number(static_cast<double>(v)); }
JsonValue boolean(bool v) { return JsonValue::boolean(v); }
JsonValue key_bits(int64_t v) { return JsonValue::number(static_cast<double>(v)); }

// ★ 默认值必须与 core/src/mouse/MouseTypes.hpp 的结构体默认值一字不差。
std::vector<CtrlField> make_recoil_fields() {
    return {
        {"only_when_target_visible", CtrlFieldKind::kBool, boolean(true)},
        {"target_lost_release_ms", CtrlFieldKind::kNumber, num(300.0)},
        {"trigger_delay_enabled", CtrlFieldKind::kBool, boolean(false)},
        {"trigger_delay_ms", CtrlFieldKind::kNumber, num(120.0)},
        {"strength", CtrlFieldKind::kNumber, num(100.0)},
        {"speed", CtrlFieldKind::kNumber, num(1.0)},
        {"curve_strength", CtrlFieldKind::kNumber, num(0.6)},
        {"roi_h", CtrlFieldKind::kNumber, num(300.0)},
    };
}

std::vector<CtrlField> make_trigger2_fields() {
    return {
        {"enabled", CtrlFieldKind::kBool, boolean(false)},
        {"key1", CtrlFieldKind::kKey, key_bits(16)},
        {"key2", CtrlFieldKind::kKey, key_bits(0)},
        {"fire_button", CtrlFieldKind::kKey, key_bits(1)},
        {"with_aim", CtrlFieldKind::kBool, boolean(true)},
        {"with_simple_recoil", CtrlFieldKind::kBool, boolean(false)},
        {"confidence", CtrlFieldKind::kNumber, num(0.5)},
        {"first_err", CtrlFieldKind::kNumber, num(30.0)},
        {"first_delay", CtrlFieldKind::kNumber, num(0.0)},
        {"fire_interval", CtrlFieldKind::kNumber, num(1.0)},
        {"fire_random", CtrlFieldKind::kNumber, num(0.0)},
        {"fire_count", CtrlFieldKind::kInt, integer(1)},
        {"press_duration", CtrlFieldKind::kNumber, num(50.0)},
        {"move_throttle_frames", CtrlFieldKind::kInt, integer(2)},
        {"precision_enabled", CtrlFieldKind::kBool, boolean(false)},
        {"precision_range", CtrlFieldKind::kNumber, num(10.0)},
        {"precision_frames", CtrlFieldKind::kInt, integer(5)},
        {"retarget_reset_ms", CtrlFieldKind::kNumber, num(1000.0)},
        {"stop_detect_enabled", CtrlFieldKind::kBool, boolean(false)},
        {"stop_detect_color_id", CtrlFieldKind::kInt, integer(2)},
        // 注意：Python 表里这两项 kind='n'（60.0/80.0），与 MouseTypes.hpp 的 int 默认值
        // 60/80 数值相同、类型不同 —— 属 Python 既有口径，逐字保留不修正。
        {"stop_detect_tolerance", CtrlFieldKind::kNumber, num(60.0)},
        {"stop_detect_range", CtrlFieldKind::kNumber, num(80.0)},
        {"stop_detect_interval", CtrlFieldKind::kInt, integer(10)},
    };
}

std::vector<CtrlSelectorField> make_selector_fields() {
    return {
        {"selector_lock_hold_ms", "lock_hold_ms", CtrlFieldKind::kNumber, num(0.0)},
        {"selector_switch_hysteresis", "switch_hysteresis", CtrlFieldKind::kNumber, num(0.5)},
        {"selector_switch_cooldown_ms", "switch_cooldown_ms", CtrlFieldKind::kNumber, num(600.0)},
        {"selector_priority_scoring", "priority_scoring", CtrlFieldKind::kBool, boolean(false)},
        {"selector_weight_dist", "weight_dist", CtrlFieldKind::kNumber, num(1.0)},
        {"selector_weight_size", "weight_size", CtrlFieldKind::kNumber, num(0.3)},
        {"selector_stickiness", "stickiness", CtrlFieldKind::kNumber, num(1.0)},
        {"selector_switch_threshold_px", "switch_threshold_px", CtrlFieldKind::kNumber, num(60.0)},
        {"selector_head_body_stable", "head_body_stable", CtrlFieldKind::kBool, boolean(false)},
        {"selector_hb_body1", "hb_body1", CtrlFieldKind::kInt, integer(0)},
        {"selector_hb_head1", "hb_head1", CtrlFieldKind::kInt, integer(1)},
        {"selector_hb_body2", "hb_body2", CtrlFieldKind::kInt, integer(-1)},
        {"selector_hb_head2", "hb_head2", CtrlFieldKind::kInt, integer(-1)},
    };
}

}  // namespace

const std::vector<std::pair<std::string, std::string>>& controller_nums() {
    static const std::vector<std::pair<std::string, std::string>> table = {
        {"fitts_a_ms", "fitts_a_ms"},
        {"fitts_b_ms", "fitts_b_ms"},
        {"fitts_deadzone_px", "fitts_deadzone_px"},
        {"fitts_ff_gain", "fitts_ff_gain"},
        {"kp_x", "kp_x"}, {"kp_y", "kp_y"},
        {"kd_x", "kd_x"}, {"kd_y", "kd_y"},
        {"predict_x", "predict_x"}, {"predict_y", "predict_y"},
        {"rate_x", "rate_x"}, {"rate_y", "rate_y"},
        {"smooth_x", "smooth_x"}, {"smooth_y", "smooth_y"},
        {"output_deadzone", "output_deadzone"},
        {"selector_lost_grace_ms", "lost_grace_ms"},
    };
    return table;
}

const std::vector<std::pair<std::string, std::string>>& controller_strings() {
    static const std::vector<std::pair<std::string, std::string>> table = {
        {"controller_type", "controller_type"},
    };
    return table;
}

const std::vector<std::pair<std::string, std::string>>& controller_bools() {
    // 现役四个布尔键都是嵌套结构开关（值以 '_' 开头），在 web_body_to_profile 里
    // 被统一跳过、改由各自专段处理；表保留是为了与 Python 同源、防后续误删。
    static const std::vector<std::pair<std::string, std::string>> table = {
        {"pull_curve_enabled", "_pc_enabled"},
        {"personal_trajectory_enabled", "_pt_enabled"},
        {"lock_confirm_instant_enter_enabled", "_lc_inst_enter_enabled"},
        {"head_aim_enabled", "_ha_enabled"},
    };
    return table;
}

const std::vector<CtrlBlock>& ctrl_blocks() {
    static const std::vector<CtrlBlock> blocks = {
        {"recoil", "recoil", make_recoil_fields()},
        {"trigger2", "trigger2", make_trigger2_fields()},
    };
    return blocks;
}

const std::vector<CtrlSelectorField>& ctrl_selector_fields() {
    static const std::vector<CtrlSelectorField> fields = make_selector_fields();
    return fields;
}

JsonValue coerce_ctrl_value(const JsonValue& value, CtrlFieldKind kind) {
    switch (kind) {
        case CtrlFieldKind::kBool:
            // Python bool(v) 的 truthiness 对非布尔输入更宽；面板 'b' 字段恒发真布尔，
            // 此处按 JSON 布尔收窄，非布尔一律 false（对 checkbox 语义等价）。
            return JsonValue::boolean(value.as_bool());
        case CtrlFieldKind::kKey:
            return JsonValue::number(static_cast<double>(hotkey_to_bits(value, 0)));
        case CtrlFieldKind::kInt:
            if (value.is_number()) {
                return JsonValue::number(static_cast<double>(value.as_int()));
            }
            return JsonValue::null();
        case CtrlFieldKind::kNumber:
            if (value.is_number()) return JsonValue::number(value.as_number());
            return JsonValue::null();
    }
    return JsonValue::null();
}

JsonValue ctrl_read_block(const JsonValue& ctrl, const CtrlBlock& block) {
    JsonValue out = JsonValue::object();
    for (const CtrlField& f : block.fields) {
        const std::string web_key = block.prefix + "_" + f.name;
        const JsonValue* v = ctrl.is_object() ? ctrl.find(web_key) : nullptr;
        if (v == nullptr || v->is_null()) continue;
        JsonValue cv = coerce_ctrl_value(*v, f.kind);
        if (!cv.is_null()) out.set(f.name, std::move(cv));
    }
    return out;
}

void ctrl_write_block(JsonValue& ctrl, const CtrlBlock& block, const JsonValue* blk) {
    for (const CtrlField& f : block.fields) {
        const std::string web_key = block.prefix + "_" + f.name;
        const JsonValue* v =
            (blk != nullptr && blk->is_object()) ? blk->find(f.name) : nullptr;
        JsonValue out_value;
        if (f.kind == CtrlFieldKind::kKey) {
            const int64_t bits = v != nullptr ? v->as_int() : f.default_value.as_int();
            out_value = JsonValue::string(bits_to_hotkey(bits));
        } else if (v != nullptr) {
            out_value = *v;
        } else {
            out_value = f.default_value;
        }
        ctrl.set(web_key, std::move(out_value));
    }
}

JsonValue ctrl_read_vec(const JsonValue& ctrl, const std::string& key, int n) {
    const JsonValue* v = ctrl.is_object() ? ctrl.find(key) : nullptr;
    if (v == nullptr || !v->is_array() || static_cast<int>(v->as_array().size()) != n) {
        return JsonValue::null();
    }
    JsonValue out = JsonValue::array();
    for (const JsonValue& item : v->as_array()) {
        JsonValue cv = coerce_ctrl_value(item, CtrlFieldKind::kNumber);
        if (cv.is_null()) return JsonValue::null();
        out.push_back(std::move(cv));
    }
    return out;
}

JsonValue ctrl_read_table(const JsonValue& ctrl, const std::string& key, int rows, int cols) {
    const JsonValue* v = ctrl.is_object() ? ctrl.find(key) : nullptr;
    if (v == nullptr || !v->is_array() || static_cast<int>(v->as_array().size()) != rows) {
        return JsonValue::null();
    }
    JsonValue out = JsonValue::array();
    for (const JsonValue& row : v->as_array()) {
        if (!row.is_array() || static_cast<int>(row.as_array().size()) != cols) {
            return JsonValue::null();
        }
        JsonValue crow = JsonValue::array();
        for (const JsonValue& item : row.as_array()) {
            JsonValue cv = coerce_ctrl_value(item, CtrlFieldKind::kNumber);
            if (cv.is_null()) return JsonValue::null();
            crow.push_back(std::move(cv));
        }
        out.push_back(std::move(crow));
    }
    return out;
}

int64_t normalize_capture_crop_size(const JsonValue& value) {
    constexpr int64_t kFullFrame = 0;
    constexpr int64_t kMinValid = 64;
    constexpr int64_t kMaxValid = 3840;

    double d = 0.0;
    if (value.is_number()) {
        d = std::nearbyint(value.as_number());  // 对齐 Python int(round(float(v)))（banker's）
    } else if (value.is_string()) {
        try {
            d = std::nearbyint(std::stod(value.as_string()));
        } catch (...) {
            return kFullFrame;
        }
    } else {
        return kFullFrame;
    }
    if (std::isnan(d)) return kFullFrame;

    const int64_t v = static_cast<int64_t>(d);
    if (v <= kFullFrame) return kFullFrame;
    if (v < kMinValid) return kMinValid;
    if (v > kMaxValid) return kMaxValid;
    return v;
}

}  // namespace ttbox::core::web
