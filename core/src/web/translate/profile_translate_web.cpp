// profile_translate_web.cpp — profile_to_web（见 .hpp 头注释）。
#include "web/translate/profile_translate.hpp"

#include <string>
#include <utility>

#include "web/translate/controller_params.hpp"
#include "web/translate/hotkeys.hpp"

namespace ttbox::core::web {

// core RuntimeProfile → 面板可读配置体：控制器字段直投 + 结构块展开 + FOV/热键换算。
JsonValue profile_to_web(const JsonValue& prof) {
    const JsonValue& mouse = json_field(prof, "mouse");
    const JsonValue& pc = json_field(mouse, "pull_curve");
    const JsonValue& lead = json_field(mouse, "continuous_lead");
    const JsonValue& fov_p = json_field(prof, "fov");
    const JsonValue& prev_p = json_field(prof, "preview");
    const JsonValue& inf = json_field(prof, "inference");
    const JsonValue& cap = json_field(prof, "capture");
    const JsonValue& personal_traj = json_field(mouse, "personal_trajectory");
    const JsonValue& lock_confirm = json_field(mouse, "lock_confirm");
    const JsonValue& head_aim = json_field(mouse, "head_aim");
    const JsonValue& recoil = json_field(mouse, "recoil");

    // FOV 半径 → 面板倍率（缺 radius 时给 0.5，与 Python fov_p.get('radius', 0.5) 同源）。
    const JsonValue& radius_v = json_field(fov_p, "radius");
    const JsonValue radius = radius_v.is_null() ? JsonValue::number(0.5) : radius_v;
    const double fov_factor_web = fov_radius_to_factor(radius, json_field(fov_p, "enabled"));

    JsonValue ctrl = JsonValue::object();
    // 字段直投：缺字段补 Core 结构体默认值（保证面板首次回填显示的就是 Core 实际值）。
    auto set_ctrl = [&ctrl](const char* web_key, const JsonValue& obj, const char* src_key,
                            JsonValue dflt) {
        const JsonValue& v = json_field(obj, src_key);
        ctrl.set(web_key, v.is_null() ? std::move(dflt) : v);
    };

    set_ctrl("kp_x", mouse, "kp_x", JsonValue::null());
    set_ctrl("kp_y", mouse, "kp_y", JsonValue::null());
    set_ctrl("kd_x", mouse, "kd_x", JsonValue::null());
    set_ctrl("kd_y", mouse, "kd_y", JsonValue::null());
    set_ctrl("predict_x", mouse, "predict_x", JsonValue::null());
    set_ctrl("predict_y", mouse, "predict_y", JsonValue::null());
    set_ctrl("rate_x", mouse, "rate_x", JsonValue::null());
    set_ctrl("rate_y", mouse, "rate_y", JsonValue::null());
    set_ctrl("smooth_x", mouse, "smooth_x", JsonValue::null());
    set_ctrl("smooth_y", mouse, "smooth_y", JsonValue::null());
    set_ctrl("output_deadzone", mouse, "output_deadzone", JsonValue::null());
    set_ctrl("selector_lost_grace_ms", mouse, "lost_grace_ms", JsonValue::null());
    set_ctrl("pull_curve_enabled", pc, "enabled", JsonValue::boolean(true));
    set_ctrl("pull_curve_strength", pc, "strength", JsonValue::number(0.8));
    set_ctrl("pull_curve_min_distance", pc, "min_distance", JsonValue::number(80.0));
    set_ctrl("continuous_lead_enabled", lead, "enabled", JsonValue::boolean(false));
    set_ctrl("continuous_lead_enter_distance", lead, "enter_distance", JsonValue::number(150.0));
    set_ctrl("continuous_lead_scale", lead, "scale", JsonValue::number(0.5));
    set_ctrl("continuous_lead_fade_in_ms", lead, "fade_in_ms", JsonValue::number(300.0));
    set_ctrl("continuous_lead_fade_out_ms", lead, "fade_out_ms", JsonValue::number(300.0));
    set_ctrl("continuous_lead_near_disable_ratio", lead, "near_disable_ratio",
             JsonValue::number(0.66));
    set_ctrl("personal_trajectory_enabled", personal_traj, "enabled", JsonValue::boolean(false));
    set_ctrl("personal_trajectory_speed_scale", personal_traj, "speed_scale",
             JsonValue::number(1.0));
    set_ctrl("personal_trajectory_stability_scale", personal_traj, "stability_scale",
             JsonValue::number(1.0));
    set_ctrl("personal_trajectory_variation_scale", personal_traj, "variation_scale",
             JsonValue::number(1.0));
    set_ctrl("personal_trajectory_jitter_amp_px", personal_traj, "jitter_amp_px",
             JsonValue::number(0.20));
    set_ctrl("personal_trajectory_fitts_intercept_ms", personal_traj, "fitts_intercept_ms",
             JsonValue::number(120.0));
    set_ctrl("personal_trajectory_fitts_slope_ms_per_bit", personal_traj,
             "fitts_slope_ms_per_bit", JsonValue::number(85.0));
    set_ctrl("lock_confirm_confirmation_frames", lock_confirm, "confirmation_frames",
             JsonValue::number(1.0));
    set_ctrl("lock_confirm_enter_conf", lock_confirm, "enter_conf", JsonValue::number(0.0));
    set_ctrl("lock_confirm_hold_conf", lock_confirm, "hold_conf", JsonValue::number(0.0));
    set_ctrl("lock_confirm_instant_enter_enabled", lock_confirm, "instant_enter_enabled",
             JsonValue::boolean(true));
    set_ctrl("lock_confirm_instant_enter_dist", lock_confirm, "instant_enter_dist",
             JsonValue::number(105.0));
    set_ctrl("lock_confirm_instant_enter_conf", lock_confirm, "instant_enter_conf",
             JsonValue::number(0.5));
    set_ctrl("head_aim_enabled", head_aim, "enabled", JsonValue::boolean(false));
    set_ctrl("head_aim_head_offset_top_fraction", head_aim, "head_offset_top_fraction",
             JsonValue::number(0.04));
    set_ctrl("head_aim_head_height_fraction", head_aim, "head_height_fraction",
             JsonValue::number(0.28));
    set_ctrl("head_aim_safe_inset_fraction", head_aim, "safe_inset_fraction",
             JsonValue::number(0.12));
    set_ctrl("head_aim_max_lag_px", head_aim, "max_lag_px", JsonValue::number(1.25));

    // BB 对标新模块：Core 子对象 → 面板扁平键（缺字段补表里的 Core 默认值）。
    for (const CtrlBlock& block : ctrl_blocks()) {
        const JsonValue* obj = mouse.is_object() ? mouse.find(block.obj_key) : nullptr;
        ctrl_write_block(ctrl, block, obj);
    }

    // 预览帧率 → 间隔。
    JsonValue lat = JsonValue::object();
    const int64_t fps = to_int64(json_field(prev_p, "fps"), 0);
    if (fps != 0) {
        int64_t interval = 1000 / fps;
        if (interval < 1) interval = 1;
        lat.set("preview_interval_ms", JsonValue::number(static_cast<double>(interval)));
    } else {
        lat.set("preview_interval_ms", JsonValue::number(66.0));
    }

    JsonValue out = JsonValue::object();
    const JsonValue& model_id = json_field(prof, "model_id");
    out.set("model_id", model_id.is_null() ? JsonValue::string("") : model_id);
    out.set("video_detection_confidence", json_field(inf, "confidence"));
    out.set("video_detection_iou", json_field(inf, "iou"));

    JsonValue capture_out = JsonValue::object();
    capture_out.set("device", JsonValue::string("/dev/video0"));
    capture_out.set("crop_size", json_field(cap, "width"));
    out.set("capture", std::move(capture_out));

    out.set("range_factor", JsonValue::number(fov_factor_web));

    const JsonValue& sens = json_field(mouse, "sensitivity");
    out.set("sens", sens.is_null() ? JsonValue::number(1.0) : sens);
    const JsonValue& oy = json_field(mouse, "offset_y");
    out.set("pos", oy.is_null() ? JsonValue::number(0.5) : oy);

    JsonValue ai = JsonValue::object();
    ai.set("controller", std::move(ctrl));
    out.set("ai", std::move(ai));

    out.set("aim_profiles", aim_profiles_to_web(mouse, inf));

    JsonValue recoil_out = JsonValue::object();
    recoil_out.set("enabled", JsonValue::boolean(json_truthy(json_field(recoil, "enabled"))));
    const JsonValue& r_hk = json_field(recoil, "hotkey");
    const int64_t r_hk_bits = r_hk.is_null() ? 1 : r_hk.as_int();
    const std::string r_hk_name = bits_to_hotkey(r_hk_bits);
    recoil_out.set("hotkey", JsonValue::string(r_hk_name.empty() ? "left" : r_hk_name));
    const JsonValue& r_hk2 = json_field(recoil, "hotkey2");
    recoil_out.set("hotkey2",
                   JsonValue::string(bits_to_hotkey(r_hk2.is_null() ? 0 : r_hk2.as_int())));
    const JsonValue& r_mode = json_field(recoil, "hotkey_mode");
    recoil_out.set("hotkey_mode", JsonValue::string(r_mode.as_int() == 2 ? "all" : "any"));
    out.set("recoil", std::move(recoil_out));

    out.set("hotkey_guard", hotkey_guard_to_web(json_field(mouse, "hotkey_guard")));

    JsonValue mouse_output = JsonValue::object();
    mouse_output.set("mode", JsonValue::string("full_passthrough"));
    out.set("mouse_output", std::move(mouse_output));

    out.set("latency", std::move(lat));
    out.set("fan_control", JsonValue::object());
    out.set("loopout_overlay", JsonValue::object());

    return out;
}

}  // namespace ttbox::core::web
