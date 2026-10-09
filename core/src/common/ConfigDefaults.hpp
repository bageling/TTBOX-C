// ConfigDefaults.hpp — TTBOX Core 出厂默认值镜像（C-CFG-4 / B-CONST-1）
//
// 出厂默认的**唯一真源** = deploy/config/00-factory.json。本头只是其 C++ 镜像，
// 供「配置缺键」时兜底；逐键值由 scripts/ttbox_conventions_gate.sh 断言
// == 00-factory.json，杜绝「代码里再写一份默认值」的漂移（V-14 / §4-模式2）。
//
// 规约（C-CFG-4）：ConfigManager::get_*(key, def) 的 def **不得**成为第二个默认真源——
// 要么与本头同一常量一致（门禁断言），要么读不到即 fail-fast。禁止各处再写字面量。
#pragma once

#include <vector>

namespace ttbox::core::cfg {

// 00-factory.json: preview_fps
inline constexpr int kPreviewFpsDefault = 30;

// 00-factory.json: cpu_min_freq_percent
inline constexpr int kCpuMinFreqPercentDefault = 50;

// 00-factory.json: worker_cores = "1,2,4"（RK3588 三 NPU 独占掩码，位掩码非 CPU 号）。
// 配置缺失 / 全非法时的兜底组合；门禁断言 00-factory.worker_cores 逐字符 == "1,2,4"。
inline std::vector<int> default_worker_cores() { return {1, 2, 4}; }

}  // namespace ttbox::core::cfg
