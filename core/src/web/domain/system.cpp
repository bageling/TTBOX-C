// system.cpp — 系统与主题域（15 条路由，URL 一字未改）。
//
// 自 plugins/web/api/system.py 逐行为移植。电源操作先问 systemd-logind
// （busctl CanReboot/CanPowerOff）再决定是否下承诺，不裸 systemctl reboot。
#include "web/domain/domain_routes.hpp"

#include <sys/wait.h>

#include <cctype>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <thread>

#include "common/Json.hpp"
#include "httplib.h"
#include "web/domain/domain_internal.hpp"
#include "web/domain/rootfs_internal.hpp"
#include "web/domain/sysinfo_internal.hpp"
#include "web/infra/ipc_client.hpp"

namespace ttbox::core::web {

namespace {

// 问 systemd-logind：当前身份能否 reboot/poweroff（对齐 _power_action_allowed）。
std::pair<bool, std::string> power_action_allowed(const std::string& verb) {
    const std::string method = (verb == "reboot") ? "CanReboot" : "CanPowerOff";
    const std::string out = run_quiet({"busctl", "call", "org.freedesktop.login1",
                                       "/org/freedesktop/login1", "org.freedesktop.login1",
                                       method});
    for (const char* token : {"yes", "no", "challenge"}) {
        const std::string needle = std::string("\"") + token + "\"";
        if (out.find(needle) != std::string::npos) {
            if (std::string(token) == "yes") return {true, ""};
            if (std::string(token) == "challenge") {
                return {false, "需要交互式授权（polkit challenge），Web 端无法代为确认"};
            }
            return {false, "当前账户无权执行该电源操作（polkit 拒绝）"};
        }
    }
    return {false, "无法查询 " + method + "（systemd-logind / busctl 不可用）"};
}

// 主机名校验（Python 只查长度；此处补 [A-Za-z0-9.-] 白名单以杜绝 shell 注入）。
bool valid_hostname(const std::string& h) {
    if (h.empty() || h.size() > 63) return false;
    for (unsigned char c : h) {
        if (!(std::isalnum(c) || c == '.' || c == '-')) return false;
    }
    return true;
}

// 执行命令，返回 (退出码, stdout+stderr 合并)；-1 = 无法执行。
std::pair<int, std::string> run_capture(const std::string& cmd) {
    FILE* p = ::popen((cmd + " 2>&1").c_str(), "r");
    if (p == nullptr) return {-1, ""};
    std::string out;
    char buf[256];
    while (std::fgets(buf, sizeof(buf), p) != nullptr) out += buf;
    const int st = ::pclose(p);
    int code = -1;
    if (st != -1 && WIFEXITED(st)) code = WEXITSTATUS(st);
    while (!out.empty() && std::isspace(static_cast<unsigned char>(out.back())) != 0) {
        out.pop_back();
    }
    return {code, out};
}

// 电源操作（reboot/poweroff 共用；对齐 power._power_action）。
void power_action(const httplib::Request& req, httplib::Response& res,
                  const std::string& verb) {
    const bool dry_run = json_truthy(json_field(parse_json_body(req), "dry_run"));
    const auto [allowed, why] = power_action_allowed(verb);
    JsonValue data = JsonValue::object();
    data.set("action", JsonValue::string(verb));
    data.set("scheduled", JsonValue::boolean(false));
    data.set("allowed", JsonValue::boolean(allowed));
    data.set("reason", JsonValue::string(why));
    if (dry_run) {
        send_json(res, 200, true, data, "", "");
        return;
    }
    if (!allowed) {
        send_json(res, 403, false, data, why, "");
        return;
    }
    std::thread([verb]() {
        std::this_thread::sleep_for(std::chrono::milliseconds(1500));
        const int rc = std::system(("systemctl " + verb).c_str());
        (void)rc;
    }).detach();
    data.set("scheduled", JsonValue::boolean(true));
    send_json(res, 200, true, data, "", "");
}

}  // namespace

void register_system_routes(httplib::Server& svr, IpcClient& ipc) {
    svr.Get("/api/announcement", [](const httplib::Request&, httplib::Response& res) {
        send_json(res, 503, false, JsonValue::object(), "announcement source unavailable", "");
    });

    svr.Get("/api/system", [](const httplib::Request&, httplib::Response& res) {
        send_json(res, 200, true, collect_system_stats(), "", "");
    });

    svr.Get("/api/system/storage", [](const httplib::Request& req, httplib::Response& res) {
        const std::string force = req.get_param_value("force");
        const bool force_b = !(force.empty() || force == "0" || force == "false");
        const JsonValue s = storage_payload();
        JsonValue data = JsonValue::object();
        data.set("free", json_field(s, "free"));
        data.set("path", JsonValue::string(ttbox_prefix()));
        data.set("percent", json_field(s, "percent"));
        data.set("root_free", json_field(s, "free"));
        data.set("root_percent", json_field(s, "percent"));
        data.set("root_total", json_field(s, "total"));
        data.set("root_used", json_field(s, "used"));
        data.set("rootfs", rootfs_expand_payload("status", force_b));
        send_json(res, 200, true, data, "", "");
    });

    svr.Post("/api/system/storage/expand", [](const httplib::Request&, httplib::Response& res) {
        const JsonValue payload = rootfs_expand_payload("expand", true);
        const bool expandable = json_truthy(json_field(payload, "expandable"));
        const bool action_available = json_truthy(json_field(payload, "action_available"));
        JsonValue data = JsonValue::object();
        data.set("rootfs", payload);
        if (!expandable) {
            send_json(res, 409, false, data, json_field(payload, "message").as_string(""), "");
            return;
        }
        if (!action_available) {
            send_json(res, 501, false, data,
                      "检测到可扩空间，但扩容执行通道尚未装箱（Web 不会修改分区表）", "");
            return;
        }
        send_json(res, 500, false, data, "扩容执行通道状态异常", "");
    });

    svr.Put("/api/system/hostname", [](const httplib::Request& req, httplib::Response& res) {
        const std::string hostname = json_field(parse_json_body(req), "hostname").as_string("");
        std::string trimmed = hostname;
        while (!trimmed.empty() &&
               std::isspace(static_cast<unsigned char>(trimmed.front())) != 0) {
            trimmed.erase(trimmed.begin());
        }
        while (!trimmed.empty() &&
               std::isspace(static_cast<unsigned char>(trimmed.back())) != 0) {
            trimmed.pop_back();
        }
        if (!valid_hostname(trimmed)) {
            send_json(res, 400, false, JsonValue::object(), "主机名无效", "");
            return;
        }
        // hostname 已过白名单（[A-Za-z0-9.-]），无 shell 注入面。
        const auto [rc, out] = run_capture("hostnamectl set-hostname " + trimmed);
        if (rc != 0) {
            send_json(res, 400, false, JsonValue::object(),
                      !out.empty() ? out : "设置失败", "");
            return;
        }
        send_json(res, 200, true, collect_network_summary(), "", "");
    });

    svr.Put("/api/system/web-port", [](const httplib::Request&, httplib::Response& res) {
        send_json(res, 400, false, JsonValue::object(),
                  "Web 控制台端口已固定为 8000，不支持在线修改", "");
    });

    svr.Post("/api/system/reactivate", [&ipc](const httplib::Request&, httplib::Response& res) {
        const JsonValue lic = license_block(ipc);
        if (json_truthy(json_field(lic, "activated"))) {
            send_json(res, 400, false, JsonValue::object(), "当前授权状态正常，无需修复授权", "");
        } else {
            send_json(res, 409, false, JsonValue::object(),
                      "授权未激活（state=" + json_field(lic, "state").as_string("unactivated") +
                          "）；在线修复下沉 T2.x",
                      "");
        }
    });

    svr.Post("/api/system/reboot", [](const httplib::Request& req, httplib::Response& res) {
        power_action(req, res, "reboot");
    });
    svr.Post("/api/system/poweroff", [](const httplib::Request& req, httplib::Response& res) {
        power_action(req, res, "poweroff");
    });

    svr.Get("/api/themes", [](const httplib::Request&, httplib::Response& res) {
        JsonValue theme = JsonValue::object();
        theme.set("active", JsonValue::boolean(true));
        theme.set("compatible", JsonValue::boolean(true));
        theme.set("description", JsonValue::string("系统内置主题，始终可用。"));
        theme.set("id", JsonValue::string("default"));
        theme.set("installed", JsonValue::boolean(true));
        theme.set("installed_version", JsonValue::string("built-in"));
        theme.set("latest_version", JsonValue::string("built-in"));
        theme.set("owned", JsonValue::boolean(true));
        theme.set("previews", JsonValue::array());
        theme.set("published", JsonValue::boolean(true));
        theme.set("title", JsonValue::string("TTBOX 默认主题"));
        theme.set("update_available", JsonValue::boolean(false));
        JsonValue themes = JsonValue::array();
        themes.push_back(std::move(theme));
        JsonValue data = JsonValue::object();
        data.set("active_theme_id", JsonValue::string("default"));
        data.set("active_version", JsonValue::string(""));
        data.set("offline", JsonValue::boolean(false));
        data.set("purchase_url", JsonValue::string(""));
        data.set("themes", std::move(themes));
        send_json(res, 200, true, data, "", "");
    });

    svr.Get(R"(/api/themes/([^/]+)/previews/([0-9]+))",
            [](const httplib::Request&, httplib::Response& res) {
                send_json(res, 200, true, JsonValue::object(), "", "");
            });

    svr.Post("/api/themes/redeem", [](const httplib::Request& req, httplib::Response& res) {
        const JsonValue body = parse_json_body(req);
        const std::string code = !json_field(body, "code").as_string("").empty()
                                     ? json_field(body, "code").as_string("")
                                     : json_field(body, "redeem_code").as_string("");
        if (code.empty()) {
            send_json(res, 400, false, JsonValue::object(), "请输入主题卡密", "");
            return;
        }
        send_json(res, 400, false, JsonValue::object(), "redeem failed: invalid code", "");
    });

    svr.Post(R"(/api/themes/([^/]+)/install)",
             [](const httplib::Request& req, httplib::Response& res) {
                 const JsonValue body = parse_json_body(req);
                 if (json_field(body, "download_url").as_string("").empty()) {
                     send_json(res, 400, false, JsonValue::object(),
                               "core download url is required", "");
                     return;
                 }
                 if (req.matches[1].str() != "default") {
                     send_json(res, 400, false, JsonValue::object(),
                               "theme not found: " + req.matches[1].str(), "");
                     return;
                 }
                 JsonValue data = JsonValue::object();
                 data.set("installed", JsonValue::boolean(true));
                 data.set("theme_id", JsonValue::string("default"));
                 send_json(res, 200, true, data, "", "");
             });

    svr.Put("/api/themes/current", [](const httplib::Request& req, httplib::Response& res) {
        const std::string theme_id =
            !json_field(parse_json_body(req), "theme_id").as_string("").empty()
                ? json_field(parse_json_body(req), "theme_id").as_string("")
                : "default";
        if (theme_id != "default") {
            send_json(res, 400, false, JsonValue::object(), "theme not found: " + theme_id, "");
            return;
        }
        JsonValue data = JsonValue::object();
        data.set("active_theme_id", JsonValue::string("default"));
        data.set("active_version", JsonValue::string(""));
        send_json(res, 200, true, data, "", "");
    });

    svr.Get(R"(/theme-assets/([^/]+)/([^/]+)/(.*))",
            [](const httplib::Request&, httplib::Response& res) {
                send_json(res, 200, true, JsonValue::object(), "", "");
            });
}

}  // namespace ttbox::core::web
