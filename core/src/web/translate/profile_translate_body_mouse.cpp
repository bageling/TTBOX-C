// profile_translate_body_mouse.cpp — web_body_to_profile 的 mouse 段（见内部头注释）。
#include "web/translate/profile_translate_body_internal.hpp"

#include <initializer_list>
#include <string>
#include <utility>

#include "web/translate/controller_params.hpp"
#include "web/translate/hotkeys.hpp"
#include "web/translate/profile_translate.hpp"

namespace ttbox::core::web {
namespace detail {

namespace {

// 面板扁平键 → 子对象字段（Python 内联映射表）。
struct FieldMap {
    const char* web_key;
    const char* core_key;
};

}  // namespace

MouseBuildResult build_mouse(const JsonValue& body) {
    const JsonValue& ctrl = json_field(json_field(body, "ai"), "controller");
    JsonValue mouse = JsonValue::object();

    // 1) controller 数值/布尔/枚举直通。
    for (const auto& [yk, tk] : controller_nums()) {
        const JsonValue& v = json_field(ctrl, yk);
        if (!v.is_null()) mouse.set(tk, v);
    }
    for (const auto& [yk, tk] : controller_bools()) {
        const JsonValue& v = json_field(ctrl, yk);
        if (v.is_null()) continue;
        if (!tk.empty() && tk[0] == '_') continue;  // 嵌套结构开关，下面统一处理
        mouse.set(tk, JsonValue::boolean(json_truthy(v)));
    }
    // controller_type 枚举字符串白名单：非法一律回退 'fitts'（与 core 缺省一致）。
    const JsonValue& ctype = json_field(ctrl, "controller_type");
    if (!ctype.is_null()) {
        const std::string s = trim_lower(ctype.as_string());
        mouse.set("controller_type",
                  JsonValue::string((s == "fitts" || s == "pid1") ? s : "fitts"));
    }

    // 只收面板真传了的字段（不补默认，补默认是 Core from_json 的职责）。
    auto collect = [&ctrl](JsonValue& blk, std::initializer_list<FieldMap> maps) {
        for (const FieldMap& m : maps) {
            const JsonValue& v = json_field(ctrl, m.web_key);
            if (!v.is_null()) blk.set(m.core_key, v);
        }
    };

    // 2) 插件结构：pull_curve / continuous_lead / personal_trajectory / lock_confirm。
    {
        JsonValue blk = JsonValue::object();
        const JsonValue& en = json_field(ctrl, "pull_curve_enabled");
        if (!en.is_null()) blk.set("enabled", JsonValue::boolean(json_truthy(en)));
        collect(blk, {{"pull_curve_strength", "strength"},
                      {"pull_curve_min_distance", "min_distance"}});
        if (!blk.as_object().empty()) mouse.set("pull_curve", std::move(blk));
    }
    {
        JsonValue blk = JsonValue::object();
        const JsonValue& en = json_field(ctrl, "continuous_lead_enabled");
        if (!en.is_null()) blk.set("enabled", JsonValue::boolean(json_truthy(en)));
        collect(blk, {{"continuous_lead_enter_distance", "enter_distance"},
                      {"continuous_lead_scale", "scale"},
                      {"continuous_lead_fade_in_ms", "fade_in_ms"},
                      {"continuous_lead_fade_out_ms", "fade_out_ms"},
                      {"continuous_lead_near_disable_ratio", "near_disable_ratio"}});
        if (!blk.as_object().empty()) mouse.set("continuous_lead", std::move(blk));
    }
    {
        JsonValue blk = JsonValue::object();
        const JsonValue& en = json_field(ctrl, "personal_trajectory_enabled");
        if (!en.is_null()) blk.set("enabled", JsonValue::boolean(json_truthy(en)));
        collect(blk, {{"personal_trajectory_speed_scale", "speed_scale"},
                      {"personal_trajectory_stability_scale", "stability_scale"},
                      {"personal_trajectory_variation_scale", "variation_scale"},
                      {"personal_trajectory_jitter_amp_px", "jitter_amp_px"},
                      {"personal_trajectory_fitts_intercept_ms", "fitts_intercept_ms"},
                      {"personal_trajectory_fitts_slope_ms_per_bit",
                       "fitts_slope_ms_per_bit"}});
        if (!blk.as_object().empty()) mouse.set("personal_trajectory", std::move(blk));
    }
    {
        JsonValue blk = JsonValue::object();
        const JsonValue& en = json_field(ctrl, "lock_confirm_instant_enter_enabled");
        if (!en.is_null()) blk.set("instant_enter_enabled", JsonValue::boolean(json_truthy(en)));
        collect(blk, {{"lock_confirm_confirmation_frames", "confirmation_frames"},
                      {"lock_confirm_enter_conf", "enter_conf"},
                      {"lock_confirm_hold_conf", "hold_conf"},
                      {"lock_confirm_instant_enter_dist", "instant_enter_dist"},
                      {"lock_confirm_instant_enter_conf", "instant_enter_conf"}});
        if (!blk.as_object().empty()) mouse.set("lock_confirm", std::move(blk));
    }

    // 压枪（recoil）：Web 提交在 body 顶层 recoil 块；这里只收开关与热键，速率/门控
    // 参数由下面 CTRL_BLOCKS['recoil'] 统一搬运并合并。
    {
        const JsonValue& rk = json_field(body, "recoil");
        JsonValue blk = JsonValue::object();
        const JsonValue& en = json_field(rk, "enabled");
        if (!en.is_null()) blk.set("enabled", JsonValue::boolean(json_truthy(en)));
        const JsonValue& hk = json_field(rk, "hotkey");
        if (!hk.is_null()) {
            int64_t bits = hotkey_to_bits(hk, 1);
            if (bits == 0) bits = 1;
            blk.set("hotkey", JsonValue::number(static_cast<double>(bits)));
        }
        const JsonValue& hk2 = json_field(rk, "hotkey2");
        if (!hk2.is_null()) {
            blk.set("hotkey2", JsonValue::number(static_cast<double>(hotkey_to_bits(hk2, 0))));
        }
        const JsonValue& mode = json_field(rk, "hotkey_mode");
        if (!mode.is_null()) {
            blk.set("hotkey_mode", JsonValue::number(mode.as_string() == "all" ? 2.0 : 1.0));
        }
        if (!blk.as_object().empty()) mouse.set("recoil", std::move(blk));
    }

    // 表驱动搬运：CTRL_BLOCKS 每个 (前缀, 子对象) 收成 mouse[子对象]；压枪块要与上面
    // body['recoil'] 收进来的开关/热键合并（直接覆盖会把 enabled/hotkey 一起抹掉）。
    for (const CtrlBlock& block : ctrl_blocks()) {
        JsonValue blk = ctrl_read_block(ctrl, block);
        if (blk.as_object().empty()) continue;
        const JsonValue* cur = mouse.is_object() ? mouse.find(block.obj_key) : nullptr;
        if (cur != nullptr && cur->is_object()) {
            JsonValue merged = *cur;
            for (const auto& [k, v] : blk.as_object()) {
                merged.set(k, v);
            }
            mouse.set(block.obj_key, std::move(merged));
        } else {
            mouse.set(block.obj_key, std::move(blk));
        }
    }

    // 选靶四项机制：Core 侧是 mouse 顶层扁平键。
    for (const CtrlSelectorField& f : ctrl_selector_fields()) {
        const JsonValue& v = json_field(ctrl, f.web_key);
        if (v.is_null()) continue;
        JsonValue cv = coerce_ctrl_value(v, f.kind);
        if (!cv.is_null()) mouse.set(f.core_key, std::move(cv));
    }

    // 热键保护（hotkey_guard）：toggle_hotkey 空/未识别回落 middle（4）。
    {
        const JsonValue& hg = json_field(body, "hotkey_guard");
        JsonValue guard = JsonValue::object();
        const JsonValue& en = json_field(hg, "enabled");
        if (!en.is_null()) guard.set("enabled", JsonValue::boolean(json_truthy(en)));
        const JsonValue& toggle = json_field(hg, "toggle_hotkey");
        if (!toggle.is_null()) {
            int64_t bits = hotkey_to_bits(toggle, 4);
            if (bits == 0) bits = 4;
            guard.set("toggle_hotkey", JsonValue::number(static_cast<double>(bits)));
        }
        if (!guard.as_object().empty()) mouse.set("hotkey_guard", std::move(guard));
    }

    // 头部瞄准约束（head_aim）。
    {
        JsonValue blk = JsonValue::object();
        const JsonValue& en = json_field(ctrl, "head_aim_enabled");
        if (!en.is_null()) blk.set("enabled", JsonValue::boolean(json_truthy(en)));
        collect(blk, {{"head_aim_head_offset_top_fraction", "head_offset_top_fraction"},
                      {"head_aim_head_height_fraction", "head_height_fraction"},
                      {"head_aim_safe_inset_fraction", "safe_inset_fraction"},
                      {"head_aim_max_lag_px", "max_lag_px"}});
        if (!blk.as_object().empty()) mouse.set("head_aim", std::move(blk));
    }

    // 3) 个人移动曲线（personal_motion）。
    {
        JsonValue blk = JsonValue::object();
        collect(blk, {{"personal_motion_enabled", "enabled"},
                      {"personal_motion_curve_blend", "curve_blend"},
                      {"personal_motion_speed_blend", "speed_blend"},
                      {"personal_motion_reaction_blend", "reaction_blend"},
                      {"personal_motion_max_reaction_delay_ms", "max_reaction_delay_ms"}});
        if (!blk.as_object().empty()) mouse.set("personal_motion", std::move(blk));
    }

    // 4) 目标选择（selector_lost_grace_ms 已在 CONTROLLER_NUMS 表里映射到 lost_grace_ms，
    //    此处不再重复处理 —— Python 原实现两处等价，逐字复刻只留表驱动这一处）。

    // 5) 瞄准档位 aim_profiles[]：热键 / 瞄准点 / 移动倍率 / FOV 倍率 / 目标类别。
    const JsonValue& profiles = json_field(body, "aim_profiles");
    JsonValue core_profiles =
        profiles.is_null() ? JsonValue::array() : validate_aim_profiles(profiles);
    if (!core_profiles.as_array().empty()) mouse.set("aim_profiles", core_profiles);
    const JsonValue& p0 = core_profiles.as_array().empty()
                              ? JsonValue::null()
                              : core_profiles.as_array().front();

    // 目标类别：推理侧只能收全档并集（-1 = 未提交哨兵，与 mouse 段同解）。
    int64_t class_union_mask = -1;
    if (!core_profiles.as_array().empty()) {
        class_union_mask = 0;
        for (const JsonValue& p : profiles.as_array()) {
            class_union_mask |= to_int64(json_field(p, "class_filter_mask"), 0);
        }
    }

    // 5b) 全局量：sens → sensitivity；pos → offset_y（卡片的移动倍率不写进全局）。
    const JsonValue& sens = json_field(body, "sens");
    if (!sens.is_null()) mouse.set("sensitivity", sens);

    JsonValue aim_point_vals = JsonValue::object();
    const JsonValue& p0_ox = json_field(p0, "offset_x");
    if (!p0_ox.is_null()) aim_point_vals.set("offset_x", p0_ox);
    const JsonValue& p0_oy = json_field(p0, "offset_y");
    if (!p0_oy.is_null()) {
        aim_point_vals.set("offset_y", p0_oy);
    } else {
        const JsonValue& pos = json_field(body, "pos");
        if (!pos.is_null()) aim_point_vals.set("offset_y", pos);
    }
    const JsonValue& p0_co = json_field(p0, "class_offsets");
    if (json_truthy(p0_co)) mouse.set("class_offsets", p0_co);
    for (const auto& [k, v] : aim_point_vals.as_object()) {
        mouse.set(k, v);
    }

    return {std::move(mouse), class_union_mask};
}

}  // namespace detail
}  // namespace ttbox::core::web
