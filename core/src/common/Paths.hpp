// Paths.hpp — TTBOX Core 运行期路径单点真源（A-PATH-5 / B-CONST-2）
//
// 为什么需要本头：同一路径字面量（IPC socket / 鼠标透传 socket / license 文件）曾散落在
// C++/Python/shell 共 20+ 处；改一处漏一处即"连不上 core"。规约：
//   · C++ 侧这些默认值**只允许**在本头定义一次，其余 .cpp/.hpp 一律引用（B-CONST-1）；
//   · 跨语言（Python/shell/usbproxy）无法 include 本头 ⇒ 以
//     docs/protocols/config-path-env-registry.md 登记 + scripts/ttbox_conventions_gate.sh
//     同值断言保证不漂移（A-PATH-5）。
//
// 取值链（A-PATH-2）：CLI 参数 > 环境变量 > 本头默认。
#pragma once

namespace ttbox::core::paths {

// IPC 控制通道 socket 默认（env 覆盖名 = TTBOX_IPC_SOCKET）。
inline constexpr const char* kIpcSocketDefault = "/run/ttbox/core.sock";

// 鼠标透传（usb-proxy）控制/事件 socket 默认。与 usbproxy 进程**必须同值**才通
// （usbproxy/usb-proxy.cpp 的 mouse_cmd_socket / mouse_event_socket 由门禁断言对齐）。
inline constexpr const char* kMouseCmdSocketDefault = "/run/ttbox-mouse-passthrough/cmd.sock";
inline constexpr const char* kMouseEventSocketDefault = "/run/ttbox-mouse-passthrough/event.sock";

// 系统级授权凭据文件（--license > 本文件 > 配置 > Store，见 Application::resolve_license_card）。
inline constexpr const char* kSystemLicenseFile = "/etc/ttbox/license.key";

// 运行期状态目录（非版本目录，升级绝不清理）：版本留档 / OTA 状态 / 用户启停意愿等。
// 目录基址 = 环境变量 TTBOX_STATE（与 scripts/ttbox.sh 同源）> 本默认值；
// 该目录由 scripts/ttbox_release_install.sh 创建（mkdir -p），OTA 更新不触碰。
inline constexpr const char* kStateDirDefault = "/opt/ttbox/state";

// 用户启停意愿文件（R5）：只记录用户**显式** start/stop 的意愿，core 重启（含 OTA 更新后的
// systemctl restart）时据此还原 want_runtime_running_，避免"没点启动却自己跑起来"。
// 无该文件（首次开机 / 记录损坏）时保持编译期默认（自动启动推理）。
inline constexpr const char* kRuntimeIntentFileName = "runtime_intent.json";

// 上次启动时 core 自己的版本（R6）：与当前 kCoreVersion 不一致 ⇒ 中间发生过版本更替
// （OTA / 重装），据此把流水线停在停止态等用户手动启动。与 <state>/ota_status.json
// （更新器写的安装结果）配合，覆盖"升级到本特性首个版本"那一次（此时标记尚不存在）。
inline constexpr const char* kCoreBootVersionFileName = "core_boot_version";

// 更新器安装结果文件（更新器写、web `/api/update/status` 读，键 state/version）。
inline constexpr const char* kOtaStatusFileName = "ota_status.json";

// 日志目录默认（env 覆盖名 = TTBOX_LOG_DIR）。口径真源《代码书写规矩·技术版》§5.2：
//   FATAL/ERROR 直写并 fsync 到 <dir>/ttbox-error.log；WARN/INFO 进内存缓冲每 5 分钟
//   批量落盘到 <dir>/ttbox.log（「少写卡」#64）；客户操作同步写 <dir>/operation.log。
// 读者：web 面板「导出日志」= plugins/system_common.py::LogService（读 <dir>/ttbox.log
//   最后 500 行，只按行取、不解析格式）。目录由 scripts/ttbox_fhs_init.sh 创建
//   （ttbox:ttbox 0755）；目录缺失时 Logger 的 FileSink 自降级，不影响启动。
inline constexpr const char* kLogDirDefault = "/var/log/ttbox";

}  // namespace ttbox::core::paths
