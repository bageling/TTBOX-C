// ota_impl.cpp — OTA 安装实现 + 当前版本（status/check 见 ota_query.cpp）。
//
// 自 plugins/web/api/ota.py 与 lib/ota.py 逐行为移植。URL 一字未改。
#include "web/domain/ota_internal.hpp"

#include <cctype>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <string>

#include "common/Json.hpp"
#include "common/Paths.hpp"
#include "web/domain/domain_internal.hpp"
#include "web/domain/domain_routes.hpp"

namespace ttbox::core::web {

namespace {

// OTA 任务目录（更新器轮询该目录取待办任务）。
std::string ota_jobs_dir() { return "/var/lib/ttbox/ota/jobs"; }

// OTA 更新器二进制路径（<prefix>/current/bin/ttbox_ota）。
std::string ota_updater_path() {
    // ★ V1.0.56：由 python3 脚本换成 C++ 二进制。契约不变（argv[1]=url、可选 [key_id]）。
    return join_path(join_path(ttbox_prefix(), "current"), "bin/ttbox_ota");
}

// key_id / version 白名单校验（对齐 ota._OTA_ID_RE，源头拒路径穿越）。
bool ota_id_valid(const std::string& s) {
    if (s.empty() || s.find("..") != std::string::npos) return false;
    const unsigned char c0 = static_cast<unsigned char>(s[0]);
    if (!std::isalnum(c0)) return false;
    for (unsigned char c : s) {
        if (!(std::isalnum(c) || c == '.' || c == '_' || c == '-')) return false;
    }
    return true;
}

}  // namespace

// 当前部署版本：优先 current 软链目录名，回退 GET_STATUS.version。
std::string ota_current_version(IpcClient& ipc) {
    // current 软链目录名优先。
    const std::string current = join_path(ttbox_prefix(), "current");
    std::error_code ec;
    const std::string real = std::filesystem::canonical(current, ec).string();
    if (!ec) {
        const std::string base = std::filesystem::path(real).filename().string();
        if (!base.empty() && base != "current") return base;
    }
    return json_field(get_status_data(ipc), "version").as_string("");
}

// 安装实现：校验授权/参数/目录后，把任务写成 jobs/job-<ts>.json 交 root 更新器。
void ota_install_impl(IpcClient& ipc, const httplib::Request& req, httplib::Response& res) {
    const JsonValue lic = license_block(ipc);
    const bool ota_cap = json_truthy(json_field(json_field(lic, "capabilities"), "ota"));
    if (!ota_cap) {
        send_json(res, 403, false, JsonValue::object(), "feature 'ota' not licensed", "");
        return;
    }
    const JsonValue body = parse_json_body(req);
    const std::string url = json_field(body, "url").as_string("");
    const std::string key_id =
        !json_field(body, "key_id").as_string("").empty()
            ? json_field(body, "key_id").as_string("")
            : ota_default_key_id();
    if (!ota_id_valid(key_id)) {
        send_json(res, 400, false, JsonValue::object(), "非法 key_id", "");
        return;
    }
    if (url.empty()) {
        send_json(res, 400, false, JsonValue::object(), "url is required", "");
        return;
    }
    if (url.compare(0, 8, "https://") != 0) {
        send_json(res, 400, false, JsonValue::object(), "仅接受 https 更新源", "");
        return;
    }
    const std::string updater = ota_updater_path();
    const std::string jobs = ota_jobs_dir();
    if (!std::filesystem::is_regular_file(updater)) {
        send_json(res, 500, false, JsonValue::object(), "更新器缺失: " + updater, "");
        return;
    }
    if (!std::filesystem::is_directory(jobs)) {
        send_json(res, 500, false, JsonValue::object(), "任务目录缺失: " + jobs, "");
        return;
    }
    // 任务目录可写探测（D03：环境故障当场报错，不假成功）。
    const std::string probe = join_path(jobs, ".probe-" + std::to_string(std::time(nullptr)));
    {
        std::ofstream f(probe);
        if (!f.is_open()) {
            send_json(res, 500, false, JsonValue::object(), "任务目录不可写: " + jobs, "");
            return;
        }
        f.close();
        std::error_code ec;
        std::filesystem::remove(probe, ec);
    }
    JsonValue job = JsonValue::object();
    job.set("url", JsonValue::string(url));
    job.set("key_id", JsonValue::string(key_id));
    job.set("enqueued_at", JsonValue::number(static_cast<double>(std::time(nullptr))));
    const std::string version = json_field(body, "version").as_string("");
    if (!version.empty()) {
        if (!ota_id_valid(version)) {
            send_json(res, 400, false, JsonValue::object(), "非法 version", "");
            return;
        }
        job.set("version", JsonValue::string(version));
    }
    const std::string name = "job-" + std::to_string(std::time(nullptr) * 1000LL) + ".json";
    if (!write_file(join_path(jobs, name), job.dump())) {
        send_json(res, 500, false, JsonValue::object(), "写任务文件失败: " + jobs + "/" + name, "");
        return;
    }
    JsonValue data = JsonValue::object();
    data.set("scheduled", JsonValue::string(name));
    data.set("message", JsonValue::string("更新任务已受理"));
    send_json(res, 200, true, data, "", "");
}

}  // namespace ttbox::core::web
