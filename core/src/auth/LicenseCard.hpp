#pragma once
// LicenseCard.hpp — M2.01：离线授权卡（信封格式 + canonical 串 + 解析/归一化）
//
// 卡 = 单文件 JSON 信封（激活时整段粘贴进 Web / 放 /etc/ttbox/license.key 一行）：
//   {
//     "license": {
//       "license_id": "ttbox-lic-2026-0001",   // [A-Za-z0-9_-]{1,64}
//       "device":     "5214e9fb06e8",           // = DeviceFingerprint.bind_string()
//       "plan":       "subscription",            // none|trial|subscription|permanent
//       "is_pro":     true,
//       "features":   ["capture","inference","aim","ota"],   // 闭集子集
//       "ui_brand":   "acme",                    // [A-Za-z0-9_-] ≤32（sanitize 规则）
//       "issued_at":  1758096000,                // unix 秒
//       "expires_at": 0                          // unix 秒；0 = 永久
//     },
//     "key_id":    "ttbox-license-2026a",
//     "signature": "<base64(64B Ed25519)>"
//   }
//
// ★ canonical 签名原文（两侧逐字节一致；Python 生成侧见
//   tools/license/ttbox_license_gen.py 的 canonical_license()）：
//     按字段名字母序，每字段两行 "key\nvalue\n"：
//     device\n<device>\n
//     expires_at\n<十进制无前导零>\n
//     features\n<闭集序逗号连接，空集=空行>\n
//     is_pro\n<0|1>\n
//     issued_at\n<十进制>\n
//     license_id\n<license_id>\n
//     plan\n<plan>\n
//     ui_brand\n<ui_brand>\n
//   所有字段都来自受限字符集（[A-Za-z0-9_-,]），**无需** JSON 转义 ⇒
//   规避跨语言 canonical-JSON 差异（键序/浮点/转义三陷阱，见 t1.11 §6 陷阱 3 同族）。
//
// ★ fail-closed 纪律：parse 阶段任一字段违例 ⇒ 整卡拒绝（不走 sanitize 兜底，
//   除非 ui_brand——sanitize_ui_brand 本身即闭集过滤、回落默认品牌是既定语义）。
//   features 归一化复用 LicenseStateMachine::normalize_features（唯一权威闭集）。
//
// 依赖：仓库内置 Json（无 OpenSSL / 无网络 / 无文件 I-O）⇒ AUTH=ON/OFF 都编译
// （CORE_SOURCES 无条件列表，同 LicenseStore/LicenseGate 纪律）。
#include <cstdint>
#include <string>
#include <vector>

namespace ttbox::core::auth {

// 解析后的离线卡（字段已归一化：features 过闭集+闭集序，ui_brand 过 sanitize）。
struct LicenseCard {
    std::string license_id;    // [A-Za-z0-9_-]{1,64}
    std::string device;        // 绑定串（bind_string() = cpu_serial）
    std::string plan;          // none|trial|subscription|permanent
    bool is_pro = false;
    std::vector<std::string> features;  // 闭集子集，按 known_features() 序
    std::string ui_brand;       // sanitize 后（空 ⇒ "ttbox"）
    int64_t issued_at = 0;      // unix 秒（≥0）
    int64_t expires_at = 0;     // unix 秒（≥0；0 = 永久）
    std::string key_id;         // 信封层
    std::string signature_b64;  // 信封层：base64(64B)
};

// canonical 签名原文（见头注释格式）。输入须为**已归一化**的 LicenseCard
// （即 parse_license_card 的产物；对未归一化输入行为未定义——由 parse 保证）。
std::string license_canonical(const LicenseCard& c);

// 解析 + 归一化。失败 ⇒ false + error（fail-closed，不部分接受）。
// 注意：本函数**不验签**（纯逻辑，host 可单测）；验签在 OfflineCardClient。
bool parse_license_card(const std::string& envelope_json, LicenseCard* out,
                        std::string* error);

// 标准 base64 解码（LicenseCard / 通用）。失败 ⇒ false。
bool license_base64_decode(const std::string& in, std::vector<uint8_t>* out);

// plan 合法值（none|trial|subscription|permanent）。
bool is_valid_plan(const std::string& plan);

}  // namespace ttbox::core::auth
