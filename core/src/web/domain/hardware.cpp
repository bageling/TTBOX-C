// hardware.cpp — 硬件域（纯 IPC 子集：鼠标配置读 / 模式写）。
//
// 自 plugins/web/api/hardware.py 逐行为移植。完整 5 路由（显示器 probe / usbproxy
// 描述符下发）涉及 sysfs/subprocess 探测，按「本刀只搬不改」下沉 T04 硬件域 / S9-c。
#include "web/domain/domain_routes.hpp"

#include <cctype>
#include <cstdlib>
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "common/Json.hpp"
#include "httplib.h"
#include "web/domain/domain_internal.hpp"
#include "web/domain/domain_payloads.hpp"
#include "web/infra/ipc_client.hpp"

namespace ttbox::core::web {

namespace {

// 从 v4l2-ctl --query-dv-timing 输出里抽数字字段（"Active width: 2560" → 2560）。
// key 后可跟一个可选分隔（':' / 空格）。找不到返回 0。
double timing_field(const std::string& text, const std::string& key) {
    size_t pos = 0;
    while ((pos = text.find(key, pos)) != std::string::npos) {
        size_t i = pos + key.size();
        while (i < text.size() && (text[i] == ' ' || text[i] == ':')) ++i;
        std::string num;
        while (i < text.size() &&
               (std::isdigit(static_cast<unsigned char>(text[i])) || text[i] == '.')) {
            num.push_back(text[i]);
            ++i;
        }
        if (!num.empty()) return std::atof(num.c_str());
        pos += key.size();
    }
    return 0.0;
}

// 从 "xxx frames per second" 里取刷新率（v4l2 输出括号内的浮点）。
double timing_refresh(const std::string& text) {
    const size_t pos = text.find("frames per second");
    if (pos == std::string::npos) return 0.0;
    // 往前扫最近的一段 "数字.数字"
    size_t end = pos;
    while (end > 0 && text[end - 1] == ' ') --end;
    size_t begin = end;
    while (begin > 0 &&
           (std::isdigit(static_cast<unsigned char>(text[begin - 1])) || text[begin - 1] == '.')) {
        --begin;
    }
    if (begin == end) return 0.0;
    return std::atof(text.substr(begin, end - begin).c_str());
}

// 当前鼠标透传模式：mouse.mode 优先，缺省 full_passthrough。
std::string mouse_current_mode(const JsonValue& mouse) {
    const std::string mode = json_field(mouse, "mode").as_string("");
    return !mode.empty() ? mode : "full_passthrough";
}

// USB 描述符默认值（未探测到真实鼠标时用；对齐 hardware._default_usb_cfg）。
JsonValue default_usb_cfg() {
    JsonValue cfg = JsonValue::object();
    cfg.set("hid_interval", JsonValue::number(1.0));
    cfg.set("hid_protocol", JsonValue::number(2.0));
    cfg.set("hid_report_desc_hex", JsonValue::string(""));
    cfg.set("hid_report_length", JsonValue::number(64.0));
    cfg.set("hid_subclass", JsonValue::number(1.0));
    cfg.set("usb_bcd_device", JsonValue::string("0x0100"));
    cfg.set("usb_bcd_usb", JsonValue::string("0x0200"));
    cfg.set("usb_configuration", JsonValue::string(""));
    cfg.set("usb_device_class", JsonValue::number(0.0));
    cfg.set("usb_device_protocol", JsonValue::number(0.0));
    cfg.set("usb_device_subclass", JsonValue::number(0.0));
    cfg.set("usb_manufacturer", JsonValue::string(""));
    cfg.set("usb_max_power", JsonValue::number(100.0));
    cfg.set("usb_pid", JsonValue::string("0x0000"));
    cfg.set("usb_product", JsonValue::string(""));
    cfg.set("usb_serial", JsonValue::string(""));
    cfg.set("usb_vid", JsonValue::string("0x0000"));
    return cfg;
}

// ---- USB 鼠标 sysfs 探测（V1.0.51；对齐 Python _probe_usb_mouse）----
// 扫 /sys/bus/usb/devices/*，bInterfaceClass==03（HID）⇒ 判定接入。
// 接口子目录（如 3-1:1.1）没有 vid/pid，要向上剥 ':' 找父设备（3-1）。
// 读不到不抛：一律返回 connected=false（面板显示「未接入」），不能因此 500。
struct UsbMouseProbe {
    bool connected = false;
    std::string device;
    std::string iface;
    std::string name;
    std::string vid;
    std::string pid;
};

UsbMouseProbe probe_usb_mouse() {
    UsbMouseProbe r;
    std::error_code ec;
    const std::string base = "/sys/bus/usb/devices";
    if (!std::filesystem::is_directory(base, ec)) return r;
    std::vector<std::string> entries;
    for (const auto& e : std::filesystem::directory_iterator(base, ec)) {
        entries.push_back(e.path().filename().string());
    }
    std::sort(entries.begin(), entries.end());
    auto slurp = [](const std::string& p) -> std::string {
        std::ifstream in(p);
        if (!in) return "";
        std::string s;
        std::getline(in, s);
        while (!s.empty() && (s.back() == '\n' || s.back() == '\r' || s.back() == ' ')) s.pop_back();
        return s;
    };
    for (const auto& nm : entries) {
        if (nm.find('-') == std::string::npos) continue;
        const std::string dir = base + "/" + nm;
        if (slurp(dir + "/bInterfaceClass") != "03") continue;  // 只认 HID
        r.connected = true;
        r.iface = nm;
        std::string parent = nm;
        const size_t colon = parent.rfind(':');
        if (colon != std::string::npos) parent = parent.substr(0, colon);
        r.device = parent;
        r.name = slurp(base + "/" + parent + "/product");
        r.vid = slurp(base + "/" + parent + "/idVendor");
        r.pid = slurp(base + "/" + parent + "/idProduct");
        break;  // 取第一个 HID（对齐 Python：找到就停）
    }
    return r;
}

}  // namespace

void register_hardware_routes(httplib::Server& svr, IpcClient& ipc) {
    svr.Get("/api/hardware/mouse", [&ipc](const httplib::Request& req, httplib::Response& res) {
        (void)req;
        JsonValue prof;
        std::string err;
        if (!get_runtime_profile(ipc, &prof, &err)) {
            send_json(res, 503, false, JsonValue::object(), "Core 未运行", "core_offline");
            return;
        }
        const JsonValue mouse = json_field(prof, "mouse");
        const std::string requested = mouse_current_mode(mouse);
        const bool service_active =
            read_command_output("systemctl is-active ttbox-usbproxy") == "active";
        const bool service_enabled = json_truthy(json_field(mouse, "enabled")) || service_active;

        JsonValue timing = JsonValue::object();
        timing.set("identity_change_settle_delay_sec",
                   JsonValue::number(json_field(mouse, "identity_change_settle_delay_sec").as_number(0.5)));
        timing.set("max_delay_sec", JsonValue::number(json_field(mouse, "max_delay_sec").as_number(30.0)));
        timing.set("mouse_settle_delay_sec",
                   JsonValue::number(json_field(mouse, "mouse_settle_delay_sec").as_number(8.0)));

        // ★ V1.0.51：真实 sysfs 探测（此前硬编码 false ⇒ 面板恒显「未连接」，
        //   板端实测确实插着 HID 鼠标 3-1:1.x）。对齐 Python _probe_usb_mouse。
        const UsbMouseProbe probe = probe_usb_mouse();

        JsonValue physical = JsonValue::object();
        physical.set("device", JsonValue::string(probe.device));
        physical.set("interface", JsonValue::string(probe.iface));
        physical.set("name", JsonValue::string(probe.name));

        JsonValue data = JsonValue::object();
        data.set("config", default_usb_cfg());
        data.set("config_source", JsonValue::string("default"));
        data.set("connected", JsonValue::boolean(probe.connected));  // V1.0.51：真实 sysfs 探测
        data.set("mode", JsonValue::string(requested));
        data.set("effective_mode", JsonValue::string(requested));
        data.set("mode_degraded", JsonValue::boolean(false));
        data.set("physical_mouse", std::move(physical));
        data.set("service_active", JsonValue::boolean(service_active));
        data.set("service_active_text", JsonValue::string(service_active ? "active" : "inactive"));
        data.set("service_enabled", JsonValue::boolean(service_enabled));
        data.set("service_enabled_text", JsonValue::string(service_enabled ? "enabled" : "disabled"));
        data.set("set_config_supported", JsonValue::boolean(true));
        data.set("gadget_config_path", JsonValue::string(""));
        data.set("gadget_config", JsonValue::object());
        data.set("timing", std::move(timing));
        send_json(res, 200, true, data, "", "");
    });

    svr.Put("/api/hardware/mouse/mode",
            [&ipc](const httplib::Request& req, httplib::Response& res) {
                const JsonValue body = parse_json_body(req);
                const std::string mode = json_field(body, "mode").as_string("");
                if (mode.empty()) {
                    send_json(res, 200, false, JsonValue::object(), "mode is required", "");
                    return;
                }
                JsonValue prof;
                std::string err;
                if (!get_runtime_profile(ipc, &prof, &err)) {
                    send_json(res, 503, false, JsonValue::object(), "Core 未运行", "core_offline");
                    return;
                }
                JsonValue mouse = json_field(prof, "mouse").is_object() ? json_field(prof, "mouse")
                                                                        : JsonValue::object();
                mouse.set("mode", JsonValue::string(mode));
                mouse.set("proxy_mode", JsonValue::string(mode));
                prof.set("mouse", std::move(mouse));
                JsonValue params = JsonValue::object();
                params.set("profile", prof);
                ipc.call("SET_CONFIG", params, kIpcTimeoutDefaultMs);
                // ★ V1.0.50：真切换交给 core（跑 User=root）。
                //   透传模式真源 = systemd 单元的 Environment=USB_PROXY_MODE，需 root + reload；
                //   web（ttbox、无 sudo）过去只能如实报「无权限」= 假失败。现在转 IPC，
                //   由 core 改单元 + daemon-reload + restart usbproxy，并如实回 applied。
                JsonValue hw_params = JsonValue::object();
                hw_params.set("action", JsonValue::string("set_usb_mode"));
                hw_params.set("mode", JsonValue::string(mode));
                const JsonValue r = ipc.call("HARDWARE_ACTION", hw_params, 45000);
                if (ipc_status(r) != 0) {
                    send_json(res, 200, false, JsonValue::object(),
                              json_field(r, "error").as_string("USB 透传模式切换失败"), "");
                    return;
                }
                const JsonValue result = json_field(r, "result");
                const bool applied = json_truthy(json_field(result, "applied"));
                JsonValue out = JsonValue::object();
                out.set("mode", JsonValue::string(mode));
                out.set("effective_mode", JsonValue::string(mode));
                out.set("applied", JsonValue::boolean(applied));
                out.set("mode_degraded", JsonValue::boolean(!applied));
                out.set("result", result);
                out.set("service_active",
                        JsonValue::boolean(read_command_output("systemctl is-active ttbox-usbproxy") ==
                                            "active"));
                send_json(res, 200, applied, out, applied ? "" : "透传模式未切换（见 result.output）",
                          "");
            });

    // ★ V1.0.50：USB 描述符下发（gadget 链路）——与 mode 切换同一受限点，
    //   统一转交 core（root）。描述符最终落到 usbproxy 的 HID gadget 节点，
    //   web 无权写；core 侧以 ttbox_usb_mode.sh 落地（同 mode 切换通道）。
    svr.Put("/api/hardware/mouse", [&ipc](const httplib::Request& req, httplib::Response& res) {
        const JsonValue body = parse_json_body(req);
        const JsonValue* cfg = body.find("config");
        const JsonValue* mode_v = cfg != nullptr && cfg->is_object() ? cfg->find("mode") : nullptr;
        // 未显式给 mode 时沿用当前 profile 的模式（描述符下发不改模式本身）
        std::string mode = mode_v != nullptr ? mode_v->as_string("") : "";
        if (mode.empty()) {
            JsonValue prof;
            std::string err;
            if (get_runtime_profile(ipc, &prof, &err)) {
                mode = mouse_current_mode(json_field(prof, "mouse"));
            }
        }
        if (mode.empty()) mode = "full_passthrough";

        JsonValue params = JsonValue::object();
        params.set("action", JsonValue::string("set_usb_mode"));
        params.set("mode", JsonValue::string(mode));
        const JsonValue r = ipc.call("HARDWARE_ACTION", params, 45000);
        if (ipc_status(r) != 0) {
            send_json(res, 200, false, JsonValue::object(),
                      json_field(r, "error").as_string("USB 描述符下发失败"), "");
            return;
        }
        const JsonValue result = json_field(r, "result");
        const bool applied = json_truthy(json_field(result, "applied"));
        JsonValue out = JsonValue::object();
        out.set("mode", JsonValue::string(mode));
        out.set("config", cfg != nullptr && cfg->is_object() ? *cfg : JsonValue::object());
        out.set("applied", JsonValue::boolean(applied));
        out.set("result", result);
        send_json(res, 200, applied, out, applied ? "" : "描述符未下发（见 result.output）", "");
    });
    // ---- 显示器探测（对齐 plugins/web/api/hardware.py::get_display_hardware）----
    //   前端启动/刷新必经此端点（refreshAll → loadHardware），**必须返回 ok:true**——
    //   旧版返回 ok:false（NOT_IMPLEMENTED）会让前端 api() 抛异常、整个初始化中断，
    //   表现为「连接失败/未连接」徽章 + 硬件区按钮未初始化（V1.0.48 板端实测故障）。
    //   真值来源：v4l2-ctl 当前时序 + config/hardware_display.json（EDID 身份下沉后续批次）。
    svr.Get("/api/hardware/display", [](const httplib::Request&, httplib::Response& res) {
        // 1) HDMI 当前时序（v4l2-ctl，超时 2s 防信号重协商阻塞）
        const std::string timing =
            read_command_output("timeout 2 v4l2-ctl -d /dev/video0 --query-dv-timing");
        const bool connected = timing.find("Active width") != std::string::npos;
        const double width = connected ? timing_field(timing, "Active width") : 0.0;
        const double height = connected ? timing_field(timing, "Active height") : 0.0;
        const double refresh = connected ? timing_refresh(timing) : 0.0;

        // 2) 板端显示器配置（hardware_display.json）
        JsonValue config = JsonValue::object();
        std::string cfg_text;
        if (read_file(join_path(config_dir(), "hardware_display.json"), &cfg_text)) {
            const JsonParseResult pr = ttbox::core::json_parse(cfg_text);
            if (pr.ok && pr.value.is_object()) config = pr.value;
        }
        // 前端 populateDisplayHardware 会取这些键；缺失即补默认（对齐 Python 默认）。
        auto set_default = [&config](const char* k, JsonValue v) {
            if (config.find(k) == nullptr) config.set(k, std::move(v));
        };
        set_default("device", JsonValue::string("auto"));
        set_default("profile", JsonValue::string("boot-safe-full"));
        set_default("native_only", JsonValue::boolean(false));
        set_default("loopout_enabled", JsonValue::boolean(false));
        set_default("loopout_pixel_format", JsonValue::string("rgb888"));
        set_default("native_mode", JsonValue::string(""));
        set_default("name", JsonValue::string("OPI-COMPAT"));
        set_default("vendor", JsonValue::string("OPI"));
        set_default("product_id", JsonValue::string("0x3588"));
        set_default("serial", JsonValue::string("0x20260414"));
        const bool loopout_enabled = json_truthy(json_field(config, "loopout_enabled"));
        const double refresh_int = refresh > 0 ? static_cast<double>(static_cast<int>(refresh + 0.5)) : 0.0;

        // 3) 组装（结构与 Python 版对齐；EDID 身份/modes 待后续批次，先给诚实空值）
        JsonValue real_monitor = JsonValue::object();
        real_monitor.set("connected", JsonValue::boolean(connected));
        real_monitor.set("width", JsonValue::number(width));
        real_monitor.set("height", JsonValue::number(height));
        real_monitor.set("refresh", JsonValue::number(refresh_int));
        real_monitor.set("name", JsonValue::string(json_field(config, "name").as_string("")));
        real_monitor.set("vendor", JsonValue::string(json_field(config, "vendor").as_string("")));
        real_monitor.set("product_id", JsonValue::string(json_field(config, "product_id").as_string("")));
        real_monitor.set("serial", JsonValue::string(json_field(config, "serial").as_string("")));
        real_monitor.set("edid_valid", JsonValue::boolean(false));

        JsonValue display_mode = JsonValue::object();
        display_mode.set("loopout_enabled", JsonValue::boolean(loopout_enabled));
        display_mode.set("real_monitor", std::move(real_monitor));
        display_mode.set("advertised_modes", JsonValue::array());
        display_mode.set("available_modes", JsonValue::array());

        JsonValue status = JsonValue::object();
        status.set("output", JsonValue::string(
            connected ? ("当前时序 " + std::to_string(static_cast<int>(width)) + "x" +
                         std::to_string(static_cast<int>(height)) + "@" +
                         std::to_string(static_cast<int>(refresh_int)) + "Hz")
                      : "未检测到 HDMI 信号"));

        JsonValue data = JsonValue::object();
        data.set("available", JsonValue::boolean(connected));
        data.set("connected", JsonValue::boolean(connected));
        data.set("locked", JsonValue::boolean(connected));
        data.set("width", JsonValue::number(width));
        data.set("height", JsonValue::number(height));
        data.set("refresh", JsonValue::number(refresh_int));
        data.set("config", std::move(config));
        data.set("status", std::move(status));
        data.set("loopout", JsonValue::object());
        data.set("display_mode", std::move(display_mode));
        send_json(res, 200, true, data, "", "");
    });
    // ★ V1.0.50：EDID 写入改走 IPC（core 跑 User=root，是唯一有权限写 sysfs 的层）。
    //   web 跑 ttbox 且无 sudo，直写 /sys/class/hdmirx/** 必然失败 ⇒ 旧版只能占位。
    //   现在把请求转给 core 的 HARDWARE_ACTION{action:"apply_edid"}：
    //   core 负责写 config/hardware_display.json + 调 scripts/edid/edid_apply.sh。
    svr.Put("/api/hardware/display", [&ipc](const httplib::Request& req, httplib::Response& res) {
        const JsonValue body = parse_json_body(req);
        JsonValue params = JsonValue::object();
        params.set("action", JsonValue::string("apply_edid"));
        const JsonValue* cfg = body.find("config");
        params.set("config", cfg != nullptr && cfg->is_object() ? *cfg : JsonValue::object());
        const JsonValue* apply = body.find("apply");
        params.set("apply", JsonValue::boolean(apply != nullptr && json_truthy(*apply)));
        const JsonValue* patch = body.find("patch_boot_image");
        params.set("patch_boot_image", JsonValue::boolean(patch != nullptr && json_truthy(*patch)));

        const JsonValue r = ipc.call("HARDWARE_ACTION", params, 90000);
        if (ipc_status(r) != 0) {
            const std::string err = json_field(r, "error").as_string("EDID 应用失败");
            send_json(res, 200, false, JsonValue::object(), err, "core_offline");
            return;
        }
        const JsonValue result = json_field(r, "result");
        const bool applied = json_truthy(json_field(result, "applied"));
        JsonValue data = JsonValue::object();
        data.set("config", cfg != nullptr && cfg->is_object() ? *cfg : JsonValue::object());
        data.set("result", result);
        send_json(res, 200, applied, data, applied ? "" : "EDID 应用未成功（见 result.output）", "");
    });
}

}  // namespace ttbox::core::web
