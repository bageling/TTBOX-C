// profile_translate_aim.cpp — 热键卡与 aim_profiles[]（见 .hpp 头注释）。
#include "web/translate/profile_translate.hpp"

#include <string>
#include <utility>
#include <vector>

#include "web/translate/hotkeys.hpp"

namespace ttbox::core::web {

namespace {

// core 的触发方式 → 面板选单值（'all' / 'any'）。同时认 bool / number / string 三种形式。
std::string hotkey_mode_to_web(const JsonValue& v) {
    if (v.is_bool()) return v.as_bool() ? "all" : "any";
    if (v.is_number()) return v.as_int() == 1 ? "all" : "any";
    return trim_lower(v.as_string()) == "all" ? "all" : "any";
}

// class_filter 列表 → 位掩码（对齐 Python int(i) 语义；负数/非法跳过）。
// class_id 域内为 0~31（与反向换算 range(32) 同源），<63 防移位 UB。
int64_t class_filter_to_mask(const JsonValue& cf) {
    if (!cf.is_array()) return 0;
    int64_t mask = 0;
    for (const JsonValue& e : cf.as_array()) {
        const int64_t iv = to_int64(e, -1);
        if (iv >= 0 && iv < 63) mask |= (1LL << iv);
    }
    return mask;
}

}  // namespace

// 单张面板热键卡 → core aim_profiles 档位对象（热键位合法化 + 类别掩码展开）。
JsonValue aim_profile_core_dict(const JsonValue& p) {
    JsonValue out = JsonValue::object();

    const int64_t hk = hotkey_to_bits(json_field(p, "hotkey"), 2);
    out.set("hotkey", JsonValue::number(static_cast<double>(is_valid_hotkey_bit(hk) ? hk : 2)));

    const int64_t hk2 = hotkey_to_bits(json_field(p, "hotkey2"), 0);
    out.set("hotkey2", JsonValue::number(static_cast<double>(is_valid_hotkey_bit(hk2) ? hk2 : 0)));

    out.set("hotkey_mode", JsonValue::string(hotkey_mode_to_web(json_field(p, "hotkey_mode"))));

    const JsonValue& ox = json_field(p, "offset_x");
    if (!ox.is_null()) out.set("offset_x", ox);
    const JsonValue& oy = json_field(p, "offset_y");
    if (!oy.is_null()) out.set("offset_y", oy);

    const JsonValue& co = json_field(p, "class_offsets");
    if (json_truthy(co)) out.set("class_offsets", co);

    const JsonValue& sens = json_field(p, "sensitivity");
    if (!sens.is_null()) out.set("sensitivity", sens);

    const JsonValue& fov_scale = json_field(p, "fov_scale");
    if (!fov_scale.is_null()) {
        out.set("fov_scale", JsonValue::number(fov_factor_clamp(fov_scale, 1.0)));
    }

    const JsonValue& mask_v = json_field(p, "class_filter_mask");
    if (!mask_v.is_null()) {
        const int64_t m = to_int64(mask_v, 0);
        JsonValue cf = JsonValue::array();
        for (int i = 0; i < 32; ++i) {
            if ((m & (1LL << i)) != 0) {
                cf.push_back(JsonValue::number(static_cast<double>(i)));
            }
        }
        out.set("class_filter", std::move(cf));
    }
    return out;
}

// 档位主/副热键位掩码的并集（按键冲突检测用）。
int64_t aim_profile_key_bits(const JsonValue& p) {
    return hotkey_to_bits(json_field(p, "hotkey"), 0) |
           hotkey_to_bits(json_field(p, "hotkey2"), 0);
}

// 校验并规范化 aim_profiles[]：档数上限、主/副键合法且不冲突，逐档转 core 档位对象。
JsonValue validate_aim_profiles(const JsonValue& profiles) {
    if (!profiles.is_array() || profiles.as_array().empty()) {
        return JsonValue::array();
    }
    const std::vector<JsonValue>& arr = profiles.as_array();
    if (static_cast<int>(arr.size()) > kAimProfileMax) {
        throw ConfigValidationError("热键最多 " + std::to_string(kAimProfileMax) +
                                    " 组，当前提交了 " + std::to_string(arr.size()) + " 组");
    }

    std::vector<int64_t> masks;
    masks.reserve(arr.size());
    int idx = 1;
    for (const JsonValue& p : arr) {
        const int64_t hk = hotkey_to_bits(json_field(p, "hotkey"), 0);
        const int64_t hk2 = hotkey_to_bits(json_field(p, "hotkey2"), 0);
        const std::string mode = hotkey_mode_to_web(json_field(p, "hotkey_mode"));
        if (hk == 0) {
            throw ConfigValidationError("热键 " + std::to_string(idx) + "：请选择主按键");
        }
        if (!is_valid_hotkey_bit(hk)) {
            throw ConfigValidationError(
                "热键 " + std::to_string(idx) + "：主按键值非法（" + std::to_string(hk) +
                "），只能是 左键/右键/中键/侧键1/侧键2 之一");
        }
        if (hk2 != 0 && !is_valid_hotkey_bit(hk2)) {
            throw ConfigValidationError(
                "热键 " + std::to_string(idx) + "：副按键值非法（" + std::to_string(hk2) +
                "），只能是 左键/右键/中键/侧键1/侧键2 之一");
        }
        if ((hk & hk2) != 0) {
            throw ConfigValidationError(
                "热键 " + std::to_string(idx) + "：副按键不能与主按键相同（同一个键等于没按）");
        }
        if (mode == "all" && hk2 == 0) {
            throw ConfigValidationError(
                "热键 " + std::to_string(idx) +
                "：触发方式选了「同时按下」，必须再选一个副按键");
        }
        masks.push_back(hk | hk2);
        ++idx;
    }

    for (size_t i = 0; i < masks.size(); ++i) {
        for (size_t j = i + 1; j < masks.size(); ++j) {
            const int64_t dup = masks[i] & masks[j];
            if (dup == 0) continue;
            std::string names;
            for (int b = 0; b < 5; ++b) {
                const int64_t bit = 1LL << b;
                if ((dup & bit) == 0) continue;
                std::string name = hotkey_name(bit);
                if (name.empty()) name = std::to_string(bit);
                if (!names.empty()) names += "、";
                names += name;
            }
            throw ConfigValidationError(
                "热键 " + std::to_string(i + 1) + " 与热键 " + std::to_string(j + 1) +
                " 不能共用按键（重复：" + names + "）。一个按键只能归一组 —— 请换一个键，或删掉其中一组");
        }
    }

    JsonValue out = JsonValue::array();
    for (const JsonValue& p : arr) {
        out.push_back(aim_profile_core_dict(p));
    }
    return out;
}

// core aim_profiles → 面板热键卡数组；无档则用老配置平铺键合成单卡（行为等价回退）。
JsonValue aim_profiles_to_web(const JsonValue& mouse, const JsonValue& inf) {
    const JsonValue& raw = json_field(mouse, "aim_profiles");
    if (raw.is_array() && !raw.as_array().empty()) {
        JsonValue cards = JsonValue::array();
        for (const JsonValue& j : raw.as_array()) {
            const int64_t mask = class_filter_to_mask(json_field(j, "class_filter"));
            const double ox = json_field(j, "offset_x").as_number(0.5);
            const double oy = json_field(j, "offset_y").as_number(0.5);

            JsonValue card = JsonValue::object();
            const std::string hk = bits_to_hotkey(json_field(j, "hotkey").as_int(2));
            card.set("hotkey", JsonValue::string(hk.empty() ? "right" : hk));
            card.set("hotkey2",
                     JsonValue::string(bits_to_hotkey(json_field(j, "hotkey2").as_int(0))));
            card.set("hotkey_mode",
                     JsonValue::string(hotkey_mode_to_web(json_field(j, "hotkey_mode"))));
            const JsonValue& sens = json_field(j, "sensitivity");
            card.set("sensitivity", sens.is_null() ? JsonValue::number(1.0) : sens);
            const JsonValue& fov_scale = json_field(j, "fov_scale");
            card.set("fov_scale", fov_scale.is_null() ? JsonValue::number(1.0) : fov_scale);
            card.set("offset_x", JsonValue::number(ox));
            card.set("offset_y", JsonValue::number(oy));
            card.set("alternate_offset_x", JsonValue::number(ox));
            card.set("alternate_offset_y", JsonValue::number(oy));
            card.set("class_filter_mask", JsonValue::number(static_cast<double>(mask)));
            const JsonValue& co = json_field(j, "class_offsets");
            card.set("class_offsets", co.is_array() ? co : JsonValue::array());
            card.set("offset_switch_enabled", JsonValue::boolean(false));
            card.set("offset_switch_hotkey", JsonValue::string(""));
            cards.push_back(std::move(card));
        }
        return cards;
    }

    // 老配置回退：合成单卡（sensitivity / fov_scale 都是 1.0；类别继承全局 class_filter）。
    JsonValue card = JsonValue::object();
    const std::string hk = bits_to_hotkey(json_field(mouse, "aim_hotkey").as_int(2));
    card.set("hotkey", JsonValue::string(hk.empty() ? "right" : hk));
    card.set("hotkey2",
             JsonValue::string(bits_to_hotkey(json_field(mouse, "aim_hotkey2").as_int(0))));
    card.set("hotkey_mode",
             JsonValue::string(hotkey_mode_to_web(json_field(mouse, "aim_hotkey_mode"))));
    card.set("sensitivity", JsonValue::number(1.0));
    card.set("fov_scale", JsonValue::number(1.0));
    const double ox = json_field(mouse, "offset_x").as_number(0.5);
    const double oy = json_field(mouse, "offset_y").as_number(0.5);
    card.set("offset_x", JsonValue::number(ox));
    card.set("offset_y", JsonValue::number(oy));
    card.set("alternate_offset_x", JsonValue::number(ox));
    card.set("alternate_offset_y", JsonValue::number(oy));
    card.set("class_filter_mask", JsonValue::number(static_cast<double>(
                                      class_filter_to_mask(json_field(inf, "class_filter")))));
    const JsonValue& co = json_field(mouse, "class_offsets");
    card.set("class_offsets", co.is_array() ? co : JsonValue::array());
    card.set("offset_switch_enabled", JsonValue::boolean(false));
    card.set("offset_switch_hotkey", JsonValue::string(""));

    JsonValue cards = JsonValue::array();
    cards.push_back(std::move(card));
    return cards;
}

}  // namespace ttbox::core::web
