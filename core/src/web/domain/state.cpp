// state.cpp — 运行状态与配置域（/api/state、/api/config、/api/events）。
//
// 自 plugins/web/api/state.py 逐行为移植。URL 一字未改。
#include "web/domain/domain_routes.hpp"

#include <string>

#include "common/Json.hpp"
#include "httplib.h"
#include "web/domain/domain_internal.hpp"
#include "web/infra/calibration_controller.hpp"  // config_write_lock()
#include "web/infra/ipc_client.hpp"
#include "web/translate/profile_translate.hpp"

namespace ttbox::core::web {

// 注册状态与配置域路由：/api/state、/api/config（GET/PUT）、/api/events。
void register_state_routes(httplib::Server& svr, IpcClient& ipc) {
    // GET /api/state：整块状态快照；core 离线时返回 503 core_offline。
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

    // PUT /api/config：把提交体翻译并与现有 RuntimeProfile 深合并后写回 core。
    svr.Put("/api/config", [&ipc](const httplib::Request& req, httplib::Response& res) {
        const JsonValue body = parse_json_body(req);

        // 读-改-写全序列的结果先在锁内算好，HTTP 回包放在锁外发。
        int out_status = 400;
        bool out_ok = false;
        JsonValue out_data = JsonValue::object();
        std::string out_msg;
        std::string out_code;
        {
            // ── 配置读-改-写必须持 config_write_lock：防止 64 线程 + 标定线程整份覆盖 ──
            //    （这正是「面板开关改了没反应」的根因：并发 SET 互相覆盖）
            std::lock_guard<std::recursive_mutex> lk(config_write_lock());
            // 唯一来源 = Core IPC；Core 离线 ⇒ 503 fail-loud。
            JsonValue prof;
            std::string err;
            if (!get_runtime_profile(ipc, &prof, &err)) {
                out_status = 503;
                out_msg = "Core 未运行，无法读取配置";
                out_code = "core_offline";
            } else if (!body.is_object() || body.as_object().empty()) {
                out_status = 400;
                out_msg = "请求体为空，未保存";
            } else {
                JsonValue translated;
                bool translated_ok = true;
                try {
                    translated = web_body_to_profile(body, &prof);
                } catch (const ConfigValidationError& e) {
                    translated_ok = false;
                    out_status = 400;
                    out_msg = e.what();
                }
                if (translated_ok) {
                    JsonValue merged = deep_merge_profile(prof, strip_empty_model_id(translated));
                    normalize_profile_capture_size(&merged);
                    JsonValue params = JsonValue::object();
                    params.set("profile", merged);
                    const JsonValue r = ipc.call("SET_CONFIG", params, kIpcTimeoutDefaultMs);
                    if (ipc_status(r) != 0) {
                        const std::string core_error = json_field(r, "error").as_string("");
                        if (ipc_status(r) == 3) {
                            const std::string detail = !core_error.empty() ? "；" + core_error : "";
                            out_status = 503;
                            out_msg = "Core 未运行，配置未保存（请先启动 ttbox-core）" + detail;
                            out_code = "core_offline";
                        } else {
                            const std::string reason = !core_error.empty() ? core_error : "未知原因";
                            out_status = 400;
                            out_msg = "配置被 Core 拒绝：" + reason;
                            out_code = "core_error";
                        }
                    } else {
                        JsonValue rr;
                        std::string err2;
                        if (!get_runtime_profile(ipc, &rr, &err2)) {
                            out_status = 503;
                            out_msg = "Core 未运行";
                            out_code = "core_offline";
                        } else {
                            out_status = 200;
                            out_ok = true;
                            out_data = profile_to_web(rr);
                        }
                    }
                }
            }
        }
        send_json(res, out_status, out_ok, out_data, out_msg, out_code);
    });

    // GET /api/config：读取 RuntimeProfile 并转成面板可读形态。
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

    // GET /api/events：事件流占位（恒返回空数组）。
    svr.Get("/api/events", [](const httplib::Request& req, httplib::Response& res) {
        (void)req;
        JsonValue data = JsonValue::object();
        data.set("events", JsonValue::array());
        send_json(res, 200, true, data, "", "");
    });
}

}  // namespace ttbox::core::web
