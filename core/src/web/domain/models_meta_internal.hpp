// models_meta_internal.hpp — models 域「非纯 IPC」收口路由的段内共享小工具。
//
// 自 models_meta.cpp 拆出（因超 300 行）。全部 inline，models_meta.cpp 与本头共用；
// 只做 ui_meta 读写 / 列表投影 / 转换前置检查，不含路由注册。
#pragma once

#include <cctype>
#include <cstdio>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

#include "common/Json.hpp"
#include "web/domain/domain_internal.hpp"
#include "web/domain/domain_routes.hpp"
#include "web/infra/ipc_client.hpp"

namespace ttbox::core::web {

// 转换任务对外状态（对齐 model_convert._convert_state_public 的 idle 默认值）。
inline JsonValue convert_state_public() {
    JsonValue s = JsonValue::object();
    s.set("state", JsonValue::string("idle"));
    s.set("error", JsonValue::string(""));
    s.set("model_id", JsonValue::string(""));
    s.set("started_at", JsonValue::number(0.0));
    s.set("finished_at", JsonValue::number(0.0));
    s.set("message", JsonValue::string(""));
    return s;
}

// ui_meta.json 允许持久化的字段白名单（对齐 _MODEL_UI_META_KEYS）。
inline const char* kModelUiMetaKeys[] = {"game_profile", "preset_name", "hailo_pipeline_depth",
                                         "remote_frame_format", "class_names", "description"};

inline std::string model_ui_meta_path(const std::string& model_id) {
    return join_path(join_path(models_root(), "installed"), model_id) + "/ui_meta.json";
}

inline JsonValue read_model_ui_meta(const std::string& model_id) {
    std::string text;
    if (!read_file(model_ui_meta_path(model_id), &text)) return JsonValue::object();
    const JsonParseResult r = json_parse(text);
    return (r.ok && r.value.is_object()) ? r.value : JsonValue::object();
}

// 对齐 _write_model_ui_meta：只合并白名单键，原子写（tmp + rename）。
inline void write_model_ui_meta(const std::string& model_id, const JsonValue& patch) {
    JsonValue cur = read_model_ui_meta(model_id);
    for (const char* k : kModelUiMetaKeys) {
        const JsonValue* v = patch.find(k);
        if (v != nullptr) cur.set(k, *v);
    }
    const std::string path = model_ui_meta_path(model_id);
    std::error_code ec;
    std::filesystem::create_directories(std::filesystem::path(path).parent_path(), ec);
    const std::string tmp = path + ".tmp";
    write_file(tmp, cur.dump());
    std::rename(tmp.c_str(), path.c_str());
}

// list_models 的 data 对象（models + selected/running/state）；MODEL_LIST 失败返回 null。
inline JsonValue list_models_data(IpcClient& ipc) {
    const JsonValue r = ipc.call("MODEL_LIST", JsonValue::object(), kIpcTimeoutDefaultMs);
    if (ipc_status(r) != 0) return JsonValue::null();
    const JsonValue* d = r.find("data");
    const JsonValue data = (d != nullptr && d->is_object()) ? *d : JsonValue::object();
    JsonValue out = JsonValue::object();
    out.set("models", models_view(ipc, data));
    out.set("selected_model_id", json_field(data, "selected_model_id"));
    out.set("running_model_id", json_field(data, "running_model_id"));
    out.set("state", json_field(data, "state"));
    return out;
}

// 对齐 _models_patch_response：校验模型存在 → 写 ui_meta → 返回最新列表；不可用返回 nullopt。
inline std::optional<JsonValue> models_patch_response(IpcClient& ipc, const std::string& model_id,
                                                      const JsonValue& patch) {
    const JsonValue ml = ipc.call("MODEL_LIST", JsonValue::object(), kIpcTimeoutDefaultMs);
    if (ipc_status(ml) != 0) return std::nullopt;
    const JsonValue* d = ml.find("data");
    const JsonValue data = (d != nullptr && d->is_object()) ? *d : JsonValue::object();
    bool known = false;
    const JsonValue& records = json_field(data, "models");
    if (records.is_array()) {
        for (const JsonValue& m : records.as_array()) {
            if (json_field(m, "model_id").as_string("") == model_id) {
                known = true;
                break;
            }
        }
    }
    if (!known) return std::nullopt;
    write_model_ui_meta(model_id, patch);
    JsonValue out = JsonValue::object();
    out.set("message", JsonValue::string("已保存"));
    out.set("model_id", JsonValue::string(model_id));
    const JsonValue list = list_models_data(ipc);
    if (list.is_object()) {
        for (const auto& [k, v] : list.as_object()) out.set(k, v);
    }
    return out;
}

// ONNX→RKNN 转换缺失依赖清单（对齐 convert_prerequisites，空 = 可转）。
inline std::vector<std::string> convert_prerequisites() {
    std::vector<std::string> missing;
    const std::string prefix = ttbox_prefix();
    const std::string py = prefix + "/venv-convert/bin/python";
    const std::string script = prefix + "/tools/converter/convert_onnx_to_rknn.py";
    const std::string calib = prefix + "/calib/imgs";
    if (!std::filesystem::exists(py)) missing.push_back("转换器解释器 " + py);
    if (!std::filesystem::exists(script)) missing.push_back("转换器脚本 " + script);
    std::error_code ec;
    if (!std::filesystem::is_directory(calib, ec)) {
        missing.push_back("校准数据集目录 " + calib);
    } else {
        bool has_img = false;
        for (const auto& entry : std::filesystem::directory_iterator(calib, ec)) {
            if (ec) break;
            const std::string name = entry.path().filename().string();
            const size_t dot = name.find_last_of('.');
            const std::string ext = dot == std::string::npos ? "" : name.substr(dot + 1);
            std::string lower;
            for (char c : ext) {
                lower.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
            }
            if (lower == "jpg" || lower == "jpeg" || lower == "png" || lower == "bmp" ||
                lower == "webp") {
                has_img = true;
                break;
            }
        }
        if (!has_img) missing.push_back("校准图片（" + calib + " 内无 jpg/png）");
    }
    return missing;
}

// 解析 body 里的整数字段（count / rknn_concurrency / hailo_pipeline_depth 共用）。
inline int int_field(const JsonValue& body, const char* key, int fallback) {
    const JsonValue& v = json_field(body, key);
    if (v.is_number()) return static_cast<int>(v.as_number());
    if (v.is_string()) {
        try {
            return std::stoi(v.as_string());
        } catch (...) {
            return fallback;
        }
    }
    return fallback;
}

}  // namespace ttbox::core::web
