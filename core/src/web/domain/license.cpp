// license.cpp — 授权域（查询 / 激活 / 重置本地身份 / 全量恢复）。
//
// 自 plugins/web/api/license.py 逐行为移植。授权语义**零推导**：GET /api/license
// 只投影 GET_STATUS.license（license_block），激活走 IPC 交 core 落盘/执法
// （授权执法权在 core）。云端 card-login 复用 core 的 TtboxLicenseClient
// （在线授权层，需 TTBOX_CORE_BUILD_AUTH=ON + OpenSSL；默认构建诚实报 501）。
#include "web/domain/domain_routes.hpp"

#include <cctype>
#include <string>
#include <vector>

#include "common/Json.hpp"
#include "httplib.h"
#include "web/domain/domain_internal.hpp"
#include "web/domain/domain_payloads.hpp"
#include "web/infra/ipc_client.hpp"
#ifdef TTBOX_WEB_CLOUD_AUTH
#include "auth/TtboxLicenseClient.hpp"
#endif

namespace ttbox::core::web {

namespace {

// 卡号 → 可读短码（前 5 + **** + 后 4；过短整体 ****）。对齐 cloud_session.card_mask。
// 仅云端激活路径使用（AUTH=OFF 构建不引用），标 maybe_unused 避免 -Wunused-function。
[[maybe_unused]] std::string card_mask(const std::string& card_key) {
    std::string s = card_key;
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.front())) != 0) s.erase(s.begin());
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.back())) != 0) s.pop_back();
    if (s.empty()) return "";
    if (s.size() <= 9) return "****";
    return s.substr(0, 5) + "****" + s.substr(s.size() - 4);
}

// 真实板卡 MAC（/sys/class/net/<iface>/address）。读不到跳过。
std::vector<std::string> board_macs() {
    std::vector<std::string> out;
    for (const char* iface : {"enP3p49s0", "eth0", "end0", "wlan0"}) {
        std::string content;
        if (!read_file("/sys/class/net/" + std::string(iface) + "/address", &content)) continue;
        while (!content.empty() && std::isspace(static_cast<unsigned char>(content.back())) != 0) {
            content.pop_back();
        }
        out.push_back(content);
    }
    return out;
}

// /api/license 全量投影（license_payload）。授权语义逐字段来自 IPC，零推导。
JsonValue license_payload(IpcClient& ipc) {
    const std::string serial = cpu_serial();
    const std::string device_id = !serial.empty() ? "opi-" + serial : "opi-ttbox-local";

    JsonValue license_data = license_block(ipc);
    license_data.set("device_id", JsonValue::string(device_id));
    license_data.set("device_fingerprint_hash", JsonValue::string(device_id));

    const std::string brand = json_field(license_data, "ui_brand").as_string("ttbox");
    const JsonValue ui = ui_block(brand);
    const std::string ota_ver = ota_current_version(ipc);
    const std::string app_ver = !ota_ver.empty() ? ota_ver : kAppVersion;

    JsonValue macs = JsonValue::array();
    for (const std::string& m : board_macs()) macs.push_back(JsonValue::string(m));
    JsonValue cpu = JsonValue::object();
    cpu.set("Serial", JsonValue::string(serial));
    JsonValue binding = JsonValue::object();
    binding.set("board_mac_addresses", std::move(macs));
    binding.set("cpu", std::move(cpu));
    binding.set("schema", JsonValue::string("orangepi-board-v4"));
    JsonValue hw = JsonValue::object();
    hw.set("Serial", JsonValue::string(serial));
    JsonValue device = JsonValue::object();
    device.set("binding_hardware", std::move(binding));
    device.set("device_id", JsonValue::string(device_id));
    device.set("fingerprint_hash", JsonValue::string(device_id));
    device.set("hardware", std::move(hw));

    JsonValue out = JsonValue::object();
    out.set("app_version", JsonValue::string(app_ver));
    out.set("auto_start", auto_start_payload());
    out.set("core", core_state_payload());
    out.set("device", std::move(device));
    out.set("license", std::move(license_data));
    out.set("ui", ui);
    out.set("ui_brand", JsonValue::string(json_field(ui, "ui_brand").as_string("ttbox")));
    out.set("version", JsonValue::string(app_ver));
    out.set("cloud", cloud_license_subblock(machine_code(ipc)));
    return out;
}

// 激活成功响应体（离线卡 / 云端共用；expire_unix_ms 缺省 0）。
JsonValue activation_ok(IpcClient& ipc, const JsonValue& data, int64_t default_expire_ms) {
    JsonValue out = JsonValue::object();
    out.set("state", JsonValue::string(json_field(data, "state").as_string("valid")));
    out.set("activated", JsonValue::boolean(json_field(data, "activated").as_bool(true)));
    out.set("plan", JsonValue::string(json_field(data, "plan").as_string("none")));
    out.set("is_pro", JsonValue::boolean(json_field(data, "is_pro").as_bool(false)));
    out.set("ui_brand", JsonValue::string(json_field(data, "ui_brand").as_string("ttbox")));
    out.set("features", json_field(data, "features").is_array() ? json_field(data, "features")
                                                                : JsonValue::array());
    out.set("expire_unix_ms",
            JsonValue::number(static_cast<double>(json_field(data, "expire_unix_ms").as_int(default_expire_ms))));
    out.set("license", license_payload(ipc));
    return out;
}

}  // namespace

// 注册授权域路由：查询 / 激活 / 重置本地身份 / 全量恢复。
void register_license_routes(httplib::Server& svr, IpcClient& ipc) {
    // GET /api/license：全量授权投影（逐字段来自 core，零推导）。
    svr.Get("/api/license", [&ipc](const httplib::Request& req, httplib::Response& res) {
        (void)req;
        send_json(res, 200, true, license_payload(ipc), "", "");
    });

    // POST /api/license/activate：离线卡（`{` 信封）走 ACTIVATE_LICENSE；否则云端 card-login。
    svr.Post("/api/license/activate",
             [&ipc](const httplib::Request& req, httplib::Response& res) {
                 const JsonValue body = parse_json_body(req);
                 const std::string license_key = json_field(body, "license_key").as_string("");
                 if (license_key.empty()) {
                     send_json(res, 400, false, JsonValue::object(), "license_key is required", "");
                     return;
                 }
                 // ---- 存量离线卡路径（`{` 信封开头）→ ACTIVATE_LICENSE ----
                 size_t b = 0;
                 while (b < license_key.size() &&
                        std::isspace(static_cast<unsigned char>(license_key[b])) != 0) {
                     ++b;
                 }
                 if (b < license_key.size() && license_key[b] == '{') {
                     JsonValue params = JsonValue::object();
                     params.set("card", JsonValue::string(license_key));
                     const JsonValue r = ipc.call("ACTIVATE_LICENSE", params, 10000);
                     if (ipc_status(r) != 0) {
                         send_json(res, 400, false, JsonValue::object(),
                                   "激活被拒绝：" +
                                       json_field(r, "error").as_string("IPC 无响应（Core 未运行?）"),
                                   "");
                         return;
                     }
                     const JsonValue* d = r.find("data");
                     const JsonValue data = (d != nullptr && d->is_object()) ? *d
                                                                            : JsonValue::object();
                     send_json(res, 200, true, activation_ok(ipc, data, 0), "", "");
                     return;
                 }
                 // ---- 云端 card-login（主入口）----
                 const std::string mc = machine_code(ipc);
                 if (mc.empty()) {
                     send_json(res, 500, false, JsonValue::object(),
                               "无法读取设备指纹（cpu_serial）", "");
                     return;
                 }
#ifdef TTBOX_WEB_CLOUD_AUTH
                 ttbox::core::auth::TtboxLicenseClient client;
                 ttbox::core::auth::LicenseStatus status;
                 std::string err;
                 const bool req_ok = client.verify_once(license_key, mc, status, &err);
                 if (!req_ok) {
                     send_json(res, 502, false, JsonValue::object(), "授权服务器不可达：" + err, "");
                     return;
                 }
                 if (status.state != ttbox::core::auth::LicenseState::kValid) {
                     const std::string msg =
                         !status.last_error.empty() ? status.last_error : (err.empty() ? "授权被拒绝" : err);
                     send_json(res, 400, false, JsonValue::object(), msg, "");
                     return;
                 }
                 JsonValue params = JsonValue::object();
                 params.set("expire_unix_ms", JsonValue::number(static_cast<double>(status.expire_unix_ms)));
                 JsonValue feats = JsonValue::array();
                 for (const std::string& f : status.features) feats.push_back(JsonValue::string(f));
                 params.set("features", std::move(feats));
                 params.set("plan", JsonValue::string(!status.plan.empty() ? status.plan : "subscription"));
                 params.set("card_mask", JsonValue::string(card_mask(license_key)));
                 params.set("source", JsonValue::string("cloud"));
                 const JsonValue r = ipc.call("ACTIVATE_CLOUD", params, 10000);
                 if (ipc_status(r) != 0) {
                     send_json(res, 502, false, JsonValue::object(),
                               "云端验证成功但核心激活失败：" +
                                   json_field(r, "error").as_string("IPC 无响应（Core 未运行?）"),
                               "");
                     return;
                 }
                 const JsonValue* d = r.find("data");
                 const JsonValue data = (d != nullptr && d->is_object()) ? *d : JsonValue::object();
                 send_json(res, 200, true, activation_ok(ipc, data, status.expire_unix_ms), "", "");
#else
                 send_json(res, 501, false, JsonValue::object(),
                           "云端激活需要 TTBOX_CORE_BUILD_AUTH=ON 构建（在线授权层未编入）", "");
#endif
             });

    // POST /api/activation/reset-local-identity：授权正常时拒绝，未激活则下沉 T2.x。
    svr.Post("/api/activation/reset-local-identity",
             [&ipc](const httplib::Request& req, httplib::Response& res) {
                 (void)req;
                 const JsonValue lic = license_block(ipc);
                 if (json_field(lic, "activated").as_bool(false)) {
                     send_json(res, 400, false, JsonValue::object(),
                               "当前授权状态正常，已拒绝重置本地授权身份", "");
                 } else {
                     send_json(res, 409, false, JsonValue::object(),
                               "授权未激活（state=" + json_field(lic, "state").as_string("unactivated") +
                                   "）；本地重置下沉 T2.x",
                               "");
                 }
             });

    // GET /api/activation/full-recovery：回报是否允许全量恢复（实际执行下沉 T2.x）。
    svr.Get("/api/activation/full-recovery",
            [&ipc](const httplib::Request& req, httplib::Response& res) {
                (void)req;
                const JsonValue lic = license_block(ipc);
                const bool activated = json_field(lic, "activated").as_bool(false);
                JsonValue data = JsonValue::object();
                data.set("allowed", JsonValue::boolean(!activated));
                data.set("available", JsonValue::boolean(false));
                data.set("reason",
                         JsonValue::string(activated
                                               ? "当前授权和 daemon 状态正常，已拒绝本地全量恢复"
                                               : "授权未激活（state=" +
                                                     json_field(lic, "state").as_string("unactivated") +
                                                     "）；授权侧不阻止恢复，但本地全量恢复下沉 T2.x"));
                data.set("saved_at", JsonValue::string(""));
                data.set("size", JsonValue::number(0.0));
                data.set("source", JsonValue::string(""));
                data.set("version", JsonValue::string(""));
                send_json(res, 200, true, data, "", "");
            });

    // POST /api/activation/full-recovery：执行入口（授权正常时拒绝，否则下沉 T2.x）。
    svr.Post("/api/activation/full-recovery",
             [&ipc](const httplib::Request& req, httplib::Response& res) {
                 (void)req;
                 const JsonValue lic = license_block(ipc);
                 if (json_field(lic, "activated").as_bool(false)) {
                     send_json(res, 400, false, JsonValue::object(),
                               "当前授权和 daemon 状态正常，已拒绝本地全量恢复", "");
                 } else {
                     send_json(res, 409, false, JsonValue::object(),
                               "授权未激活（state=" + json_field(lic, "state").as_string("unactivated") +
                                   "）；本地全量恢复下沉 T2.x",
                               "");
                 }
             });
}

}  // namespace ttbox::core::web
