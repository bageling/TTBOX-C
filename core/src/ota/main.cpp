// main.cpp — ttbox_ota 入口（V1.0.56：替代 scripts/ttbox_ota_updater.py）。
//
// 契约与旧 Python 入口**保持一致**（systemd 单元与 web 调用点无需改参数）：
//   用法： ttbox_ota <url> [key_id] [--version V]
//         ttbox_ota --from-jobs [JOBS_DIR]
//   环境： TTBOX_PREFIX（前缀，默认 /opt/ttbox）
//   退出： 0 成功 / 1 失败 / 2 用法错误
#include <cstdio>
#include <cstdlib>
#include <string>

#include "ota/OtaUpdater.hpp"

namespace {

// 读环境变量，未设或空串时返回默认值。
std::string env_or(const char* key, const std::string& def) {
    const char* v = std::getenv(key);
    return (v != nullptr && *v != '\0') ? std::string(v) : def;
}

}  // namespace

// ttbox_ota 入口：解析 url/key_id/--version/--from-jobs 并派发到 process_jobs 或 run_update。
int main(int argc, char** argv) {
    ttbox::core::ota::OtaEnv env;
    env.prefix = env_or("TTBOX_PREFIX", "/opt/ttbox");
    env.default_key_id = env_or("TTBOX_OTA_KEY_ID", "ttbox-ota-2026b");

    std::string url, key_id, version, from_jobs;
    bool has_from_jobs = false;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--from-jobs") {
            has_from_jobs = true;
            if (i + 1 < argc && argv[i + 1][0] != '-') {
                from_jobs = argv[++i];
            } else {
                from_jobs = "/var/lib/ttbox/ota/jobs";
            }
        } else if (a == "--version" && i + 1 < argc) {
            version = argv[++i];
        } else if (url.empty()) {
            url = a;
        } else if (key_id.empty()) {
            key_id = a;
        }
    }

    if (has_from_jobs) return ttbox::core::ota::process_jobs(env, from_jobs);
    if (url.empty()) {
        std::fprintf(stderr, "需要 url 或 --from-jobs\n");
        return 2;
    }
    if (key_id.empty()) key_id = env.default_key_id;
    return ttbox::core::ota::run_update(env, url, key_id, version);
}
