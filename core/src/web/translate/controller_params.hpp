// controller_params.hpp — 控制器参数块的表驱动读写。
//
// 自 plugins/web/lib/controller_params.py 逐行为移植。表项里的默认值必须与
// core/src/mouse/MouseTypes.hpp 的结构体默认值一字不差（面板首次回填显示的就是它）。
#pragma once

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include "common/Json.hpp"

namespace ttbox::core::web {

// 字段类型标记：与 Python 的 'n'/'i'/'b'/'key' 一一对应。
enum class CtrlFieldKind { kNumber, kInt, kBool, kKey };

// 表驱动搬运的单个字段：core 字段名 + 类型 + 缺省回填值。
struct CtrlField {
    std::string name;         // core 字段名
    CtrlFieldKind kind;
    JsonValue default_value;  // 缺省回填值（= MouseTypes.hpp 结构体默认）
};

// 一个子对象块：面板扁平键前缀 + mouse 子对象名 + 字段表。
struct CtrlBlock {
    std::string prefix;   // 面板扁平键前缀（如 "recoil_"）
    std::string obj_key;  // mouse 子对象名（如 "recoil"）
    std::vector<CtrlField> fields;
};

// controller 数值直通字段（Web key → mouse key）。
const std::vector<std::pair<std::string, std::string>>& controller_nums();
// controller 字符串枚举字段（Web key → mouse key）。
const std::vector<std::pair<std::string, std::string>>& controller_strings();
// controller 布尔直通字段（现役全为嵌套结构开关，见 .cpp 注释）。
const std::vector<std::pair<std::string, std::string>>& controller_bools();
// 表驱动搬运块（recoil / trigger2）。
const std::vector<CtrlBlock>& ctrl_blocks();

// 面板值 → Core 类型。非法返回 JsonValue::null()（调用方跳过该字段）。
JsonValue coerce_ctrl_value(const JsonValue& value, CtrlFieldKind kind);

// 面板扁平键 → Core 子对象（只收面板真的传了的字段）。
JsonValue ctrl_read_block(const JsonValue& ctrl, const CtrlBlock& block);

// Core 子对象 → 面板扁平键（缺字段补 Core 默认值）。
void ctrl_write_block(JsonValue& ctrl, const CtrlBlock& block, const JsonValue* blk);

// 一维数组透传（长度不符返回 null，整套跳过）。
JsonValue ctrl_read_vec(const JsonValue& ctrl, const std::string& key, int n);

// 二维数组透传（形状不符返回 null，整套跳过）。
JsonValue ctrl_read_table(const JsonValue& ctrl, const std::string& key, int rows, int cols);

// 面板 crop_size → Core 合法值（0=全帧，或 64~3840；绝不产生 1~63）。
int64_t normalize_capture_crop_size(const JsonValue& value);

}  // namespace ttbox::core::web
