// HidPackageConfig.hpp — A9-P2 HID Package 独立配置
//
// HID 配置独立于 AI Runtime（不写入 config/default.json）。
// 存储于 <hid_root>/config/hid_config.json；以后 HID 参数修改不影响 AI Runtime 配置。
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "common/Json.hpp"

namespace ttbox::core {

// HID Package 独立配置（字段预留，开发版默认值）
struct HidPackageConfig {
    // 设备
    std::string keyboard_hidraw;   // 空 = 自动枚举
    std::string mouse_hidraw;
    int report_rate_hz = 500;      // 目标回报率（f_hid 上限 ~500Hz，实测）
    // 鼠标
    bool mouse_enabled = true;
    bool mouse_buttons_enabled = true;
    bool mouse_wheel_enabled = true;
    // 键盘
    bool keyboard_enabled = true;
    // 线程
    int cpu_affinity = -1;         // -1 = 默认调度
    int queue_size = 1024;         // SPSC 队列容量
    // Gadget
    std::string gadget_name = "ttbox-hid";
    // UDC（USB Device Controller）名：**空 = 运行时解析**，HidRuntime::start 按
    // 「USB_PROXY_DEVICE 环境变量 → /sys/class/udc 枚举首个控制器」取值。
    // 不写死 SoC 上的控制器编号（如历史上的 "fc000000.usb"）——那是板级拓扑，
    // 不是软件常量（P8）。
    std::string udc;
    std::string keyboard_hidg = "/dev/hidg0";
    std::string mouse_hidg = "/dev/hidg1";
    // 描述符（descriptors/ 目录下文件，空 = 内置默认）
    std::string keyboard_descriptor = "keyboard.desc";
    std::string mouse_descriptor = "mouse.desc";

    JsonValue to_json() const;
    static HidPackageConfig from_json(const JsonValue& v);
    static HidPackageConfig load(const std::string& path, std::string* error = nullptr);
    bool save(const std::string& path, std::string* error = nullptr) const;
};

}  // namespace ttbox::core
