// EdidValidator.hpp — EDID 二进制合规校验（自 scripts/edid/validator.py 逐行移植）。
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace ttbox::core::edid {

// 返回错误列表；空 = 通过（对应 Python validate_edid）。
// 长度不为 256 时**立即返回**（与 Python 同：后续索引不再执行）。
std::vector<std::string> validate_edid(const std::vector<uint8_t>& edid);

// (通过?, 错误列表) —— 对应 Python verify_edid。
bool verify_edid(const std::vector<uint8_t>& edid, std::vector<std::string>* errors);

}  // namespace ttbox::core::edid
