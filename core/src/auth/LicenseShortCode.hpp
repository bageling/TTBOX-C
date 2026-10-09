#pragma once
// LicenseShortCode.hpp — M2.05：卡号「可读短码 + 校验位」（纯逻辑单元）
//
// 目的（路线 §1.2 / M2.05）：把签名卡里的 license_id（形如
// "ttbox-lic-20260917-3842ff"）派生为一张**人能念、人能录**的短码，
// 唯一作用是「防抄错 + 人眼对账」——商户台账记 (license_id, short_code) 二元组，
// 客服念短码、用户报短码，一位录错即被校验位拦下。
//
// ★ 非安全边界（红线，勿误用）：
//   短码的算法用**非密码学**摘要（FNV-1a-32），只防「手抄笔误」，**不防伪造**。
//   真正的准入判据永远是 Ed25519 签名（OfflineCardClient）。
//   ⇒ 短码**不得**被任何拒绝/放行路径当作准入判据（verify 只查校验位、不查签名）。
//
// ★ 单一实现（跨语言逐字节一致）：
//   本文件 + .cpp 是 C++ 侧唯一实现；Python 侧唯一实现 =
//   tools/license/ttbox_license_gen.py 的 short_code()/_fnv1a32/_crockford32_7/_luhn_mod32。
//   两侧任何**单方**改动都会让跨语言向量测试红灯（这是特性，不是缺陷）——
//   改算法必须两侧同改，并把新向量同时钉进
//   core/tests/test_license_shortcode.cpp 与 tools/license/test_license_gen.py。
//
// ★ 依赖纪律：只用 <cstdint> / <string>，**无平台分支**（POSIX/Win32 逐字节同结果），
//   无 OpenSSL、无网络、无文件 I-O ⇒ 无条件编入 CORE_SOURCES（AUTH=ON/OFF 两种构建都要编）。
//
// 格式：TTB-XXXX-XXXX      （前缀 "TTB" + 两段 4 字符，段间以 '-' 分隔）
//   后 8 字符 = 7 位 Crockford-Base32(FNV-1a-32(license_id)) + 1 位 Luhn-mod-32 校验位。
//   字符集 = 0123456789ABCDEFGHJKMNPQRSTVWXYZ（Crockford，排除易混 I L O U）。
//   归一：O→0，I→1，L→1；大小写不敏感；'-' 与前缀 'TTB' 不参与计算。
//
// ★ 关于「长度」的实测口径（供账本与验收脚本引用，勿把两种读法混淆）：
//   · 派生串字面 = "TTB-XXXX-XXXX" ⇒ 实测长度 = **13**（"TTB" 3 + body 8 + 分隔符 2）。
//   · 权威格式 = 规格 §2.4 的字面构造 `"TTB-" + 4 + "-" + 4` 与 B11 的 `TTB-XXXX-XXXX`（两者一致）。
//   · 规格 §3.4 写的「长度恒 11」与上述字面构造**不一致**（11 = "TTB" 3 + body 8，漏计 2 个分隔符）；
//     本实现以**字面格式串**为准，并在交付报告登记该处规格笔误。
//   · `verify` 对「有分隔符 / 无分隔符 / 无前缀」三种写法**都接受**（见 .cpp），
//     因此版端若按"长度 11 的无分隔符形"检查亦可通过。
#include <cstdint>
#include <string>

namespace ttbox::core::auth {

// 短码字符集 = Crockford Base32（排除易混 I L O U）。32 字符，下标即"数值"。
inline const char* kShortCodeAlphabet = "0123456789ABCDEFGHJKMNPQRSTVWXYZ";
// 短码前缀（人眼识别用；不参与校验位计算）。
inline constexpr const char* kShortCodePrefix = "TTB";
// body = 7 位数据 + 1 位校验 = 8 字符。
inline constexpr int kShortCodeBodyLen = 8;
// Luhn 基数 = 字符集大小。
inline constexpr int kShortCodeRadix = 32;

// ① FNV-1a-32（offset basis 0x811C9DC5；prime 0x01000193；逐字节 XOR 后 32-bit 溢出回绕）。
uint32_t short_code_fnv1a32(const std::string& bytes);

// ② 32 位 → 7 个 Crockford 字符（按 5 位一组、从高位到低位；最高字符仅用 2 位 = 35>32 的余量）。
std::string short_code_body7(uint32_t h);

// ③ Luhn mod 32 校验字符（alphabet 下标参与；标准 Luhn-mod-N，见 .cpp 注释）。
//    传入 body（7 位数据）；body 含非 alphabet 字符 ⇒ 返回 '\0'（调用方视为不可派生）。
char short_code_luhn_check(const std::string& body);

// 人脸可读短码："TTB-" + 4 + "-" + 4（第 8 个字符 = 校验位）。
// license_id 非法（见 license_short_code_is_valid_id）⇒ 返回空串（fail-closed，绝不臆造）。
std::string license_short_code_derive(const std::string& license_id);

// 校验（去分隔/前缀 → Crockford 归一 O→0,I/L→1 → 跑 Luhn）；true = 校验位通过。
// ★ 只查校验位，**不查签名**（非安全边界，见文件头）。
bool license_short_code_verify(const std::string& short_code);

// license_id 合法性：[A-Za-z0-9_-]{1,64}（与 LicenseCard 契约一致）。
bool license_short_code_is_valid_id(const std::string& license_id);

}  // namespace ttbox::core::auth
