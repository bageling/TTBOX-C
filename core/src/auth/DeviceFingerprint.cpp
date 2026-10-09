#include "auth/DeviceFingerprint.hpp"

#include <array>
#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>

#if defined(__linux__)
#include <cstring>
#include <dirent.h>
#include <net/if.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

namespace ttbox::core::auth {

namespace {

// 去掉字符串首尾空白（isspace），返回副本；输入不含内部空白处理。
std::string strip_ws(const std::string& s) {
    size_t a = 0, b = s.size();
    while (a < b && std::isspace(static_cast<unsigned char>(s[a]))) ++a;
    while (b > a && std::isspace(static_cast<unsigned char>(s[b - 1]))) --b;
    return s.substr(a, b - a);
}

}  // namespace

// 读取 /proc/cpuinfo 的 "Serial" 行值；非 Linux 平台返回固定开发串。
std::string read_cpuinfo_serial() {
#if defined(__linux__)
    std::ifstream f("/proc/cpuinfo");
    if (!f) return {};
    std::string line;
    while (std::getline(f, line)) {
        auto pos = line.find(':');
        if (pos == std::string::npos) continue;
        std::string key = strip_ws(line.substr(0, pos));
        if (key == "Serial") {
            return strip_ws(line.substr(pos + 1));
        }
    }
    return {};
#else
    return "LOCALHOST-SERIAL";
#endif
}

// 采集本机指纹：cpu serial + 设备树 serial + 首个非 lo 网卡 MAC（各项失败留空）。
DeviceFingerprint DeviceFingerprint::detect() {
    DeviceFingerprint fp;
    fp.cpu_serial = read_cpuinfo_serial();

#if defined(__linux__)
    // DT serial: /proc/device-tree/serial-number
    {
        std::ifstream f("/proc/device-tree/serial-number", std::ios::binary);
        if (f) {
            std::ostringstream ss;
            ss << f.rdbuf();
            std::string raw = ss.str();
            // 以 NUL 结尾的 ASCII；截断首个 \0
            auto z = raw.find('\0');
            if (z != std::string::npos) raw.resize(z);
            fp.dt_serial = strip_ws(raw);
        }
    }

    // 首个网卡 MAC（遍历 /sys/class/net 排除 lo）
    {
        DIR* d = opendir("/sys/class/net");
        if (d) {
            struct dirent* ent;
            while ((ent = readdir(d)) != nullptr) {
                std::string name = ent->d_name;
                if (name == "." || name == ".." || name == "lo") continue;
                std::string path = std::string("/sys/class/net/") + name + "/address";
                std::ifstream f(path);
                if (f) {
                    std::string line;
                    std::getline(f, line);
                    fp.mac_first = strip_ws(line);
                    break;
                }
            }
            closedir(d);
        }
    }
#endif
    return fp;
}

// 返回设备绑定权威串：仅 cpu_serial；为空时回落 "unknown-serial"。
std::string DeviceFingerprint::bind_string() const {
    // 与原 aibox-bl `bind_device(cpu_serial)` 完全兼容：只用 cpu_serial
    return cpu_serial.empty() ? std::string("unknown-serial") : cpu_serial;
}

}  // namespace ttbox::core::auth
