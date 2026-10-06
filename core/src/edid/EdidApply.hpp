// EdidApply.hpp — EDID 应用流程（自 scripts/edid/edid_apply.sh 415 行逐段移植）。
//
// 这一段是整个 EDID 链最"硬件"的部分：写 sysfs 切 HPD、调 v4l2-ctl 注入并回读、
// 等 HDMI 输入锁、以及在**被 systemd 超时杀掉时把 HPD 拉回 on**（否则源端会一直黑屏）。
//
// ★ 本文件用 popen/system 而非 fork+execvp：项目里 system.cpp 用了 sys/wait.h，
//   导致本机 MSYS **编不了** ttbox_web。这里刻意只用 C 标准 + POSIX popen，
//   使 EdidApply 在本机也能编译（运行仍需 Linux 的 sysfs 与 v4l2-ctl）。
//   代价：pclose 只判「== 0」而不解析 WEXITSTATUS（那需要 sys/wait.h）。
//
// ★ 死路径已剔除（业主 2026-10-06 裁定）：不再写 /lib/firmware/ttbox/hdmirx_edid.bin
//   （07 册实测：全板端无任何读取方，只写只删）。
#pragma once

#include <string>

namespace ttbox::core::edid {

struct ApplyOptions {
    std::string prefix = "/opt/ttbox";  // TTBOX_PREFIX
    std::string config_path;            // 空 ⇒ <prefix>/config/hardware_display.json
    std::string output_path;            // 空 ⇒ <prefix>/runtime/edid/current.bin
    std::string video_dev = "/dev/video0";
    bool rehandshake = true;            // TTBOX_EDID_REHANDSHAKE（默认 1）
    int attempts = 2;                   // ATTEMPTS_DEFAULT（2026-09-28 由 12 下调为 2）
    int lock_timeout_sec = 10;          // TTBOX_EDID_LOCK_TIMEOUT_SEC
    double hpd_settle_sec = 0.5;        // TTBOX_EDID_HPD_SETTLE_SEC
    // 只构建 + 写 current.bin，不碰 HPD / 不注入（供本机预检与单测）
    bool dry_run = false;
};

struct ApplyResult {
    bool ok = false;
    bool edid_applied = false;
    bool locked = false;
    int exit_code = 1;
    std::string error;
    std::string json;          // 与脚本 stdout 同结构，供 core 原样转发
    std::string output_path;
    std::string mode_name;
};

// 入口：读配置 → native_mode 保护 → 构建 EDID → 写 current.bin → HPD 重协商 →
//       回读全字节比对 → 等输入锁 → 输出 JSON。
ApplyResult apply(const ApplyOptions& opt);

// ------------- 纯函数（无平台依赖，供单测）-------------

// 成功 JSON（对齐脚本的 stdout）：{ok:true, hpd:rehandshake|unchanged, version, method, file, mode}
std::string make_success_json(bool rehandshake, const std::string& file,
                              const std::string& mode_name);

// 失败 JSON：{ok:false, error, edid_applied, locked}
std::string make_failure_json(bool edid_applied, bool locked, const std::string& error);

// 判锁（debugfs /sys/kernel/debug/hdmirx/status）：四通道全 Lock 才算锁上。
bool debugfs_locked(const std::string& status_text);

// 判锁（v4l2-ctl --query-dv-timing）：无 "failed" / "No locks" 即视为锁上。
bool v4l2_timing_locked(const std::string& timing_text);

}  // namespace ttbox::core::edid
