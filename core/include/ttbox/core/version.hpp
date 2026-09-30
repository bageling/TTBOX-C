// version.hpp — TTBox C++ Core 公共版本头（对外 include 入口）
#pragma once

namespace ttbox::core {

inline constexpr const char* kAppName = "ttbox_core";
// B-CONST-3：版本语义三名分离（三个**不同事实**，禁止合并为同一字符串/重复硬编码）：
//   · kCoreVersion          = 内核内部版本（仅日志/诊断，core 自述）；
//   · kAppVersion           = 面板对外可见版本（web: plugins/web/bin/ttbox-web.py）；
//   · TTBOX_RELEASE_VERSION = 出货留档版本（构建/安装脚本消费）。
//   ★ 2026-09-29 起产品版本改走「V 线」（业主令「全面改版本号」）：值形如 "V1.0.01"，带字母前缀。
//     为什么**必须**带 V：板端 OTA 的版本比较有三处同源实现
//     （scripts/ttbox_ota_updater.py::_version_key / plugins/web/bin/ttbox-web.py::_ota_ver_key /
//       云端 bridge ota.js::versionKey），它们把**非数字段**的排序权重排在数字段之上
//     ⇒ "V1.0.01" > "1.5.70"。若写成纯数字 "1.0.01"，已装 1.5.x 的盒子会判定为**降级**
//     （is_downgrade=true）⇒ 更新器直接拒装、面板也只显示「已是最新」，永远升不上来。
//     回归锁：plugins/web/tests/test_web_ota_version_scheme.py。
inline constexpr const char* kCoreVersion = "V1.0.11";

}  // namespace ttbox::core
