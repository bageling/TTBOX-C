// models_meta.cpp — 模型域「非纯 IPC」收口路由（T05）。
//
// T03 只搬了 list / delete / select 三条纯 IPC 子集，其余 13 条（转换状态 / 设备码 /
// ui_meta 字段写 / 远端模型占位 / 上传）在此补齐。URL 一字未改，对齐 api/models.py。
// 上传（import / import-onnx 的 multipart 链路）按方案「非纯 IPC 下沉后续批次」诚实
// 回报 NOT_IMPLEMENTED / 501，不造假数据。段内共享小工具在 models_meta_internal.hpp。
#include "web/domain/domain_routes.hpp"

#include <cctype>
#include <cstdio>
#include <filesystem>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "common/Json.hpp"
#include "httplib.h"
#include "web/domain/domain_internal.hpp"
#include "web/domain/models_meta_internal.hpp"
#include "web/infra/ipc_client.hpp"

namespace ttbox::core::web {

void register_models_meta_routes(httplib::Server& svr, IpcClient& ipc) {
    svr.Get("/api/models/convert-status",
            [](const httplib::Request&, httplib::Response& res) {
                send_json(res, 200, true, convert_state_public(), "", "");
            });

    svr.Get("/api/models/device-code",
            [](const httplib::Request&, httplib::Response& res) {
                const std::string serial = cpu_serial();
                const std::string device_id =
                    !serial.empty() ? "opi-" + serial : "opi-ttbox-local";
                std::string compact;
                for (char c : device_id) {
                    if (c != '-') compact.push_back(c);
                }
                if (compact.size() > 40) compact.resize(40);
                JsonValue data = JsonValue::object();
                data.set("code", JsonValue::string("AIMK1_" + compact));
                data.set("device_fingerprint_hash", JsonValue::string(device_id));
                data.set("device_id", JsonValue::string(device_id));
                data.set("format", JsonValue::string("AIMK1"));
                send_json(res, 200, true, data, "", "");
            });

    svr.Post("/api/models/import",
             [](const httplib::Request&, httplib::Response& res) {
                 send_json(res, 200, false, JsonValue::object(),
                           "模型上传暂未迁移至 C++（multipart 上传链路后续批次）",
                           "NOT_IMPLEMENTED");
             });

    svr.Post("/api/models/import-onnx",
             [](const httplib::Request&, httplib::Response& res) {
                 const std::vector<std::string> missing = convert_prerequisites();
                 if (!missing.empty()) {
                     std::string joined;
                     for (size_t i = 0; i < missing.size(); ++i) {
                         if (i != 0) joined += "、";
                         joined += missing[i];
                     }
                     send_json(res, 501, false, JsonValue::object(),
                               "板端不具备 ONNX→RKNN 转换能力（缺少：" + joined +
                                   "）。请上传已转换好的 .rknn（走 /api/models/import），"
                                   "或补齐上述依赖后重试。",
                               "");
                     return;
                 }
                 send_json(res, 200, false, JsonValue::object(),
                           "ONNX 转换工作线程暂未迁移至 C++", "NOT_IMPLEMENTED");
             });

    svr.Post("/api/models/bind-preset",
             [&ipc](const httplib::Request& req, httplib::Response& res) {
                 const JsonValue body = parse_json_body(req);
                 const std::string model_id = json_field(body, "model_id").as_string("");
                 if (model_id.empty()) {
                     send_json(res, 400, false, JsonValue::object(), "model_id is required", "");
                     return;
                 }
                 JsonValue patch = JsonValue::object();
                 patch.set("preset_name", json_field(body, "preset_name"));
                 const auto r = models_patch_response(ipc, model_id, patch);
                 if (!r.has_value()) {
                     send_json(res, 404, false, JsonValue::object(), "模型不存在或不可用", "");
                     return;
                 }
                 JsonValue data = *r;
                 JsonValue model = JsonValue::object();
                 model.set("preset_name", json_field(body, "preset_name"));
                 data.set("model", std::move(model));
                 send_json(res, 200, true, data, "", "");
             });

    svr.Post("/api/models/remote-frame-format",
             [&ipc](const httplib::Request& req, httplib::Response& res) {
                 const JsonValue body = parse_json_body(req);
                 const std::string model_id = json_field(body, "model_id").as_string("");
                 if (model_id.empty()) {
                     send_json(res, 200, false, JsonValue::object(), "model_id is required", "");
                     return;
                 }
                 std::string fmt = json_field(body, "remote_frame_format").as_string("jpeg");
                 for (char& c : fmt) {
                     c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
                 }
                 if (fmt != "jpeg" && fmt != "nv12" && fmt != "h264") fmt = "jpeg";
                 JsonValue patch = JsonValue::object();
                 patch.set("remote_frame_format", JsonValue::string(fmt));
                 const auto r = models_patch_response(ipc, model_id, patch);
                 if (!r.has_value()) {
                     send_json(res, 404, false, JsonValue::object(), "模型不存在或不可用", "");
                     return;
                 }
                 JsonValue data = *r;
                 data.set("message", JsonValue::string("帧格式已保存"));
                 send_json(res, 200, true, data, "", "");
             });

    svr.Post("/api/models/rknn-concurrency",
             [&ipc](const httplib::Request& req, httplib::Response& res) {
                 const JsonValue body = parse_json_body(req);
                 const std::string model_id = json_field(body, "model_id").as_string("");
                 if (model_id.empty()) {
                     send_json(res, 400, false, JsonValue::object(), "model_id is required", "");
                     return;
                 }
                 const JsonValue& c = json_field(body, "count");
                 const int count = !c.is_null() ? int_field(body, "count", 0)
                                                : int_field(body, "rknn_concurrency", 0);
                 if (count < 1 || count > 3) {
                     send_json(res, 400, false, JsonValue::object(), "count 必须在 1~3 之间", "");
                     return;
                 }
                 JsonValue params = JsonValue::object();
                 params.set("model_id", JsonValue::string(model_id));
                 params.set("count", JsonValue::number(static_cast<double>(count)));
                 const JsonValue r =
                     ipc.call("MODEL_SET_CONCURRENCY", params, kIpcTimeoutDefaultMs);
                 if (ipc_status(r) != 0) {
                     send_json(res, 409, false, JsonValue::object(),
                               json_field(r, "error").as_string("设置并发失败"), "");
                     return;
                 }
                 JsonValue out = JsonValue::object();
                 out.set("message", JsonValue::string("并发已保存并生效"));
                 out.set("model_id", JsonValue::string(model_id));
                 out.set("rknn_concurrency", JsonValue::number(static_cast<double>(count)));
                 out.set("restart_required", JsonValue::boolean(false));
                 const JsonValue list = list_models_data(ipc);
                 if (list.is_object()) {
                     for (const auto& [k, v] : list.as_object()) out.set(k, v);
                 }
                 send_json(res, 200, true, out, "", "");
             });

    svr.Post("/api/models/hailo-pipeline-depth",
             [&ipc](const httplib::Request& req, httplib::Response& res) {
                 const JsonValue body = parse_json_body(req);
                 const std::string model_id = json_field(body, "model_id").as_string("");
                 if (model_id.empty()) {
                     send_json(res, 200, false, JsonValue::object(), "model_id is required", "");
                     return;
                 }
                 int depth = int_field(body, "hailo_pipeline_depth", 3);
                 if (depth < 1) depth = 1;
                 if (depth > 4) depth = 4;
                 JsonValue patch = JsonValue::object();
                 patch.set("hailo_pipeline_depth", JsonValue::number(static_cast<double>(depth)));
                 const auto r = models_patch_response(ipc, model_id, patch);
                 if (!r.has_value()) {
                     send_json(res, 404, false, JsonValue::object(), "模型不存在或不可用", "");
                     return;
                 }
                 JsonValue data = *r;
                 data.set("message", JsonValue::string("流水线深度已保存"));
                 send_json(res, 200, true, data, "", "");
             });

    svr.Post("/api/models/class-names",
             [&ipc](const httplib::Request& req, httplib::Response& res) {
                 const JsonValue body = parse_json_body(req);
                 const std::string model_id = json_field(body, "model_id").as_string("");
                 if (model_id.empty()) {
                     send_json(res, 400, false, JsonValue::object(), "model_id is required", "");
                     return;
                 }
                 const JsonValue& cn = json_field(body, "class_names");
                 if (!cn.is_array()) {
                     send_json(res, 400, false, JsonValue::object(), "class_names 必须是数组", "");
                     return;
                 }
                 JsonValue clean = JsonValue::array();
                 for (const JsonValue& n : cn.as_array()) {
                     const std::string s = n.as_string("");
                     if (!s.empty()) clean.push_back(JsonValue::string(s));
                 }
                 JsonValue patch = JsonValue::object();
                 patch.set("class_names", std::move(clean));
                 const auto r = models_patch_response(ipc, model_id, patch);
                 if (!r.has_value()) {
                     send_json(res, 404, false, JsonValue::object(), "模型不存在或不可用", "");
                     return;
                 }
                 JsonValue data = *r;
                 data.set("message", JsonValue::string("类别名称已保存"));
                 send_json(res, 200, true, data, "", "");
             });

    svr.Post("/api/remote/connect", [](const httplib::Request&, httplib::Response& res) {
        send_json(res, 200, false, JsonValue::object(), "请输入 Windows 电脑局域网 IP", "");
    });
    svr.Get("/api/remote/models", [](const httplib::Request&, httplib::Response& res) {
        send_json(res, 200, false, JsonValue::object(), "请输入 Windows 电脑局域网 IP", "");
    });
    svr.Post("/api/remote/import", [](const httplib::Request&, httplib::Response& res) {
        send_json(res, 200, false, JsonValue::object(), "请输入 Windows 电脑局域网 IP", "");
    });
    svr.Post("/api/remote/delete", [](const httplib::Request&, httplib::Response& res) {
        send_json(res, 200, false, JsonValue::object(), "请输入 Windows 电脑局域网 IP", "");
    });
}

}  // namespace ttbox::core::web
