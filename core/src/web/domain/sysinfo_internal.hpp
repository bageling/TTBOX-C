// sysinfo_internal.hpp — system 域共享：只读命令执行 + sysfs 读数 + 板载资源采集。
//
// 自 plugins/web/lib/sysinfo.py 与 bin/ttbox-web.py::_run_quiet/_sysfs_int 逐行为移植。
// 全部 inline：system.cpp / rootfs_internal.hpp / power_action_allowed 共用。
#pragma once

#include <sys/statvfs.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cctype>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <initializer_list>
#include <sstream>
#include <string>
#include <vector>

#include "common/Json.hpp"

namespace ttbox::core::web {

// 面板监听端口（与 plugins/web/lib/settings.py::LISTEN_PORT / paths.WEB_PORT_DEFAULT 同值）。
inline constexpr int kWebPort = 8000;

// 只读探测命令：非 0 退出 / 无法执行一律返回空串（对齐 _run_quiet，不抛）。
// argv 均为字面量（findmnt/lsblk/busctl/which/hostname），无 shell 注入面。
inline std::string run_quiet(std::initializer_list<std::string> argv) {
    std::string cmd;
    for (const std::string& a : argv) {
        if (!cmd.empty()) cmd += " ";
        cmd += a;
    }
    cmd += " 2>/dev/null";
    FILE* p = ::popen(cmd.c_str(), "r");
    if (p == nullptr) return "";
    std::string out;
    char buf[256];
    while (std::fgets(buf, sizeof(buf), p) != nullptr) out += buf;
    const int st = ::pclose(p);
    if (st == -1) return "";
    if (WIFEXITED(st) && WEXITSTATUS(st) != 0) return "";
    while (!out.empty() && std::isspace(static_cast<unsigned char>(out.back())) != 0) {
        out.pop_back();
    }
    return out;
}

// 读 sysfs 整数；读不到返回 -1（区分"值为 0"和"读不到"，对齐 _sysfs_int）。
inline int64_t sysfs_int(const std::string& path) {
    std::ifstream in(path);
    if (!in.is_open()) return -1;
    int64_t v = 0;
    in >> v;
    if (in.fail()) return -1;
    return v;
}

// 墙钟秒（time.time 等价；rootfs 5s 缓存用）。
inline double now_seconds() {
    return std::chrono::duration<double>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

// /proc 浮点/整数读取兜底（对齐 sysinfo._read_float/_read_int）。
inline double read_float(const std::string& path, double def = 0.0) {
    std::ifstream in(path);
    if (!in.is_open()) return def;
    double v = def;
    in >> v;
    return in.fail() ? def : v;
}
inline int64_t read_int(const std::string& path, int64_t def = 0) {
    std::ifstream in(path);
    if (!in.is_open()) return def;
    int64_t v = def;
    in >> v;
    return in.fail() ? def : v;
}

// CPU 占用%（两次 /proc/stat 采样差分；首次返回 0，对齐 sysinfo._cpu_percent）。
inline double cpu_percent() {
    static int64_t prev_idle = -1;
    static int64_t prev_total = 0;
    std::ifstream in("/proc/stat");
    std::string line;
    if (!std::getline(in, line)) return 0.0;
    std::istringstream ss(line);
    std::string cpu;
    ss >> cpu;
    std::vector<int64_t> vals;
    int64_t v;
    while (ss >> v) vals.push_back(v);
    if (vals.size() < 4) return 0.0;
    const int64_t idle = vals[3] + (vals.size() > 4 ? vals[4] : 0);
    int64_t total = 0;
    for (int64_t x : vals) total += x;
    double result = 0.0;
    if (prev_idle >= 0) {
        const int64_t didle = idle - prev_idle;
        const int64_t dtotal = total - prev_total;
        if (dtotal > 0) {
            const double p = 100.0 * (1.0 - static_cast<double>(didle) / dtotal);
            result = p < 0.0 ? 0.0 : (p > 100.0 ? 100.0 : p);
        }
    }
    prev_idle = idle;
    prev_total = total;
    return result;
}

// 内存（/proc/meminfo；对齐 sysinfo._memory）。
inline JsonValue memory_payload() {
    int64_t total = 0, free_kb = 0, avail = 0;
    std::ifstream in("/proc/meminfo");
    std::string line;
    while (std::getline(in, line)) {
        std::istringstream ss(line);
        std::string key, value;
        ss >> key >> value;
        if (key == "MemTotal:") total = std::stoll(value) * 1024;
        else if (key == "MemFree:") free_kb = std::stoll(value) * 1024;
        else if (key == "MemAvailable:") avail = std::stoll(value) * 1024;
    }
    if (avail == 0) avail = free_kb;
    const int64_t used = total - avail;
    JsonValue out = JsonValue::object();
    out.set("total", JsonValue::number(static_cast<double>(total)));
    out.set("used", JsonValue::number(static_cast<double>(used > 0 ? used : 0)));
    out.set("free", JsonValue::number(static_cast<double>(avail)));
    out.set("percent", JsonValue::number(total ? 100.0 * used / total : 0.0));
    return out;
}

// SoC 温度（优先 soc-thermal，回退第一个可用 zone；对齐 sysinfo._temperature）。
inline JsonValue temperature_payload() {
    std::string best_label;
    double best_temp = 0.0;
    std::error_code ec;
    for (const auto& e : std::filesystem::directory_iterator(
             "/sys/class/thermal", std::filesystem::directory_options::skip_permission_denied, ec)) {
        const std::string name = e.path().filename().string();
        if (name.rfind("thermal_zone", 0) != 0) continue;
        const std::string base = e.path().string();
        std::ifstream t(base + "/type");
        std::string ztype;
        std::getline(t, ztype);
        const double temp = read_float(base + "/temp", 0.0) / 1000.0;
        if (temp <= 0) continue;
        if (ztype == "soc-thermal") {
            JsonValue out = JsonValue::object();
            out.set("celsius", JsonValue::number(temp));
            out.set("label", JsonValue::string("SoC"));
            out.set("zone", JsonValue::string(ztype));
            return out;
        }
        if (best_label.empty()) {
            best_label = ztype;
            best_temp = temp;
        }
    }
    JsonValue out = JsonValue::object();
    if (!best_label.empty()) {
        out.set("celsius", JsonValue::number(best_temp));
        out.set("label", JsonValue::string(best_label));
        out.set("zone", JsonValue::string(best_label));
    } else {
        out.set("celsius", JsonValue::number(0.0));
        out.set("label", JsonValue::string("thermal"));
        out.set("zone", JsonValue::string(""));
    }
    return out;
}

// 根分区容量（statvfs；对齐 sysinfo._storage）。
inline JsonValue storage_payload() {
    struct statvfs st;
    JsonValue out = JsonValue::object();
    if (::statvfs("/", &st) != 0) {
        out.set("total", JsonValue::number(0.0));
        out.set("used", JsonValue::number(0.0));
        out.set("free", JsonValue::number(0.0));
        out.set("percent", JsonValue::number(0.0));
        return out;
    }
    const int64_t total = static_cast<int64_t>(st.f_blocks) * st.f_frsize;
    const int64_t bfree = static_cast<int64_t>(st.f_bfree) * st.f_frsize;
    const int64_t bavail = static_cast<int64_t>(st.f_bavail) * st.f_frsize;
    const int64_t used = total - bfree;
    out.set("total", JsonValue::number(static_cast<double>(total)));
    out.set("used", JsonValue::number(static_cast<double>(used)));
    out.set("free", JsonValue::number(static_cast<double>(bavail)));
    out.set("percent", JsonValue::number(total ? 100.0 * used / total : 0.0));
    return out;
}

// 负载（/proc/loadavg 前三项；对齐 sysinfo._load_average）。
inline JsonValue load_average_payload() {
    JsonValue arr = JsonValue::array();
    std::ifstream in("/proc/loadavg");
    double v;
    int n = 0;
    while (n < 3 && in >> v) {
        arr.push_back(JsonValue::number(v));
        ++n;
    }
    return arr;
}

inline std::string hostname_payload() {
    const std::string h = run_quiet({"hostname"});
    return h.empty() ? "ttbox" : h;
}

// LAN IPv4（hostname -I 首个非 127 地址；对齐 sysinfo._lan_ipv4）。
inline std::string lan_ipv4_payload() {
    const std::string out = run_quiet({"hostname", "-I"});
    std::istringstream ss(out);
    std::string ip;
    while (ss >> ip) {
        if (ip.rfind("127.", 0) != 0) return ip;
    }
    return "";
}

inline double uptime_seconds_payload() {
    std::ifstream in("/proc/uptime");
    double v = 0.0;
    in >> v;
    return in.fail() ? 0.0 : v;
}

// /api/system 全量系统状态（对齐 sysinfo.collect_system_stats）。
inline JsonValue collect_system_stats() {
    JsonValue out = JsonValue::object();
    out.set("hostname", JsonValue::string(hostname_payload()));
    out.set("uptime_seconds", JsonValue::number(uptime_seconds_payload()));
    out.set("cpu_percent", JsonValue::number(cpu_percent()));
    out.set("load_average", load_average_payload());
    out.set("memory", memory_payload());
    out.set("temperature", temperature_payload());
    out.set("storage", storage_payload());
    out.set("lan_ipv4", JsonValue::string(lan_ipv4_payload()));
    out.set("lan_url", JsonValue::string(""));
    out.set("mdns_url", JsonValue::string(""));
    out.set("web_port", JsonValue::number(static_cast<double>(kWebPort)));
    out.set("os", JsonValue::string("Orange Pi 1.2.0"));
    out.set("version", JsonValue::string(""));
    out.set("app_version", JsonValue::string("ttbox-0.1.0"));
    return out;
}

// hostname PUT / web-port PUT 返回的网络摘要（对齐 sysinfo.collect_network_summary）。
inline JsonValue collect_network_summary() {
    JsonValue out = JsonValue::object();
    out.set("hostname", JsonValue::string(hostname_payload()));
    out.set("lan_ipv4", JsonValue::string(lan_ipv4_payload()));
    out.set("lan_url", JsonValue::string(""));
    out.set("mdns_url", JsonValue::string(""));
    out.set("web_port", JsonValue::number(static_cast<double>(kWebPort)));
    return out;
}

}  // namespace ttbox::core::web
