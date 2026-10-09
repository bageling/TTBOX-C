// OtaCanonical.hpp — OTA 验签用的**规范化 JSON 序列化**（自 ttbox_ota_updater.py::canonical 移植）。
//
// ★ 为什么必须逐字节等价：Ed25519 签名是对 canonical(...) 的**字节**签的。
//   键序、空格、转义方式、非 ASCII 处理，任何一处不同 ⇒ 验签必然失败 ⇒ 板子升不了级。
//   基准 = core/tests/fixtures/ota_golden.json 的 A 段（由现役 Python 实现冻结）。
//
// ★ 为什么不复用 common/Json.hpp 的 JsonValue::dump()：
//   那个函数是**通用**序列化，格式（排序策略、转义策略）可能随需求变化；
//   而本函数是**签名协议的一部分**，必须由自己锁死 + 单独对拍。
//   两者混用 = 一次"顺手优化通用 dump"就会静默打断 OTA 验签。
//
// 目标语义（= Python json.dumps(rec, sort_keys=True, separators=(",", ":"), ensure_ascii=False)）：
//   · 键按**码点**升序（UTF-8 字节序与码点序一致 ⇒ 用 std::map 的字节序即可）
//   · 分隔符 ',' 与 ':' 后**无空格**
//   · 非 ASCII **原样输出**（不转成 \uXXXX）
//   · 字符串转义遵循 JSON 标准：\" \\ \b \f \n \r \t，其余 <0x20 用 \u00XX
//   · true / false / null 小写；整数无小数点
#pragma once

#include <string>

#include "common/Json.hpp"

namespace ttbox::core::ota {

// Python canonical(rec) 的等价实现。
std::string canonical_dump(const JsonValue& v);

// 只取 SIGNED_FIELDS 子集后再 canonical（Python：{k: rec[k] for k in SIGNED_FIELDS}）。
// fields 顺序无关紧要 —— canonical 内部会排序。
std::string canonical_signed_fields(const JsonValue& rec,
                                    const std::vector<std::string>& signed_fields);

}  // namespace ttbox::core::ota
