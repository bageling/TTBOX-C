// OtaCrypto.hpp — OTA 验签与摘要（自 ttbox_ota_updater.py 移植）。
//
// ★ 为什么**不用** OpenSSL：出货向量强制 TTBOX_CORE_BUILD_AUTH=OFF，且"随包 ELF 运行期依赖闭集"
//   已冻结（引入 libcrypto 会新增 NEEDED 并翻 M1 构建契约）。本仓已有**自包含**的
//   auth/ed25519/ed25519_verify（TweetNaCl 验签路径，零外部依赖，且已与 Python cryptography
//   互证过）⇒ OTA 直接复用，不引新依赖。
//
// ★ 为什么 SHA-256 自己实现：同样不能引 OpenSSL；而 manifest 校验要对 135 个文件逐个算，
//   调 `sha256sum` 命令会起 135 个进程。正确性由 RFC 6234 测试向量在单测里钉死。
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace ttbox::core::ota {

// ---- SHA-256（自实现；单测用 RFC 6234 / 常用向量验证）----
std::string sha256_hex(const uint8_t* data, size_t len);
std::string sha256_hex(const std::string& s);
// 整文件摘要（分块读，避免大包进内存）。读失败返回 false。
bool sha256_file_hex(const std::string& path, std::string* out_hex);

// ---- Ed25519 公钥装载 ----
// 从 PEM（"-----BEGIN PUBLIC KEY-----" 文本）提取 raw 32 字节公钥。
// 做法：base64 解码 ⇒ 校验 Ed25519 SPKI 固定 12 字节头 30 2a 30 05 06 03 2b 65 70 03 21 00
//       ⇒ 取其后 32 字节。任何不符即 false（fail-closed）。
bool load_ed25519_pem(const std::string& pem, uint8_t out[32]);

// ---- 验签 ----
// sig 64 字节（R‖S）；msg 是 canonical 串的字节（★ 必须与签名时**逐字节**一致）。
bool verify_ed25519(const uint8_t sig[64], const std::string& msg, const uint8_t pk[32]);

// ---- base64（复用 auth 的既有实现，签名解码用）----
bool base64_decode(const std::string& in, std::vector<uint8_t>* out);

}  // namespace ttbox::core::ota
