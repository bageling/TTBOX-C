// main.cpp — ttbox_edid 入口（V1.0.53：替代 scripts/edid/edid_apply.sh）。
//
// 契约与旧 shell 入口**保持一致**（调用方无需改参数）：
//   用法：  ttbox_edid [device]          缺省 /dev/video0
//   环境：  TTBOX_PREFIX                       默认 /opt/ttbox
//           TTBOX_DISPLAY_CONFIG               默认 <prefix>/config/hardware_display.json
//           EDID_OUTPUT                        默认 <prefix>/runtime/edid/current.bin
//           TTBOX_EDID_REHANDSHAKE             默认 1（设 0 ⇒ 纯注入不切 HPD）
//           TTBOX_EDID_REHANDSHAKE_ATTEMPTS    默认 2
//           TTBOX_EDID_LOCK_TIMEOUT_SEC        默认 10
//           TTBOX_EDID_HPD_SETTLE_SEC          默认 0.5
//   输出：  stdout 一行 JSON（与旧脚本同结构，供 core / 面板原样转发）
//   退出码：0 成功；1 注入或锁定失败；2 设备非法（旧脚本对非法设备即 exit 2）
#include <cstdio>
#include <cstdlib>
#include <string>

#include "edid/EdidApply.hpp"

namespace {

// 读环境变量，未设或空串时返回默认值。
std::string env_or(const char* key, const std::string& def) {
    const char* v = std::getenv(key);
    return (v != nullptr && *v != '\0') ? std::string(v) : def;
}

// 读环境变量转 int；未设、非法或非正数时返回默认值。
int env_int(const char* key, int def) {
    const char* v = std::getenv(key);
    if (v == nullptr || *v == '\0') return def;
    const int n = std::atoi(v);
    return n > 0 ? n : def;
}

// 读环境变量转 double；未设、非法或非正数时返回默认值。
double env_double(const char* key, double def) {
    const char* v = std::getenv(key);
    if (v == nullptr || *v == '\0') return def;
    const double d = std::atof(v);
    return d > 0.0 ? d : def;
}

}  // namespace

// ttbox_edid 入口：解析设备/环境参数 → 调 apply → 打印 JSON 并以 exit_code 退出。
int main(int argc, char** argv) {
    const std::string dev = (argc > 1) ? std::string(argv[1]) : std::string("/dev/video0");

    // 设备白名单：与旧脚本同一句硬拦（exit 2），且**不碰任何硬件/配置**
    if (dev != "/dev/video0") {
        std::printf(
            "{\"ok\": false, \"error\": \"错误的 HDMI-RX 设备 %s：EDID 注入必须使用 "
            "/dev/video0；/dev/dri/card0 仅用于 loopout\"}\n",
            dev.c_str());
        std::fprintf(stderr,
                     "错误的 HDMI-RX 设备 %s：EDID 注入必须使用 /dev/video0；"
                     "/dev/dri/card0 仅用于 loopout\n",
                     dev.c_str());
        return 2;
    }

    ttbox::core::edid::ApplyOptions opt;
    opt.prefix = env_or("TTBOX_PREFIX", "/opt/ttbox");
    opt.config_path = env_or("TTBOX_DISPLAY_CONFIG", "");
    opt.output_path = env_or("EDID_OUTPUT", "");
    opt.video_dev = dev;
    opt.rehandshake = env_or("TTBOX_EDID_REHANDSHAKE", "1") != "0";
    opt.attempts = env_int("TTBOX_EDID_REHANDSHAKE_ATTEMPTS", 2);
    opt.lock_timeout_sec = env_int("TTBOX_EDID_LOCK_TIMEOUT_SEC", 10);
    opt.hpd_settle_sec = env_double("TTBOX_EDID_HPD_SETTLE_SEC", 0.5);

    const ttbox::core::edid::ApplyResult r = ttbox::core::edid::apply(opt);
    if (!r.json.empty()) std::printf("%s\n", r.json.c_str());
    if (!r.error.empty()) std::fprintf(stderr, "edid_apply: %s\n", r.error.c_str());
    return r.exit_code;
}
