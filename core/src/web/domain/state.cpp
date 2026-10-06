// state.cpp — 运行状态与配置域（/api/state、/api/config、/api/events）。
//
// 自 plugins/web/api/state.py 逐行为移植。URL 一字未改。
#include "web/domain/domain_routes.hpp"

#include <string>

#include "common/Json.hpp"
#include "httplib.h"
#include "web/domain/domain_internal.hpp"
#include "web/infra/ipc_client.hpp"
#include "web/translate/profile_translate.hpp"

namespace ttbox::core::web {

void register_state_routes(httplib::Server& svr, IpcClient& ipc) {
    svr.Get("/api/state", [&ipc](const httplib::Request& req, httplib::Response& res) {
        (void)req;
        const JsonValue state = collect_web_state(ipc);
        if (state.is_null()) {
            send_json(res, 503, false, JsonValue::object(), "Core 未运行，无法读取状态",
                      "core_offline");
            return;
        }
        res.status = 200;
        res.set_content(state.dump(), "application/json");
    });

    svr.Put("/api/config", [&ipc](const httplib::Request& req, httplib::Response& res) {
        const JsonValue body = parse_json_body(req);
        // 唯一来源 = Core IPC；Core 离线 ⇒ 503 fail-loud。
        JsonValue prof;
        std::string err;
        if (!get_runtime_profile(ipc, &prof, &err)) {
            send_json(res, 503, false, JsonValue::object(), "Core 未运行，无法读取配置",
                      "core_offline");
            return;
        }
        if (!body.is_object() || body.as_object().empty()) {
            send_json(res, 400, false, JsonValue::object(), "请求体为空，未保存", "");
            return;
        }
        JsonValue translated;
        try {
            translated = web_body_to_profile(body, &prof);
        } catch (const ConfigValidationError& e) {
            send_json(res, 400, false, JsonValue::object(), e.what(), "");
            return;
        }
        JsonValue merged = deep_merge_profile(prof, strip_empty_model_id(translated));
        normalize_profile_capture_size(&merged);
        JsonValue params = JsonValue::object();
        params.set("profile", merged);
        const JsonValue r = ipc.call("SET_CONFIG", params, kIpcTimeoutDefaultMs);
        if (ipc_status(r) != 0) {
            const std::string core_error = json_field(r, "error").as_string("");
            if (ipc_status(r) == 3) {
                const std::string detail = !core_error.empty() ? "；" + core_error : "";
                send_json(res, 503, false, JsonValue::object(),
                          "Core 未运行，配置未保存（请先启动 ttbox-core）" + detail,
                          "core_offline");
            } else {
                const std::string reason = !core_error.empty() ? core_error : "未知原因";
                send_json(res, 400, false, JsonValue::object(), "配置被 Core 拒绝：" + reason,
                          "core_error");
            }
            return;
        }
        JsonValue rr;
        std::string err2;
        if (!get_runtime_profile(ipc, &rr, &err2)) {
            send_json(res, 503, false, JsonValue::object(), "Core 未运行", "core_offline");
            return;
        }
        send_json(res, 200, true, profile_to_web(rr), "", "");
    });

    svr.Get("/api/config", [&ipc](const httplib::Request& req, httplib::Response& res) {
        (void)req;
        JsonValue prof;
        std::string err;
        if (!get_runtime_profile(ipc, &prof, &err)) {
            send_json(res, 503, false, JsonValue::object(), "Core 未运行", "core_offline");
            return;
        }
        send_json(res, 200, true, profile_to_web(prof), "", "");
    });

    svr.Get("/api/events", [](const httplib::Request& req, httplib::Response& res) {
        (void)req;
        JsonValue data = JsonValue::object();
        data.set("events", JsonValue::array());
        send_json(res, 200, true, data, "", "");
    });
}

}  // namespace ttbox::core::web
