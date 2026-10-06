// models.cpp — 模型库域（纯 IPC 子集：list / delete / select）。
//
// 自 plugins/web/api/models.py 与 lib/state_snapshot.py::_models_view 逐行为移植。
// 上传/转换/ui_meta 文件写/远端模型等非纯 IPC 路径在 models_meta.cpp（T05 收口）。
#include "web/domain/domain_routes.hpp"

#include <string>
#include <utility>

#include "common/Json.hpp"
#include "httplib.h"
#include "web/domain/domain_internal.hpp"
#include "web/infra/ipc_client.hpp"
#include "web/translate/profile_translate.hpp"

namespace ttbox::core::web {

namespace {

// 解析 worker_cores 逗号串，数出 1/2/4 有效 token（对齐 model_ui_meta）。
int count_worker_cores(const std::string& wc) {
    int count = 0;
    std::string tok;
    for (char c : wc) {
        if (c == ',') {
            if (tok == "1" || tok == "2" || tok == "4") ++count;
            tok.clear();
        } else if (c != ' ' && c != '\t') {
            tok.push_back(c);
        }
    }
    if (tok == "1" || tok == "2" || tok == "4") ++count;
    return count;
}

// 界面并发 = Core 实际 worker 数：manifest worker_cores 优先，否则 GET_CONFIG 全局默认。
int effective_rknn_concurrency(IpcClient& ipc, const JsonValue& record) {
    std::string wc = json_field(record, "worker_cores").as_string("");
    while (!wc.empty() && (wc.back() == ' ' || wc.back() == '\t')) wc.pop_back();
    if (wc.empty()) {
        const JsonValue r = ipc.call("GET_CONFIG", JsonValue::object(), kIpcTimeoutDefaultMs);
        const JsonValue* d = (ipc_status(r) == 0) ? r.find("data") : nullptr;
        if (d != nullptr && d->is_object()) wc = json_field(*d, "worker_cores").as_string("");
    }
    const int count = count_worker_cores(wc);
    return (count >= 1 && count <= 3) ? count : 3;
}

// 记录级 status_name 投影：优先 status_name，否则按 status==2 判 installed/staging。
std::string record_status(const JsonValue& r) {
    const std::string name = json_field(r, "status_name").as_string("");
    if (!name.empty()) return name;
    return json_field(r, "status").as_int(-1) == 2 ? "installed" : "staging";
}

// MODEL_LIST data → 面板模型卡片数组（/api/state 与 /api/models/select 同源）。
JsonValue build_models_view(IpcClient& ipc, const JsonValue& ml_data) {
    JsonValue out = JsonValue::array();
    const JsonValue& records = json_field(ml_data, "models");
    if (!records.is_array()) return out;
    for (const JsonValue& mm : records.as_array()) {
        const std::string id = json_field(mm, "model_id").as_string("");
        const std::string label = json_field(mm, "label").as_string("");
        const std::string display = !label.empty() ? label : id;
        JsonValue card = JsonValue::object();
        card.set("id", JsonValue::string(id));
        card.set("model_id", JsonValue::string(id));
        card.set("name", JsonValue::string(display));
        card.set("display_name", JsonValue::string(display));
        card.set("label", JsonValue::string(label));
        card.set("version", json_field(mm, "version"));
        card.set("status", JsonValue::string(record_status(mm)));
        card.set("origin", json_field(mm, "origin"));
        card.set("backend", JsonValue::string("rknn"));
        card.set("enabled", JsonValue::boolean(true));
        card.set("imported", JsonValue::boolean(true));
        card.set("input_width", JsonValue::number(json_field(mm, "input_width").as_number(0.0)));
        card.set("input_height", JsonValue::number(json_field(mm, "input_height").as_number(0.0)));
        card.set("output_count", JsonValue::number(json_field(mm, "output_count").as_number(0.0)));
        card.set("class_count", JsonValue::number(json_field(mm, "class_count").as_number(0.0)));
        card.set("class_names", json_field(mm, "class_names").is_array()
                                    ? json_field(mm, "class_names")
                                    : JsonValue::array());
        card.set("rknn_concurrency",
                 JsonValue::number(static_cast<double>(effective_rknn_concurrency(ipc, mm))));
        card.set("importing", JsonValue::boolean(false));  // 导入事务非本批（无上传路径）
        out.push_back(std::move(card));
    }
    return out;
}

// /api/models 的详细卡片（list_models）。字段直投影，缺省补 null/默认。
JsonValue detailed_card(IpcClient& ipc, const JsonValue& r) {
    const std::string id = json_field(r, "model_id").as_string("");
    const std::string label = json_field(r, "label").as_string("");
    const std::string name = json_field(r, "name").as_string("");
    const std::string display = !name.empty() ? name : (!label.empty() ? label : id);
    JsonValue card = JsonValue::object();
    card.set("id", JsonValue::string(id));
    card.set("model_id", JsonValue::string(id));
    card.set("name", JsonValue::string(display));
    card.set("label", json_field(r, "label"));
    card.set("version", json_field(r, "version"));
    card.set("format", json_field(r, "format").is_null() ? JsonValue::string("rknn")
                                                          : json_field(r, "format"));
    card.set("path", json_field(r, "path"));
    card.set("status", JsonValue::string(
        !json_field(r, "record_status").as_string("").empty()
            ? json_field(r, "record_status").as_string("")
            : json_field(r, "status").as_string("")));
    card.set("status_code", json_field(r, "status_code"));
    card.set("failure_code", JsonValue::string(json_field(r, "failure_code").as_string("")));
    card.set("failure_message", JsonValue::string(json_field(r, "failure_message").as_string("")));
    const std::string checksum = json_field(r, "checksum").as_string("");
    const std::string sha = json_field(r, "sha256").as_string("");
    card.set("checksum", JsonValue::string(!checksum.empty() ? checksum : sha));
    card.set("origin", json_field(r, "origin"));
    card.set("created_at", json_field(r, "created_at"));
    card.set("updated_at", json_field(r, "updated_at"));
    card.set("backend", JsonValue::string("rknn"));
    card.set("input_width", json_field(r, "input_width"));
    card.set("input_height", json_field(r, "input_height"));
    card.set("input_layout", json_field(r, "input_layout"));
    card.set("input_dtype", json_field(r, "input_dtype"));
    card.set("quantization", json_field(r, "quantization"));
    card.set("output_format", json_field(r, "output_format"));
    card.set("output_count", json_field(r, "output_count"));
    card.set("class_count", json_field(r, "class_count"));
    card.set("class_names", json_field(r, "class_names").is_array()
                                ? json_field(r, "class_names")
                                : JsonValue::array());
    card.set("rknn_concurrency",
             JsonValue::number(static_cast<double>(effective_rknn_concurrency(ipc, r))));
    card.set("selected", JsonValue::boolean(json_truthy(json_field(r, "selected"))));
    card.set("running", JsonValue::boolean(json_truthy(json_field(r, "running"))));
    card.set("metadata", json_field(r, "metadata").is_object() ? json_field(r, "metadata")
                                                                : JsonValue::object());
    return card;
}

// 在卡片数组里找 id 匹配项；找不到返回 null。
JsonValue find_card(const JsonValue& cards, const std::string& id) {
    if (!cards.is_array()) return JsonValue::null();
    for (const JsonValue& c : cards.as_array()) {
        if (json_field(c, "id").as_string("") == id) return c;
    }
    return JsonValue::null();
}

}  // namespace

JsonValue models_view(IpcClient& ipc, const JsonValue& ml_data) {
    return build_models_view(ipc, ml_data);
}

void register_models_routes(httplib::Server& svr, IpcClient& ipc) {
    svr.Get("/api/models", [&ipc](const httplib::Request& req, httplib::Response& res) {
        (void)req;
        const JsonValue r = ipc.call("MODEL_LIST", JsonValue::object(), kIpcTimeoutDefaultMs);
        if (ipc_status(r) != 0) {
            send_json(res, 503, false, JsonValue::object(),
                      json_field(r, "error").as_string("ModelRegistry unavailable"), "");
            return;
        }
        const JsonValue* d = r.find("data");
        const JsonValue data = (d != nullptr && d->is_object()) ? *d : JsonValue::object();
        JsonValue models = JsonValue::array();
        const JsonValue& records = json_field(data, "models");
        if (records.is_array()) {
            for (const JsonValue& rec : records.as_array()) {
                models.push_back(detailed_card(ipc, rec));
            }
        }
        JsonValue out = JsonValue::object();
        out.set("models", std::move(models));
        out.set("selected_model_id", json_field(data, "selected_model_id"));
        out.set("running_model_id", json_field(data, "running_model_id"));
        out.set("state", json_field(data, "state"));
        send_json(res, 200, true, out, "", "");
    });

    svr.Post("/api/models/delete", [&ipc](const httplib::Request& req, httplib::Response& res) {
        const JsonValue body = parse_json_body(req);
        const std::string model_id = json_field(body, "model_id").as_string("");
        if (model_id.empty()) {
            send_json(res, 400, false, JsonValue::object(), "missing field: model_id", "");
            return;
        }
        JsonValue params = JsonValue::object();
        params.set("model_id", JsonValue::string(model_id));
        const JsonValue r = ipc.call("MODEL_REMOVE", params, kIpcTimeoutDefaultMs);
        if (ipc_status(r) != 0) {
            send_json(res, 200, false, JsonValue::object(),
                      json_field(r, "error").as_string("删除失败"), "");
            return;
        }
        JsonValue data = JsonValue::object();
        data.set("message", JsonValue::string("已删除"));
        send_json(res, 200, true, data, "", "");
    });

    svr.Post("/api/models/select", [&ipc](const httplib::Request& req, httplib::Response& res) {
        const JsonValue body = parse_json_body(req);
        const std::string model_id = json_field(body, "model_id").as_string("");
        if (model_id.empty()) {
            send_json(res, 400, false, JsonValue::object(), "missing field: model_id", "");
            return;
        }
        JsonValue params = JsonValue::object();
        params.set("model_id", JsonValue::string(model_id));
        const JsonValue act =
            ipc.call("MODEL_ACTIVATE", params, kIpcTimeoutModelActivateMs);
        if (ipc_status(act) != 0) {
            send_json(res, 409, false, JsonValue::object(),
                      json_field(act, "error").as_string("激活失败"), "");
            return;
        }
        const JsonValue ml = ipc.call("MODEL_LIST", JsonValue::object(), kIpcTimeoutDefaultMs);
        if (ipc_status(ml) != 0) {
            send_json(res, 503, false, JsonValue::object(),
                      json_field(ml, "error").as_string("ModelRegistry unavailable"), "");
            return;
        }
        const JsonValue* d = ml.find("data");
        const JsonValue data = (d != nullptr && d->is_object()) ? *d : JsonValue::object();
        const JsonValue cards = models_view(ipc, data);
        const std::string active_id =
            !json_field(data, "selected_model_id").as_string("").empty()
                ? json_field(data, "selected_model_id").as_string("")
                : model_id;
        JsonValue config_web;
        JsonValue prof;
        std::string err;
        if (get_runtime_profile(ipc, &prof, &err)) {
            config_web = profile_to_web(prof);
        } else {
            config_web = JsonValue::object();
        }
        JsonValue out = JsonValue::object();
        out.set("message", JsonValue::string("模型已切换，Core 已加载新模型并完成首帧验证"));
        out.set("restart_required", JsonValue::boolean(false));
        out.set("selected_model_id", JsonValue::string(active_id));
        out.set("running_model_id", json_field(data, "running_model_id"));
        out.set("state", json_field(data, "state"));
        out.set("models", cards);
        out.set("config", std::move(config_web));
        out.set("presets", preset_names());
        out.set("model", find_card(cards, active_id));
        send_json(res, 200, true, out, "", "");
    });
}

}  // namespace ttbox::core::web
