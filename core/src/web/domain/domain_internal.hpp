// domain_internal.hpp — 域层共享小工具（HTTP 信封 / IPC 读取 / 深合并 / 授权投影）。
//
// T03 起 7 个 domain/*.cpp 共用；全部 inline，避免额外 .cpp 与重复实现。
#pragma once

#include <cctype>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <fstream>
#include <sstream>
#include <string>
#include <utility>

#include "common/Json.hpp"
#include "httplib.h"
#include "web/domain/domain_paths_internal.hpp"
#include "web/infra/ipc_client.hpp"
#include "web/translate/controller_params.hpp"
#include "web/translate/profile_translate.hpp"

namespace ttbox::core::web {

// 面板对外版本（与 plugins/web/lib/settings.py::kAppVersion 同值）。
inline constexpr const char* kAppVersion = "2026.08.03.1";

// ---- HTTP 信封（附 D：{ok, data, error?, code?}）----
// 统一 HTTP 响应：按 {ok, data, error?, code?} 信封序列化并设置状态码。
inline void send_json(httplib::Response& res, int http_status, bool ok, const JsonValue& data,
                      const std::string& error, const std::string& code) {
    JsonValue body = JsonValue::object();
    body.set("ok", JsonValue::boolean(ok));
    body.set("data", data);
    if (!error.empty()) body.set("error", JsonValue::string(error));
    if (!code.empty()) body.set("code", JsonValue::string(code));
    res.status = http_status;
    res.set_content(body.dump(), "application/json");
}

// 读/写整文件。
// 读整文件到字符串；打不开返回 false。
inline bool read_file(const std::string& path, std::string* out) {
    std::ifstream in(path, std::ios::binary);
    if (!in.is_open()) return false;
    std::ostringstream ss;
    ss << in.rdbuf();
    *out = ss.str();
    return true;
}
// 写字符串到整文件；写失败返回 false。
inline bool write_file(const std::string& path, const std::string& content) {
    std::ofstream out(path, std::ios::binary);
    if (!out.is_open()) return false;
    out << content;
    out.flush();
    return out.good();
}

// 解析请求体为 JSON；空/非法 → 空对象（对齐 flask get_json(silent=True) 的宽容语义）。
inline JsonValue parse_json_body(const httplib::Request& req) {
    if (req.body.empty()) return JsonValue::object();
    JsonParseResult r = ttbox::core::json_parse(req.body);
    return r.ok ? r.value : JsonValue::object();
}

// IPC 响应 status（缺省按 3=INTERNAL，与 Core 不可达兜底一致）。
inline int ipc_status(const JsonValue& resp) {
    const JsonValue* s = resp.is_object() ? resp.find("status") : nullptr;
    return s != nullptr ? static_cast<int>(s->as_int(3)) : 3;
}

// GET_STATUS data；失败返回空对象（对齐 Python _get_status：失败不抛、返回 {}）。
inline JsonValue get_status_data(IpcClient& ipc) {
    const JsonValue resp = ipc.call("GET_STATUS", JsonValue::object(), kIpcTimeoutDefaultMs);
    if (ipc_status(resp) != 0) return JsonValue::object();
    const JsonValue* d = resp.find("data");
    return (d != nullptr && d->is_object()) ? *d : JsonValue::object();
}

// GET_CONFIG → data.runtime_profile；失败返回 false（fail-loud，对齐 Python CoreUnavailableError）。
inline bool get_runtime_profile(IpcClient& ipc, JsonValue* out, std::string* err) {
    const JsonValue resp = ipc.call("GET_CONFIG", JsonValue::object(), kIpcTimeoutDefaultMs);
    if (ipc_status(resp) != 0) {
        if (err != nullptr) {
            const JsonValue* e = resp.find("error");
            *err = e != nullptr ? e->as_string("unknown") : "unknown";
        }
        return false;
    }
    const JsonValue* d = resp.find("data");
    const JsonValue* prof = (d != nullptr && d->is_object()) ? d->find("runtime_profile") : nullptr;
    if (out != nullptr) *out = (prof != nullptr) ? *prof : JsonValue::object();
    return true;
}

// MODEL_LIST data；失败返回空对象。
inline JsonValue model_list_data(IpcClient& ipc) {
    const JsonValue resp = ipc.call("MODEL_LIST", JsonValue::object(), kIpcTimeoutDefaultMs);
    if (ipc_status(resp) != 0) return JsonValue::object();
    const JsonValue* d = resp.find("data");
    return (d != nullptr && d->is_object()) ? *d : JsonValue::object();
}

// RuntimeProfile 深合并：子对象（capture/fov/mouse/...）按键级合并而非整体替换。
inline JsonValue deep_merge_profile(const JsonValue& base, const JsonValue& patch) {
    JsonValue merged = base.is_object() ? base : JsonValue::object();
    if (!patch.is_object()) return merged;
    for (const auto& [k, v] : patch.as_object()) {
        const JsonValue* existing = merged.find(k);
        if (v.is_object() && existing != nullptr && existing->is_object()) {
            merged.set(k, deep_merge_profile(*existing, v));
        } else {
            merged.set(k, v);
        }
    }
    return merged;
}

// 读 cpu_serial（/proc/cpuinfo Serial）；读不到 → 空串。
inline std::string cpu_serial() {
    std::ifstream in("/proc/cpuinfo");
    if (!in.is_open()) return "";
    std::string line;
    while (std::getline(in, line)) {
        if (line.compare(0, 6, "Serial") != 0) continue;
        const size_t colon = line.find(':');
        if (colon == std::string::npos) return "";
        size_t b = colon + 1;
        size_t e = line.size();
        while (b < e && std::isspace(static_cast<unsigned char>(line[b])) != 0) ++b;
        while (e > b && std::isspace(static_cast<unsigned char>(line[e - 1])) != 0) --e;
        return line.substr(b, e - b);
    }
    return "";
}

// machine_code：cpu_serial；读不到回退 GET_STATUS.license.bind_device；再空 → ''。
inline std::string machine_code(IpcClient& ipc) {
    const std::string serial = cpu_serial();
    if (!serial.empty()) return serial;
    const JsonValue st = get_status_data(ipc);
    return json_field(json_field(st, "license"), "bind_device").as_string("");
}

// unix 秒 → ISO8601(UTC,'Z')；0/负 → ''（语义 = 无到期，非 1970）。
inline std::string unix_to_iso(int64_t sec) {
    if (sec <= 0) return "";
    const std::time_t t = static_cast<std::time_t>(sec);
    std::tm tm{};
#if defined(_WIN32)
    if (gmtime_s(&tm, &t) != 0) return "";
#else
    if (gmtime_r(&t, &tm) == nullptr) return "";
#endif
    // 钳制年份到 4 位：既保 ISO8601 格式稳定，又消除 -Wformat-truncation
    //（GCC 无法得知 gmtime 返回的 tm_year 有界，会按 int 全范围估计输出上限）。
    int year = tm.tm_year + 1900;
    if (year < 0) year = 0;
    if (year > 9999) year = 9999;
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%04d-%02d-%02dT%02d:%02d:%02dZ", year,
                  tm.tm_mon + 1, tm.tm_mday, tm.tm_hour, tm.tm_min, tm.tm_sec);
    return buf;
}

// GET_STATUS.license → Web license 子块投影（T1.07b：单一真相源，零推导）。
inline JsonValue license_block(IpcClient& ipc) {
    const JsonValue st = get_status_data(ipc);
    const JsonValue& lic = json_field(st, "license");

    auto make_default = []() {
        JsonValue out = JsonValue::object();
        out.set("activated", JsonValue::boolean(false));
        out.set("state", JsonValue::string("unactivated"));
        out.set("plan", JsonValue::string("none"));
        out.set("is_pro", JsonValue::boolean(false));
        out.set("features", JsonValue::array());
        out.set("expires_at", JsonValue::string(""));
        out.set("grace_until", JsonValue::string(""));
        out.set("message", JsonValue::string(""));
        out.set("heartbeat_interval_s", JsonValue::number(60.0));
        out.set("short_code", JsonValue::string(""));
        JsonValue caps = JsonValue::object();
        for (const char* k : {"capture", "inference", "aim", "ota"}) {
            caps.set(k, JsonValue::boolean(false));
        }
        out.set("capabilities", std::move(caps));
        out.set("ui_brand", JsonValue::string("ttbox"));
        out.set("status", JsonValue::string("unactivated"));
        out.set("valid", JsonValue::boolean(false));
        return out;
    };
    if (!lic.is_object() || lic.as_object().empty()) return make_default();

    const std::string state = json_field(lic, "state").as_string("unactivated");
    const bool activated = json_field(lic, "activated").as_bool(false);
    const JsonValue& feats = json_field(lic, "features");
    JsonValue features = feats.is_array() ? feats : JsonValue::array();
    const int64_t heartbeat = json_field(lic, "heartbeat_interval_s").as_int(60);

    const JsonValue& caps_raw = json_field(lic, "capabilities");
    JsonValue caps = JsonValue::object();
    for (const char* k : {"capture", "inference", "aim", "ota"}) {
        caps.set(k, JsonValue::boolean(json_truthy(json_field(caps_raw, k))));
    }

    std::string ui_brand = json_field(lic, "ui_brand").as_string("ttbox");
    if (ui_brand.empty()) ui_brand = "ttbox";

    JsonValue out = JsonValue::object();
    out.set("activated", JsonValue::boolean(activated));
    out.set("state", JsonValue::string(state));
    out.set("plan", JsonValue::string(json_field(lic, "plan").as_string("none")));
    out.set("is_pro", JsonValue::boolean(json_field(lic, "is_pro").as_bool(false)));
    out.set("features", std::move(features));
    out.set("expires_at", JsonValue::string(unix_to_iso(json_field(lic, "expires_at").as_int(0))));
    out.set("grace_until", JsonValue::string(unix_to_iso(json_field(lic, "grace_until").as_int(0))));
    out.set("message", JsonValue::string(json_field(lic, "last_error").as_string("")));
    out.set("heartbeat_interval_s", JsonValue::number(static_cast<double>(heartbeat)));
    out.set("short_code", JsonValue::string(json_field(lic, "short_code").as_string("")));
    out.set("capabilities", std::move(caps));
    out.set("ui_brand", JsonValue::string(ui_brand));
    out.set("status", JsonValue::string(state));
    out.set("valid", JsonValue::boolean(activated));
    return out;
}

// ---- GET_STATUS.metrics 字段快捷读取（collect_web_state 用，压缩投影冗长）----
// metrics 数值 / 整数 / 真值 / 字符串快捷读取（缺省 0/0/false/""）。
inline double mnum(const JsonValue& m, const char* k) { return json_field(m, k).as_number(0.0); }
inline int64_t mint(const JsonValue& m, const char* k) { return json_field(m, k).as_int(0); }
inline bool mbool(const JsonValue& m, const char* k) { return json_truthy(json_field(m, k)); }
inline std::string mstr(const JsonValue& m, const char* k) {
    return json_field(m, k).as_string("");
}

// RuntimeProfile 归一：把 capture.width/height 拉回 Core 合法域（0 或 64~3840）。
// 对齐 capture_geometry.py::normalize_profile_capture_size（合并后最后一道闸）。
inline void normalize_profile_capture_size(JsonValue* prof) {
    if (prof == nullptr || !prof->is_object()) return;
    const JsonValue* cap_ptr = prof->find("capture");
    if (cap_ptr == nullptr || !cap_ptr->is_object()) return;
    JsonValue cap = *cap_ptr;
    const JsonValue* w = cap.find("width");
    if (w != nullptr) {
        cap.set("width", JsonValue::number(static_cast<double>(normalize_capture_crop_size(*w))));
    }
    const JsonValue* h = cap.find("height");
    if (h != nullptr) {
        cap.set("height", JsonValue::number(static_cast<double>(normalize_capture_crop_size(*h))));
    }
    prof->set("capture", std::move(cap));
}

// 经 IPC 读 GET_CONFIG.runtime_profile；失败返回 null（注入 runtime_profile_getter 用）。
inline JsonValue ipc_runtime_profile_or_null(IpcClient& ipc) {
    JsonValue out;
    std::string err;
    return get_runtime_profile(ipc, &out, &err) ? out : JsonValue::null();
}

// 剥离空 model_id（模型选中唯一真源 = ModelRegistry active，保存/加载预设不覆盖）。
inline JsonValue strip_empty_model_id(const JsonValue& profile) {
    JsonValue clean = JsonValue::object();
    if (!profile.is_object()) return clean;
    for (const auto& [k, v] : profile.as_object()) {
        if (k == "model_id" && v.as_string("").empty()) continue;
        clean.set(k, v);
    }
    return clean;
}

}  // namespace ttbox::core::web
