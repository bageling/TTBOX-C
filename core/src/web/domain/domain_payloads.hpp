// domain_payloads.hpp — state/license 两域共用的品牌/自启/核心态/云端子块投影。
//
// 全部 inline：这些投影在 collect_web_state 与 license_payload 两处都要用，
// 拆出头文件避免 duplicate symbol。品牌注册表数据驱动（config/ui_brands.json）
// 下沉 T04 brand 域，此处返回默认原厂 ttbox 投影。
#pragma once

#include <cstdio>
#include <ctime>
#include <string>

#include "common/Json.hpp"

namespace ttbox::core::web {

// 执行命令并读取 stdout（去尾随空白）。失败/无输出 → 空串。
inline std::string read_command_output(const std::string& cmd) {
    FILE* p = ::popen((cmd + " 2>/dev/null").c_str(), "r");
    if (p == nullptr) return "";
    std::string out;
    char buf[256];
    while (std::fgets(buf, sizeof(buf), p) != nullptr) out += buf;
    ::pclose(p);
    while (!out.empty() &&
           (out.back() == '\n' || out.back() == '\r' || out.back() == ' ' ||
            out.back() == '\t')) {
        out.pop_back();
    }
    return out;
}

// Core 服务真实状态（systemctl is-active ttbox-core）；不硬编码（core_state.py）。
inline JsonValue core_state_payload() {
    JsonValue out = JsonValue::object();
    if (read_command_output("systemctl is-active ttbox-core") == "active") {
        out.set("installed", JsonValue::boolean(true));
        out.set("loaded", JsonValue::boolean(true));
        out.set("status", JsonValue::string("loaded"));
        out.set("message", JsonValue::string("核心模块已加载"));
        out.set("version", JsonValue::string("2026.05.16"));
    } else {
        out.set("installed", JsonValue::boolean(true));
        out.set("loaded", JsonValue::boolean(false));
        out.set("status", JsonValue::string("not_running"));
        out.set("message", JsonValue::string("核心模块未运行（ttbox-core 未启动）"));
        out.set("version", JsonValue::string("2026.05.16"));
    }
    return out;
}

// 开机自启投影（hw_payloads._auto_start_payload）。
inline JsonValue auto_start_payload() {
    const bool enabled = read_command_output("systemctl is-enabled ttbox-core") == "enabled";
    JsonValue out = JsonValue::object();
    out.set("enabled", JsonValue::boolean(enabled));
    if (!enabled) {
        out.set("status", JsonValue::string("disabled"));
        out.set("message", JsonValue::string(""));
        out.set("updated_at", JsonValue::number(0.0));
    } else {
        out.set("status", JsonValue::string("next_boot"));
        out.set("message", JsonValue::string("将在下次开机时自动启动"));
        out.set("updated_at", JsonValue::number(static_cast<double>(std::time(nullptr))));
    }
    return out;
}

// ui 子块 = 品牌表投影（branding._ui_block）。T03 返回默认原厂 ttbox 投影；
// 渠道皮肤注册表（config/ui_brands.json）下沉 T04 brand 域。
inline JsonValue ui_block(const std::string& /*brand*/) {
    JsonValue ui = JsonValue::object();
    ui.set("app_title", JsonValue::string("TTBOX 控制台"));
    ui.set("brand_name", JsonValue::string("TTBOX"));
    ui.set("brand_mark", JsonValue::string("TT"));
    ui.set("brand_eyebrow", JsonValue::string("TTBOX SYSTEM"));
    ui.set("brand_title", JsonValue::string("TTBOX 控制台"));
    ui.set("ui_brand", JsonValue::string("ttbox"));
    ui.set("skin", JsonValue::string("yu"));
    ui.set("default_theme", JsonValue::string("dark"));
    ui.set("allow_theme_switch", JsonValue::boolean(true));
    ui.set("default_local_name", JsonValue::string("ttbox"));
    ui.set("default_hotspot_ssid", JsonValue::string("TTBOX"));
    ui.set("fallback_reset_text", JsonValue::string("重置默认 Wi-Fi"));
    ui.set("brand_accent", JsonValue::string("#2F81F7"));
    ui.set("brand_logo", JsonValue::null());
    JsonValue theme = JsonValue::object();
    theme.set("mode", JsonValue::string("dark"));
    theme.set("accent", JsonValue::string("#2F81F7"));
    ui.set("theme", std::move(theme));
    ui.set("static_prefix", JsonValue::string(""));
    return ui;
}

// 风扇 PWM/NPU 温度投影（hw_payloads._fan_control_payload）。T04 硬件域接管前
// 返回诚实默认（未探测），不假绿。
inline JsonValue fan_control_payload() {
    JsonValue out = JsonValue::object();
    out.set("control_available", JsonValue::boolean(false));
    out.set("enabled", JsonValue::boolean(false));
    out.set("fan_rpm", JsonValue::number(0.0));
    out.set("last_error", JsonValue::string(""));
    out.set("pwm_path", JsonValue::string(""));
    out.set("pwm_percent", JsonValue::number(0.0));
    out.set("pwm_raw", JsonValue::number(0.0));
    out.set("pwm_writable", JsonValue::boolean(false));
    out.set("source", JsonValue::string("npu"));
    out.set("source_label", JsonValue::string("NPU"));
    out.set("tachometer_available", JsonValue::boolean(false));
    out.set("temperature_celsius", JsonValue::number(0.0));
    out.set("updated_at_ms", JsonValue::number(0.0));
    return out;
}

// DRM loopout 投影（hw_payloads._loopout_payload）。T04 硬件域接管前诚实默认（禁用）。
inline JsonValue loopout_payload() {
    JsonValue out = JsonValue::object();
    out.set("active", JsonValue::boolean(false));
    out.set("available", JsonValue::boolean(false));
    out.set("connected", JsonValue::boolean(false));
    out.set("connector_id", JsonValue::number(0.0));
    out.set("crtc_id", JsonValue::number(0.0));
    out.set("drm_device", JsonValue::string("/dev/dri/card0"));
    out.set("drm_open", JsonValue::boolean(false));
    out.set("dropped", JsonValue::number(0.0));
    out.set("enabled", JsonValue::boolean(false));
    out.set("fps", JsonValue::number(0.0));
    out.set("frames", JsonValue::number(0.0));
    out.set("height", JsonValue::number(0.0));
    out.set("last_error", JsonValue::string(""));
    out.set("overlay_active", JsonValue::boolean(false));
    out.set("overlay_available", JsonValue::boolean(false));
    out.set("overlay_draw_ms", JsonValue::number(0.0));
    out.set("overlay_dropped", JsonValue::number(0.0));
    out.set("overlay_enabled", JsonValue::boolean(false));
    out.set("overlay_last_error", JsonValue::string(""));
    out.set("overlay_pixel_format", JsonValue::string(""));
    out.set("overlay_plane_id", JsonValue::number(0.0));
    out.set("overlay_plane_name", JsonValue::string(""));
    out.set("overlay_status", JsonValue::string("disabled"));
    out.set("overlay_updates", JsonValue::number(0.0));
    out.set("pixel_format", JsonValue::string("rgb888"));
    out.set("refresh", JsonValue::number(0.0));
    out.set("status", JsonValue::string("disabled"));
    out.set("width", JsonValue::number(0.0));
    return out;
}

// 自动标定状态机投影（calibration._calibration_payload['runtime']）。T05 标定域
// 接管前诚实默认（未运行）。
inline JsonValue calibration_runtime() {
    JsonValue out = JsonValue::object();
    out.set("running", JsonValue::boolean(false));
    out.set("phase", JsonValue::string(""));
    out.set("state", JsonValue::string("idle"));
    out.set("status", JsonValue::string("idle"));
    out.set("ready", JsonValue::boolean(false));
    out.set("reason", JsonValue::string(""));
    out.set("error", JsonValue::string(""));
    return out;
}

// 云端授权子块增量（core_state._cloud_license_subblock）。会话态下沉 T04 云端域，
// 此处诚实空（source:none / online:false / expire_at:''，不造假值）。
inline JsonValue cloud_license_subblock(const std::string& machine_code) {
    JsonValue out = JsonValue::object();
    out.set("source", JsonValue::string("none"));
    out.set("card_mask", JsonValue::string(""));
    out.set("expire_at", JsonValue::string(""));
    out.set("max_devices", JsonValue::number(0.0));
    JsonValue hb = JsonValue::object();
    hb.set("online", JsonValue::boolean(false));
    hb.set("last_ok_at", JsonValue::number(0.0));
    hb.set("interval", JsonValue::number(60.0));
    hb.set("timeout", JsonValue::number(180.0));
    hb.set("error", JsonValue::string(""));
    out.set("heartbeat", std::move(hb));
    out.set("machine_code", JsonValue::string(machine_code));
    return out;
}

}  // namespace ttbox::core::web
