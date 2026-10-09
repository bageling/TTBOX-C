// UdcResolve.hpp — UDC（USB Device Controller）名解析（P8）
//
// 为什么单独一个头：
//   UDC 名是**板级拓扑**（内核按枚举顺序分配），不是软件常量。历史上 HidPackageConfig
//   的默认值写死 "fc000000.usb"，换板 / 换内核就可能指到别的控制器。这里给出唯一口径，
//   供 HidRuntime（运行时）与 ttbox-hid-health（诊断工具）共用 —— 两处各写一份迟早分叉。
//
// 优先级：显式配置 > USB_PROXY_DEVICE 环境变量 > /sys/class/udc 下名字排序首个。
//   环境变量这一层与 usbproxy/board/run-ttbox-usb-proxy.sh 同一覆盖链
//   （登记于 docs/protocols/config-path-env-registry.md）。
//   都没有 ⇒ 返回空串；调用方必须据此走"跳过快路径 / 报错"分支，不得假装成功。
//
// 仅 Unix（依赖 dirent.h 与 /sys）；Windows 分支不包含本头。
#pragma once

#include <dirent.h>

#include <cstdlib>
#include <string>

namespace ttbox::core {

// /sys/class/udc 下名字排序首个（无控制器 / 打不开该目录 ⇒ 空串）。
// 取"排序首个"而不是"随便一个"：同一台机器上结果必须稳定、可复现。
inline std::string enumerate_first_udc() {
    DIR* d = ::opendir("/sys/class/udc");
    if (!d) return std::string();
    std::string best;
    struct dirent* e;
    while ((e = ::readdir(d)) != nullptr) {
        const char* n = e->d_name;
        if (n == nullptr || n[0] == '.') continue;
        if (best.empty() || std::string(n) < best) best = n;
    }
    ::closedir(d);
    return best;
}

// 解析 UDC：显式配置 > USB_PROXY_DEVICE > 枚举。
inline std::string resolve_udc(const std::string& configured) {
    if (!configured.empty()) return configured;
    if (const char* env_udc = std::getenv("USB_PROXY_DEVICE")) {
        if (*env_udc) return std::string(env_udc);
    }
    return enumerate_first_udc();
}

}  // namespace ttbox::core
