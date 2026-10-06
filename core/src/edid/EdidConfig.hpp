// EdidConfig.hpp — EDID 配置归一与模式策略（自 scripts/edid/mode_builder.py 逐行移植）。
//
// 职责：把用户/面板给的原始配置（JSON）洗成**受控字段集**，再决定"广播哪些显示模式"。
// 逐字节/逐值等价性由 core/tests/test_edid_golden.cpp 的 E/F 段断言。
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "common/Json.hpp"

namespace ttbox::core::edid {

// ---- Python 侧三个归一工具（F 段对拍）----

// Python _safe_ascii(value, limit, fallback)：只留可打印 ASCII(32..126)，截断到 limit，
// 结果为空则 fallback。
// ★ 非 ASCII 一律被丢弃：C++ 按**字节**判（UTF-8 多字节每字节都 >126）
//   ⇒ 与 Python 按**字符**判的结论相同（中文在两侧都变空串）。
std::string safe_ascii(const std::string& value, size_t limit, const std::string& fallback);

// Python _hex_text(value, width, fallback)：无 "0x" 前缀按**十进制**解析，有则按十六进制；
// 取值须满足 0 < n < 16^width，否则 fallback；通过则格式化为 0x%0*x（小写）。
std::string hex_text(const std::string& value, int width, const std::string& fallback);

// Python _bool_value(value, fallback)：bool 原样；数字非零为真；字符串
// {"1","true","yes","on"}/{ "0","false","no","off"} 判定，其余 fallback。
bool bool_value(const JsonValue& value, bool fallback);

// ---- 常量表（meta 段对拍）----
const std::vector<std::string>& profiles_set();
const std::vector<std::string>& native_modes_set();
int max_advertised_modes();

// ---- 面板「模式列表」（自旧 Python hardware.py::_probe_edid_modes 移植）----
// 结构：{token, label, width, height, refresh, pixel_clock_khz}
//   · token = timing token（如 "1440p144"）
//   · label = "宽x高@刷新"（如 "2560x1440@144"）
//   · 顺序 = TIMING_MAP 顺序
// ★ 真相澄清：旧实现是调 `hdmirx_edid --list` 再**解析其文本输出**，
//   而那个命令打印的正是 TIMING_MAP 本身（不是读 EDID、不是读显示器）
//   ⇒ 本函数直接由时序表生成，结果等价且不再需要外部进程。
JsonValue advertised_modes_json();

// 面板用：把模式列表截断到 16 条（旧 Python 的 `advertised[:16]`）
JsonValue advertised_modes_json_truncated(size_t limit);

// ---- E 段：edid_apply.sh::PYEOF 的 native_mode 保护 ----
// native_mode 为空 / "auto" / 非法（mode_info 解析不出）⇒ 用 profile 首选模式；
// profile 未知则兜底 "1080p60compat"。
std::string resolve_native_mode(const std::string& profile, const std::string& native_mode);

// ---- 显示器信息（来自 EdidMonitor；此处只承载数据，便于纯逻辑对拍）----
struct MonitorMode {
    int width = 0;
    int height = 0;
    int refresh = 0;
};

struct MonitorInfo {
    bool connected = false;
    bool edid_valid = false;
    std::string name = "unknown";
    std::string vendor = "???";
    std::string product_id = "0x0000";
    std::string serial = "0x00000000";
    std::string connector;
    int native_width = 0;
    int native_height = 0;
    std::vector<MonitorMode> modes;
};

// ---- 归一后的配置 ----
struct DisplayConfig {
    std::string device = "auto";       // "auto" 或 /dev/videoN
    std::string profile = "boot-safe-full";
    std::string native_mode;           // 空表示未指定
    bool native_only = false;
    bool loopout_enabled = false;
    std::string name = "OPI-COMPAT";
    std::string vendor = "OPI";
    std::string product_id = "0x3588";
    std::string serial = "0x20260414";
    // 显式追加的广播模式。★ 注意：load_config **不产生**该字段（与 Python 同），
    // 只有 apply_monitor_to_config（环出）才填入 tokens[1:]。
    std::vector<std::string> added_modes;

    // 转成喂给 build_from_config 的 JSON（含 native_mode 保护后的值）
    JsonValue to_json() const;
};

// Python load_config（不含 loopout 覆盖 —— 那一步需读真实显示器，见 apply_monitor_to_config）
DisplayConfig load_config(const JsonValue& raw);

// Python build_display_mode_tokens_for_config
std::vector<std::string> build_display_mode_tokens(const DisplayConfig& cfg,
                                                   const MonitorInfo& monitor);

// Python apply_real_monitor_to_config（loopout 环出：用真实显示器身份覆盖配置）
DisplayConfig apply_monitor_to_config(DisplayConfig cfg, const MonitorInfo& monitor);

// ---- monitor.py 的纯判定工具（供模式策略使用）----
bool monitor_has_resolution(const MonitorInfo& m, int width, int height);
bool monitor_supports_refresh(const MonitorInfo& m, int width, int height, int refresh);
bool at_or_below_native(const MonitorInfo& m, int width, int height);

}  // namespace ttbox::core::edid
