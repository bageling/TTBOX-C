// hotkeys.hpp — 热键位与档位映射（Web 名 ↔ 位掩码）。
//
// 自 plugins/web/lib/hotkeys.py 逐行为移植。Web 面用名字（'left'/'right'/...），
// core 面用位掩码（1/2/4/...）。本模块是两者之间唯一的换算点。
#pragma once

#include <cstdint>
#include <string>

#include "common/Json.hpp"

namespace ttbox::core::web {

// 合法热键位掩码判定：只认五个单键位 {1,2,4,8,16}（左/右/中/侧1/侧2）。
// 组合掩码（如 3=左|右）core 侧能用但面板无法回填，故面板这一层拒掉。
bool is_valid_hotkey_bit(int64_t bits);

// Web 热键字符串（'left'/'right'/''）→ 位掩码。字符串按名查表（strip + lower）；
// 数字直接转 int；其余回退 default_bits。
int64_t hotkey_to_bits(const JsonValue& value, int64_t default_bits);

// 位掩码 → Web 热键字符串（0 或未识别 → 空串）。
std::string bits_to_hotkey(int64_t bits);

// 位掩码 → 英文名（供错误文案用，如 'left'；未识别 → 空串）。与 bits_to_hotkey 同源。
std::string hotkey_name(int64_t bits);

// Core 的 mouse.hotkey_guard → 前端控件值（缺字段补 Core 结构体默认：
// enabled=false / toggle_hotkey=middle）。挂起状态是运行期状态，不在这里。
JsonValue hotkey_guard_to_web(const JsonValue& hotkey_guard);

}  // namespace ttbox::core::web
