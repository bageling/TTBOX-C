#pragma once

#include <cstdint>
#include <string>

namespace ttbox::core::auth {

// RK3588 硬件指纹：/proc/cpuinfo cpu_serial + /proc/device-tree/serial-number
// 保持与原 aibox-bl 兼容：仅以 cpu_serial 作为绑定主串。
struct DeviceFingerprint {
    std::string cpu_serial;     // /proc/cpuinfo "Serial   : xxxxxxxxxxxxxx"
    std::string dt_serial;      // /proc/device-tree/serial-number (可选)
    std::string mac_first;      // 首个非 loopback netdev MAC (可选)

    // 最终绑定串：与原系统 bind_device(cpu_serial) 一致
    std::string bind_string() const;

    // 读取系统信息（Linux/RK3588），失败字段留空
    static DeviceFingerprint detect();
};

// 读取 /proc/cpuinfo，提取 "Serial : " 之后的 16 进制串
// Windows/测试环境：回退到 "LOCALHOST-SERIAL" 方便开发调试
std::string read_cpuinfo_serial();

}  // namespace ttbox::core::auth
