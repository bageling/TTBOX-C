// hardware.cpp — 硬件域（纯 IPC 子集：鼠标配置读 / 模式写）。
//
// 自 plugins/web/api/hardware.py 逐行为移植。完整 5 路由（显示器 probe / usbproxy
// 描述符下发）涉及 sysfs/subprocess 探测，按「本刀只搬不改」下沉 T04 硬件域 / S9-c。
#include "web/domain/domain_routes.hpp"

#include <string>

#include "common/Json.hpp"
#include "httplib.h"
#include "web/domain/domain_internal.hpp"
#include "web/domain/domain_payloads.hpp"
#include "web/infra/ipc_client.hpp"

namespace ttbox::core::web {

namespace {

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

        JsonValue physical = JsonValue::object();
        physical.set("device", JsonValue::string(""));
        physical.set("interface", JsonValue::string(""));
        physical.set("name", JsonValue::string(""));

        JsonValue data = JsonValue::object();
        data.set("config", default_usb_cfg());
        data.set("config_source", JsonValue::string("default"));
        data.set("connected", JsonValue::boolean(false));  // T04 接 sysfs 探测
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
                // 透传模式真源是 systemd 单元 Environment=USB_PROXY_MODE，需 root + reload；
                // web（ttbox 身份）做不到，如实报失败（对齐 Python 假落实修复）。
                send_json(res, 200, false, JsonValue::object(),
                          "切换 USB 透传模式需修改 systemd 单元的 USB_PROXY_MODE 并以 root "
                          "重载服务，Web 无此权限；请在板端运维通道执行",
                          "");
            });

    // ---- T05 收口：T03/T04 下沉的 3 条硬件域路由（sysfs/EDID/usbproxy 探测后续批次）----
    svr.Put("/api/hardware/mouse", [](const httplib::Request&, httplib::Response& res) {
        send_json(res, 200, false, JsonValue::object(),
                  "USB 描述符下发暂未迁移至 C++（usbproxy gadget 链路后续批次）",
                  "NOT_IMPLEMENTED");
    });
    svr.Get("/api/hardware/display", [](const httplib::Request&, httplib::Response& res) {
        send_json(res, 200, false, JsonValue::object(),
                  "显示器探测暂未迁移至 C++（EDID/sysfs 探测后续批次）",
                  "NOT_IMPLEMENTED");
    });
    svr.Put("/api/hardware/display", [](const httplib::Request&, httplib::Response& res) {
        send_json(res, 200, false, JsonValue::object(),
                  "显示器配置暂未迁移至 C++（EDID/sysfs 探测后续批次）",
                  "NOT_IMPLEMENTED");
    });
}

}  // namespace ttbox::core::web
