// Shell.hpp — 跑外部命令的公共封装（stdout+stderr 合并捕获）。
//
// 为什么要公共：OTA 更新器要调 tar / v4l2-ctl / systemctl，EDID 注入要调 v4l2-ctl。
// 此前 EdidApply.cpp 里自带一份（popen + pclose）；OTA 再抄一份就是第二处同款实现 ——
// 同款实现分叉是这个项目反复吃亏的地方（见 ttbox_ota_updater 的 `from lib.paths` 事故）。
//
// ★ 为什么用 popen 而不是 fork+exec：项目里 system.cpp 用了 sys/wait.h，导致本机 MSYS
//   **编不了**带它的目标。popen 是 C 标准 + POSIX 最小集，host 与板端都能编。
//   代价：pclose 只判「== 0」，不解析 WEXITSTATUS（那需要 sys/wait.h）。
#pragma once

#include <string>

namespace ttbox::core {

// 跑 `cmd`（交给 /bin/sh -c 解释），把 stdout+stderr 合并写进 out，返回 exit code。
// · 起不来（popen 返回 nullptr）⇒ 返回 -1
// · pclose != 0 ⇒ 返回 pclose 的原值（调用方通常只关心「是否 0」）
// ★ 调用方负责命令串的转义：这是 root 进程，命令注入风险由拼接方承担。
int run_capture_cmd(const std::string& cmd, std::string* out);

// 同上但不取输出（只关心退出码）
int run_quiet_cmd(const std::string& cmd);

// 单引号包裹（路径受控、来自配置或常量时使用）
std::string shell_quote(const std::string& s);

}  // namespace ttbox::core
