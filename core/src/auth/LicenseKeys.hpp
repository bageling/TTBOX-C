#pragma once
// LicenseKeys.hpp — M2.01：内嵌 License 验签公钥（单一权威）
//
// ★ 密钥分族（路线 §1.3 裁决 1）：License 与 OTA **分用独立 Ed25519 密钥对**
//   （授权面与固件面解耦，一台泄露不连坐）。本密钥族 key_id = ttbox-license-2026a。
//
// ★ 公钥必须内嵌进二进制（不放外部文件）：
//   若验签公钥落盘为普通文件，攻击者可连同卡一起替换 ⇒ 验签形同虚设。
//   内嵌后替换公钥 = 重新编译 core = 与固件完整性（OTA 验签）绑定。
//
// ★ 换钥流程：商户侧 `ttbox_license_gen.py gen-key` 生成新族 ⇒ 用
//   `emit-c-header` 子命令重新生成本文件 ⇒ 提交 ⇒ 旧 key_id 卡自动失效
//   （verify 侧 key_id 不匹配 ⇒ kInvalidCard，fail-closed）。
//   公钥原文（hex）同步落在 tools/license/keys/<key_id>.pub.hex 供对账。
#include <array>
#include <cstdint>
#include <cstring>
#include <string>

namespace ttbox::core::auth {

// 当前生效的 License 验签公钥（Ed25519 raw 32B，hex 小写）。
inline const char* kLicenseKeyId = "ttbox-license-2026a";
inline const char* kLicensePublicKeyHex =
    "e4fd2ba32a1e2ab7d39e832bdefec8dd6cea876d2255c4464188093ea0ffff41";

// hex（64 字符）→ 32 字节。失败返回 false（长度/字符集不符）。
inline bool license_public_key(std::array<uint8_t, 32>* out) {
    const char* h = kLicensePublicKeyHex;
    for (int i = 0; i < 32; ++i) {
        int hi = 0, lo = 0;
        const char c1 = h[2 * i], c2 = h[2 * i + 1];
        if (c1 >= '0' && c1 <= '9') hi = c1 - '0';
        else if (c1 >= 'a' && c1 <= 'f') hi = c1 - 'a' + 10;
        else return false;
        if (c2 >= '0' && c2 <= '9') lo = c2 - '0';
        else if (c2 >= 'a' && c2 <= 'f') lo = c2 - 'a' + 10;
        else return false;
        (*out)[static_cast<size_t>(i)] =
            static_cast<uint8_t>((hi << 4) | lo);
    }
    return true;
}

}  // namespace ttbox::core::auth
