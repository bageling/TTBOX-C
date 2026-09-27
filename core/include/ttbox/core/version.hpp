// version.hpp — TTBox C++ Core 公共版本头（对外 include 入口）
#pragma once

namespace ttbox::core {

inline constexpr const char* kAppName = "ttbox_core";
// B-CONST-3：版本语义三名分离（三个**不同事实**，禁止合并为同一字符串/重复硬编码）：
//   · kCoreVersion          = 内核内部版本（仅日志/诊断，core 自述）；
//   · kAppVersion           = 面板对外可见版本（web: plugins/web/bin/ttbox-web.py）；
//   · TTBOX_RELEASE_VERSION = 出货留档版本（构建/安装脚本消费）。
inline constexpr const char* kCoreVersion = "1.5.62";

}  // namespace ttbox::core
