// presets.cpp — 预设参数域（CRUD + load + import/export）。
//
// 自 plugins/web/api/presets.py 逐行为移植。URL 一字未改。预设本体落盘
// plugins/web/lib/paths.py::presets_dir()（TTBOX_PRESETS_DIR > <prefix>/presets）。
#include "web/domain/domain_routes.hpp"

#include <algorithm>
#include <filesystem>
#include <string>
#include <utility>
#include <vector>

#include "common/Json.hpp"
#include "httplib.h"
#include "web/domain/domain_internal.hpp"
#include "web/infra/ipc_client.hpp"
#include "web/translate/profile_translate.hpp"

namespace ttbox::core::web {

namespace {

// 预设名净化：非 [A-Za-z0-9_-] 及非 UTF-8 多字节 → '_'，截断 64 字符。
std::string sanitize_preset_name(const std::string& name) {
    std::string out;
    for (unsigned char c : name) {
        const bool ok = (c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z') ||
                        (c >= 'a' && c <= 'z') || c == '_' || c == '-' || c >= 0x80;
        out.push_back(ok ? static_cast<char>(c) : '_');
        if (out.size() >= 64) break;
    }
    return out;
}

// 可写用户预设名：非空且不以 `_`（保留名）开头。
bool is_preset_name(const std::string& name) {
    return !name.empty() && name[0] != '_';
}

// 预设文件路径（<presets_dir>/<safe>.json）。
std::string preset_path(const std::string& safe) {
    return join_path(presets_dir(), safe + ".json");
}

// 确保预设目录存在。
void ensure_presets_dir() {
    std::error_code ec;
    std::filesystem::create_directories(presets_dir(), ec);
}

// 读取预设 JSON 文件内容；失败返回 false。
bool read_preset_json(const std::string& path, JsonValue* out) {
    std::string text;
    if (!read_file(path, &text)) return false;
    const JsonParseResult r = ttbox::core::json_parse(text);
    if (!r.ok) return false;
    *out = r.value;
    return true;
}

}  // namespace

JsonValue preset_names() {
    ensure_presets_dir();
    std::vector<std::string> names;
    std::error_code ec;
    for (std::filesystem::directory_iterator it(presets_dir(), ec), end;
         !ec && it != end; it.increment(ec)) {
        std::error_code fec;
        if (!it->is_regular_file(fec)) continue;
        if (it->path().extension().string() != ".json") continue;
        const std::string stem = it->path().stem().string();
        if (is_preset_name(stem)) names.push_back(stem);
    }
    std::sort(names.begin(), names.end());
    JsonValue out = JsonValue::array();
    for (const std::string& n : names) out.push_back(JsonValue::string(n));
    return out;
}

void register_presets_routes(httplib::Server& svr, IpcClient& ipc) {
    svr.Get("/api/presets", [](const httplib::Request& req, httplib::Response& res) {
        (void)req;
        JsonValue data = JsonValue::object();
        data.set("presets", preset_names());
        send_json(res, 200, true, data, "", "");
    });

    svr.Post("/api/presets", [&ipc](const httplib::Request& req, httplib::Response& res) {
        const JsonValue body = parse_json_body(req);
        const std::string name = json_field(body, "name").as_string("");
        const std::string action = json_field(body, "action").as_string("save");
        if (name.empty()) {
            send_json(res, 200, false, JsonValue::object(), "缺少预设名", "");
            return;
        }
        ensure_presets_dir();
        const std::string safe = sanitize_preset_name(name);
        if (!is_preset_name(safe)) {
            send_json(res, 200, false, JsonValue::object(),
                      "预设名不能以 _ 开头（保留给自动生成的诊断文件，只读）", "");
            return;
        }
        const std::string pf = preset_path(safe);
        if (action == "delete") {
            std::error_code ec;
            std::filesystem::remove(pf, ec);
            JsonValue data = JsonValue::object();
            data.set("message", JsonValue::string("已删除"));
            send_json(res, 200, true, data, "", "");
            return;
        }
        if (action == "rename") {
            const std::string safe2 = sanitize_preset_name(json_field(body, "new_name").as_string(""));
            if (!is_preset_name(safe2)) {
                send_json(res, 200, false, JsonValue::object(),
                          "新预设名不能以 _ 开头（保留名，只读）", "");
                return;
            }
            std::string content = "{}";
            std::string existing;
            if (read_file(pf, &existing)) content = existing;
            write_file(preset_path(safe2), content);
            std::error_code ec;
            std::filesystem::remove(pf, ec);
            JsonValue data = JsonValue::object();
            data.set("message", JsonValue::string("已重命名"));
            send_json(res, 200, true, data, "", "");
            return;
        }
        JsonValue config = json_field(body, "config");
        if (config.is_null()) {
            // 仅传 name 时保存当前运行配置为预设（统一存 Web 前端格式 profile_to_web）。
            JsonValue prof;
            std::string err;
            if (get_runtime_profile(ipc, &prof, &err)) {
                config = profile_to_web(prof);
            } else {
                config = JsonValue::object();
            }
        }
        if (!write_file(pf, config.dump())) {
            send_json(res, 200, false, JsonValue::object(), "预设写入失败（" + pf + "）", "");
            return;
        }
        JsonValue data = JsonValue::object();
        data.set("name", JsonValue::string(name));
        send_json(res, 200, true, data, "", "");
    });

    svr.Post("/api/presets/load", [&ipc](const httplib::Request& req, httplib::Response& res) {
        const JsonValue body = parse_json_body(req);
        const std::string safe = sanitize_preset_name(json_field(body, "name").as_string(""));
        const std::string pf = preset_path(safe);
        JsonValue config;
        if (!read_preset_json(pf, &config)) {
            send_json(res, 200, false, JsonValue::object(), "failed to open " + pf, "");
            return;
        }
        if (!config.is_object() || config.as_object().empty()) {
            send_json(res, 200, false, JsonValue::object(), "预设内容为空", "");
            return;
        }
        // 兼容两种格式：Web 前端格式（video_detection_confidence/ai/aim_profiles）→ 翻译；
        // 旧 RuntimeProfile 结构（inference/mouse/fov/capture）→ 直接深合并。
        bool needs_translate = true;
        JsonValue translated;
        const bool is_runtime = config.find("inference") != nullptr || config.find("mouse") != nullptr ||
                                config.find("fov") != nullptr || config.find("capture") != nullptr;
        if (is_runtime) {
            needs_translate = false;
            translated = config;
        }
        if (needs_translate) {
            JsonValue prof;
            std::string err;
            if (!get_runtime_profile(ipc, &prof, &err)) {
                send_json(res, 503, false, JsonValue::object(), "Core 未运行，无法加载预设",
                          "core_offline");
                return;
            }
            try {
                translated = web_body_to_profile(config, &prof);
            } catch (const ConfigValidationError& e) {
                send_json(res, 400, false, JsonValue::object(), "预设内容非法：" + std::string(e.what()),
                          "");
                return;
            }
        }
        JsonValue prof;
        std::string err;
        if (!get_runtime_profile(ipc, &prof, &err)) {
            send_json(res, 503, false, JsonValue::object(), "Core 未运行，无法应用预设",
                      "core_offline");
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
                          "Core 未运行，配置未应用（请先启动 ttbox-core）" + detail, "core_offline");
            } else {
                const std::string reason = !core_error.empty() ? core_error : "未知原因";
                send_json(res, 400, false, JsonValue::object(), "预设被 Core 拒绝：" + reason,
                          "core_error");
            }
            return;
        }
        JsonValue data = JsonValue::object();
        data.set("message", JsonValue::string("已加载"));
        send_json(res, 200, true, data, "", "");
    });

    svr.Post("/api/presets/import", [](const httplib::Request& req, httplib::Response& res) {
        if (!req.has_file("file")) {
            send_json(res, 200, false, JsonValue::object(), "missing upload field: file", "");
            return;
        }
        const httplib::MultipartFormData f = req.get_file_value("file");
        const JsonParseResult pr = ttbox::core::json_parse(f.content);
        if (!pr.ok || !pr.value.is_object()) {
            send_json(res, 200, false, JsonValue::object(), "invalid preset file", "");
            return;
        }
        JsonValue data = pr.value;
        // 兼容旧版导出信封 {"ok":true,"data":{"preset":{...}}}：剥掉信封。
        const JsonValue* env = data.find("data");
        if (data.find("ok") != nullptr && env != nullptr && env->is_object() &&
            env->find("preset") != nullptr && env->find("preset")->is_object()) {
            data = *env->find("preset");
        }
        const std::string requested = req.get_param_value("name");
        const std::string file_stem =
            std::filesystem::path(f.filename).stem().string();
        std::string name = requested;
        if (name.empty()) name = json_field(data, "name").as_string("");
        if (name.empty()) name = file_stem;
        if (name.empty()) name = "imported";
        const std::string safe = sanitize_preset_name(name);
        if (!is_preset_name(safe)) {
            send_json(res, 400, false, JsonValue::object(),
                      "预设名非法（" + safe + " 开头的是系统保留名）", "");
            return;
        }
        ensure_presets_dir();
        if (!write_file(preset_path(safe), data.dump())) {
            send_json(res, 500, false, JsonValue::object(), "预设写入失败", "");
            return;
        }
        JsonValue out = JsonValue::object();
        out.set("name", JsonValue::string(safe));
        out.set("preset", data);
        send_json(res, 200, true, out, "", "");
    });

    svr.Get(R"(/api/presets/([^/]+)/export)",
            [](const httplib::Request& req, httplib::Response& res) {
                const std::string raw = req.matches.size() > 1 ? req.matches[1].str() : "";
                const std::string safe = sanitize_preset_name(raw);
                const std::string pf = preset_path(safe);
                JsonValue data;
                if (!read_preset_json(pf, &data)) {
                    send_json(res, 200, false, JsonValue::object(), "failed to open " + pf, "");
                    return;
                }
                res.status = 200;
                res.set_header("Content-Type", "application/json");
                res.set_header("Content-Disposition",
                               "attachment; filename=\"" + safe + ".json\"");
                res.set_content(data.dump(), "application/json");
            });
}

}  // namespace ttbox::core::web
