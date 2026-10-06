// control.cpp — 控制流启停域（start/stop 走 RUNTIME_CONTROL）。
//
// 自 plugins/web/api/control.py 逐行为移植。URL 一字未改。
#include "web/domain/domain_routes.hpp"

#include <cctype>
#include <string>

#include "common/Json.hpp"
#include "httplib.h"
#include "web/domain/domain_internal.hpp"
#include "web/infra/ipc_client.hpp"

namespace ttbox::core::web {

namespace {

// 从 collect_web_state 信封里取 data；离线/无 data 时返回空对象（不崩）。
JsonValue state_data(const JsonValue& envelope) {
    const JsonValue* d = envelope.is_object() ? envelope.find("data") : nullptr;
    return (d != nullptr && d->is_object()) ? *d : JsonValue::object();
}

}  // namespace

void register_control_routes(httplib::Server& svr, IpcClient& ipc) {
    svr.Post("/api/control/start", [&ipc](const httplib::Request& req, httplib::Response& res) {
        (void)req;
        // 保持 Web 契约：无模型时返回 error=未导入模型。
        JsonValue prof;
        std::string err;
        if (!get_runtime_profile(ipc, &prof, &err)) {
            send_json(res, 503, false, JsonValue::object(), "Core 未运行，无法读取配置",
                      "core_offline");
            return;
        }
        if (json_field(prof, "model_id").as_string("").empty()) {
            send_json(res, 200, false, JsonValue::object(), "未导入模型", "");
            return;
        }
        JsonValue params = JsonValue::object();
        params.set("action", JsonValue::string("start"));
        const JsonValue r = ipc.call("RUNTIME_CONTROL", params, kIpcTimeoutRuntimeControlMs);
        if (ipc_status(r) != 0) {
            std::string msg = json_field(r, "error").as_string("启动失败");
            // 错误文案里没有模型/未导入信息 ⇒ 统一按「未导入模型」回（对齐 Python 兜底）。
            std::string lower = msg;
            for (char& c : lower) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            if (msg.find("模型") == std::string::npos && lower.find("model") == std::string::npos) {
                msg = "未导入模型";
            }
            send_json(res, 200, false, JsonValue::object(), msg, "");
            return;
        }
        send_json(res, 200, true, state_data(collect_web_state(ipc)), "", "");
    });

    svr.Post("/api/control/stop", [&ipc](const httplib::Request& req, httplib::Response& res) {
        (void)req;
        JsonValue params = JsonValue::object();
        params.set("action", JsonValue::string("stop"));
        const JsonValue r = ipc.call("RUNTIME_CONTROL", params, kIpcTimeoutRuntimeControlMs);
        // core 离线时 RUNTIME_CONTROL 返回 status=3 + error 前缀 ipc_unavailable，
        // 此时应 503 core_offline（对齐 Python CoreUnavailableError → 503），而非 200。
        if (ipc_status(r) != 0) {
            const std::string err = json_field(r, "error").as_string("");
            if (err.find("ipc_unavailable") != std::string::npos) {
                send_json(res, 503, false, JsonValue::object(), "Core 未运行", "core_offline");
                return;
            }
        }
        send_json(res, 200, ipc_status(r) == 0, state_data(collect_web_state(ipc)), "", "");
    });
}

}  // namespace ttbox::core::web
