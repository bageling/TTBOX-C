// core/src/auth/ed25519/ed25519_verify.hpp —— Ed25519 验签（verify-only，自包含）
//
// 出处与许可：TweetNaCl 20140427（公有领域，https://tweetnacl.cr.yp.to/）。
// 本仓只提取 **crypto_sign_open 验签路径**（SHA-512 + Ed25519 群/域运算 + unpackneg），
// 刻意不引入：salsa20/secretbox/onetimeauth/随机数/crypto_sign（**板端只验不签**，
// 最小攻击面）。签名侧由商户工具 tools/license/ttbox_license_gen.py（cryptography
// 库）承担，两侧以 RFC 8032 测试向量 + 发卡往返测试互证。
//
// 为什么不用 OpenSSL EVP：出货向量强制 TTBOX_CORE_BUILD_AUTH=OFF（ttbox_build_release.sh
// 0b 配置向量 + CMakeCache 一致性门禁），且 A32「随包 ELF 运行期依赖闭集」已冻结 ——
// 引入 libcrypto 会新增 NEEDED 并翻 M1 构建契约。本实现零外部依赖（纯 C++），
// AUTH=ON/OFF 两种构建都编译（归入 CORE_SOURCES 无条件列表）。
//
// 性能说明：TweetNaCl 的 ref 级实现，单次验签毫秒级 —— 授权验证频率为心跳级
// （默认 1h 一次 + 激活时一次），性能无关紧要；正确性优先（见测试向量）。
#pragma once

#include <cstddef>
#include <cstdint>

namespace ttbox::core::auth::ed25519 {

// 验签：sig 64 字节（R‖S），msg 任意长，pk 32 字节公钥。
// 返回 true ⇔ 签名有效（TweetNaCl crypto_sign_open 语义：常量时间比较，失败无副作用）。
bool verify(const uint8_t* sig, const uint8_t* msg, size_t msg_len,
            const uint8_t* pk);

}  // namespace ttbox::core::auth::ed25519
