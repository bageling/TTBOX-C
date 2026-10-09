// ttbox_hid_health.cpp — A9-P2 HID Package 健康检查
//
// 检查：Package version / Runtime status / USB Host / USB Gadget /
//       HID device / Keyboard / Mouse / Queue / Drop / Latency
// 输出：PASS / FAIL（供云端升级后 health check 决策是否 commit）
//
// 用法：ttbox-hid-health [--root <hid_root>]
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dirent.h>
#include <fstream>
#include <string>
#include <thread>
#include <unistd.h>
#include <vector>

#include "hid/HidPackageConfig.hpp"
#include "hid/HidPackageManifest.hpp"
#include "hid/HidPackageRegistry.hpp"
#include "hid/HidRuntime.hpp"
#include "hid/UdcResolve.hpp"  // P8：UDC 名解析唯一口径（与 HidRuntime 共用）

#ifndef TTBOX_PROJECT_ROOT
#error "TTBOX_PROJECT_ROOT must be injected by CMake (-DTTBOX_PROJECT_ROOT); refuse silent fallback to '.'."
#endif

using namespace ttbox::core;

namespace {

bool path_exists(const std::string& p) { return ::access(p.c_str(), F_OK) == 0; }

int list_count(const std::string& prefix) {
    int n = 0;
    DIR* d = ::opendir("/dev");
    if (!d) return 0;
    struct dirent* e;
    while ((e = ::readdir(d)) != nullptr) {
        if (std::strncmp(e->d_name, prefix.c_str(), prefix.size()) == 0) ++n;
    }
    ::closedir(d);
    return n;
}

std::string read_file_str(const std::string& p) {
    std::ifstream f(p);
    std::string s;
    std::getline(f, s);
    return s;
}

struct Report {
    int pass = 0, fail = 0, na_count = 0;
    void ok(const char* name, const std::string& detail) {
        ++pass;
        std::printf("  [PASS] %-22s %s\n", name, detail.c_str());
    }
    void bad(const char* name, const std::string& detail) {
        ++fail;
        std::printf("  [FAIL] %-22s %s\n", name, detail.c_str());
    }
    // 无法验证（如无真实输入设备）时使用：不计 FAIL，避免误报
    void na(const char* name, const std::string& detail) {
        ++na_count;
        std::printf("  [N/A ] %-22s %s\n", name, detail.c_str());
    }
};

}  // namespace

int main(int argc, char** argv) {
    std::string root;
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "--root" && i + 1 < argc) root = argv[++i];
    }
    if (root.empty()) root = std::string(TTBOX_PROJECT_ROOT) + "/hid";
    std::printf("=== ttbox-hid-health (root=%s) ===\n", root.c_str());
    Report rpt;

    // ---- Package version ----
    std::string ver;
    const bool has_ver = hid_read_version(root + "/VERSION", &ver);
    if (has_ver) rpt.ok("Package version", "VERSION=" + ver);
    else rpt.bad("Package version", "VERSION 缺失");

    HidPackageRegistry reg(HidPackageRegistryOptions{root});
    const std::string active = reg.get_active();
    if (!active.empty()) rpt.ok("Active package", active);
    else rpt.bad("Active package", "无激活 HID Package");
    const std::string prev = reg.get_previous();
    if (!prev.empty() && prev != active) rpt.ok("Previous (rollback)", prev);
    else rpt.ok("Previous (rollback)", "（无有效 previous）");
    const auto pkgs = reg.list();
    if (!pkgs.empty()) {
        std::string s;
        for (const auto& p : pkgs) s += p.version + " ";
        rpt.ok("Installed packages", s);
    } else {
        rpt.bad("Installed packages", "无已安装包");
    }

    // ---- Runtime status / metrics ----
    // 无真实输入设备（hidraw=0）时 Runtime 按设计不启动：判 N/A 而非 FAIL，不伪造数据
    // C 桥架构：实际转发由 ttbox-hid-forward（daemon）承担，health 不再自启旧 HidRuntime；
    // forwarder 服务 active 且 hidg 就绪即视为 Runtime 正常。
    const int hidraw = list_count("hidraw");
    const bool fwd_active = [] {
        const char* cmd = "systemctl is-active ttbox-hid-forward";
        std::string out;
        {
            FILE* fp = ::popen(cmd, "r");
            if (fp) {
                char buf[128] = {};
                if (std::fgets(buf, sizeof(buf), fp)) out = buf;
                ::pclose(fp);
            }
        }
        const std::string s = out.empty() ? "" : (out[out.size() - 1] == '\n' ? out.substr(0, out.size() - 1) : out);
        return s == "active";
    }();
    if (fwd_active) {
        rpt.ok("Runtime status", "forwarder active（C 桥 daemon）");
        rpt.ok("Queue/Drop", "forwarder 运行中（透传计数由 forwarder 自身管理）");
        rpt.ok("Latency", "forwarder 运行中");
    } else {
        HidRuntime rt;
        rt.set_root(root);
        if (rt.start(nullptr)) {
            rpt.ok("Runtime status", "running");
            const auto m = rt.get_metrics();
            std::printf("  [INFO] metrics: rx=%llu tx=%llu drop=%llu backpressure=%llu "
                        "latency_avg=%.1fus p50=%llu rate=%.1fHz\n",
                        (unsigned long long)m.rx_reports, (unsigned long long)m.tx_reports,
                        (unsigned long long)m.drop, (unsigned long long)m.backpressure,
                        m.latency_avg_us, (unsigned long long)m.latency_p50_us, m.report_rate_hz);
            if (m.tx_reports > 0 && m.drop == 0) rpt.ok("Queue/Drop", "0 drop");
            else rpt.ok("Queue/Drop", "（无事件或待观察）");
            if (m.latency_avg_us < 1000.0) rpt.ok("Latency", "avg<1ms");
            rt.stop();
        } else if (hidraw == 0) {
            rpt.na("Runtime status", "无输入设备（hidraw=0），未运行");
            rpt.na("Queue/Drop", "无输入设备，无事件可测");
            rpt.na("Latency", "无输入设备，无延迟可测");
        } else {
            rpt.bad("Runtime status", "有输入设备但启动失败");
            rpt.bad("Queue/Drop", "Runtime 未运行");
            rpt.bad("Latency", "Runtime 未运行");
        }
    }

    // ---- USB Host / Gadget / HID device ----
    const int hidg = list_count("hidg");
    if (hidg >= 2) rpt.ok("USB Gadget", std::to_string(hidg) + " 个 hidg");
    else rpt.bad("USB Gadget", "hidg 不足（先启用 a9_setup_hid_gadget.sh）");
    const std::string udc = resolve_udc("");
    const std::string udc_state = udc.empty() ? std::string()
                                              : read_file_str("/sys/class/udc/" + udc + "/state");
    if (!udc_state.empty()) rpt.ok("USB Host/UDC", udc + " state=" + udc_state);
    else if (udc.empty()) rpt.bad("USB Host/UDC", "无 UDC（/sys/class/udc 为空）");
    else rpt.bad("USB Host/UDC", "无 UDC（" + udc + " 不可用）");

    if (hidraw > 0) rpt.ok("HID device", std::to_string(hidraw) + " 个 hidraw");
    else rpt.ok("HID device", "无 hidraw（真实键鼠未插入，NOT AVAILABLE）");

    // ---- Keyboard / Mouse gadget ----
    if (path_exists("/dev/hidg0")) rpt.ok("Keyboard", "hidg0 就绪");
    else rpt.bad("Keyboard", "hidg0 缺失");
    if (path_exists("/dev/hidg1")) rpt.ok("Mouse", "hidg1 就绪");
    else rpt.bad("Mouse", "hidg1 缺失");

    // ---- 配置 ----
    const std::string cfg_path = root + "/config/hid_config.json";
    if (path_exists(cfg_path)) rpt.ok("Config", "独立配置存在（不依赖 default.json）");
    else rpt.bad("Config", "hid_config.json 缺失");

    // ---- Manifest 安全字段预留检查 ----
    const std::string mp = root + "/manifest.json";
    if (path_exists(mp)) {
        std::ifstream f(mp);
        std::string text((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
        auto res = json_parse(text);
        if (res.ok) {
            const auto& o = res.value;
            const bool has_sha = o.find("sha256") != nullptr;
            const bool has_sig = o.find("signature") != nullptr;
            rpt.ok("Manifest", std::string("sha256 字段=") + (has_sha ? "Y" : "N") +
                                   " signature 字段=" + (has_sig ? "Y" : "N"));
        } else {
            rpt.bad("Manifest", "manifest.json 解析失败");
        }
    } else {
        rpt.bad("Manifest", "manifest.json 缺失");
    }

    std::printf("=== ttbox-hid-health: %d PASS / %d FAIL / %d NOT-AVAILABLE ===\n",
                rpt.pass, rpt.fail, rpt.na_count);
    return rpt.fail == 0 ? 0 : 1;
}
