// test_ed25519.cpp — M2.01 单元测试：Ed25519 验签原语（TweetNaCl 提取路径）
//
// 覆盖（正确性锚 = RFC 8032 §7.1 官方测试向量 + 篡改负控）：
//   · TEST 1（空消息）/ TEST 2（1 字节消息）官方向量必须通过；
//   · 篡改消息 1 字节 / 篡改签名 1 字节 / 篡改公钥 1 字节 ⇒ 必须拒绝；
//   · 长度防御（签名 < 64B / 空消息 nullptr+0 合法）。
// 框架：ttbox_test（TEST/CHECK，禁裸 assert）。AUTH 无关（ed25519 无条件编译）。
//
// ★ 为什么用 RFC 官方向量：本仓的 ed25519_verify.cpp 是逐字提取的公有域实现，
//   浮点/进位/常量任何一处抄错 ⇒ 验签必然失败（不会"错得恰好通过"）——
//   官方向量是数学层面的可证伪判据（呼应 P0-2b 元规则 (a) 有可失败路径）。
#include "test_util.hpp"

#include "auth/ed25519/ed25519_verify.hpp"

#include <cstring>
#include <string>

using ttbox::core::auth::ed25519::verify;

namespace {

int hexval(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

std::string unhex(const char* s) {
    std::string out;
    for (size_t i = 0; s[i] && s[i + 1]; i += 2) {
        out.push_back(static_cast<char>((hexval(s[i]) << 4) | hexval(s[i + 1])));
    }
    return out;
}

const uint8_t* b(const std::string& s) {
    return reinterpret_cast<const uint8_t*>(s.data());
}

}  // namespace

// RFC 8032 §7.1 TEST 1：空消息。
TEST(ed25519_rfc8032_test1_empty_message) {
    const std::string pk = unhex(
        "d75a980182b10ab7d54bfed3c964073a0ee172f3daa62325af021a68f707511a");
    const std::string sig = unhex(
        "e5564300c360ac729086e2cc806e828a84877f1eb8e5d974d873e06522490155"
        "5fb8821590a33bacc61e39701cf9b46bd25bf5f0595bbe24655141438e7a100b");
    CHECK(verify(b(sig), nullptr, 0, b(pk)));
}

// RFC 8032 §7.1 TEST 2：1 字节消息 0x72。
TEST(ed25519_rfc8032_test2_one_byte) {
    const std::string pk = unhex(
        "3d4017c3e843895a92b70aa74d1b7ebc9c982ccf2ec4968cc0cd55f12af4660c");
    const std::string sig = unhex(
        "92a009a9f0d4cab8720e820b5f642540a2b27b5416503f8fb3762223ebdb69da"
        "085ac1e43e15996e458f3613d0f11d8c387b2eaeb4302aeeb00d291612bb0c00");
    const std::string msg = std::string("\x72", 1);
    CHECK(verify(b(sig), b(msg), msg.size(), b(pk)));
}

// 负控①：篡改消息 1 字节 ⇒ 必须拒绝。
TEST(ed25519_reject_tampered_message) {
    const std::string pk = unhex(
        "3d4017c3e843895a92b70aa74d1b7ebc9c982ccf2ec4968cc0cd55f12af4660c");
    const std::string sig = unhex(
        "92a009a9f0d4cab8720e820b5f642540a2b27b5416503f8fb3762223ebdb69da"
        "085ac1e43e15996e458f3613d0f11d8c387b2eaeb4302aeeb00d291612bb0c00");
    const std::string msg = std::string("\x73", 1);  // 0x72 → 0x73
    CHECK(!verify(b(sig), b(msg), msg.size(), b(pk)));
}

// 负控②：篡改签名末字节 ⇒ 必须拒绝。
TEST(ed25519_reject_tampered_signature) {
    const std::string pk = unhex(
        "d75a980182b10ab7d54bfed3c964073a0ee172f3daa62325af021a68f707511a");
    std::string sig = unhex(
        "e5564300c360ac729086e2cc806e828a84877f1eb8e5d974d873e06522490155"
        "5fb8821590a33bacc61e39701cf9b46bd25bf5f0595bbe24655141438e7a100b");
    sig[sig.size() - 1] = static_cast<char>(sig[sig.size() - 1] ^ 0x01);
    CHECK(!verify(b(sig), nullptr, 0, b(pk)));
}

// 负控③：篡改公钥 ⇒ 必须拒绝（同签名换钥匙 = 签名对不上另一把钥匙）。
TEST(ed25519_reject_tampered_public_key) {
    std::string pk = unhex(
        "d75a980182b10ab7d54bfed3c964073a0ee172f3daa62325af021a68f707511a");
    const std::string sig = unhex(
        "e5564300c360ac729086e2cc806e828a84877f1eb8e5d974d873e06522490155"
        "5fb8821590a33bacc61e39701cf9b46bd25bf5f0595bbe24655141438e7a100b");
    pk[0] = static_cast<char>(pk[0] ^ 0x01);
    CHECK(!verify(b(sig), nullptr, 0, b(pk)));
}

// 长度防御：签名不足 64B ⇒ 拒绝（不越界读）。
TEST(ed25519_reject_short_signature) {
    const std::string pk = unhex(
        "d75a980182b10ab7d54bfed3c964073a0ee172f3daa62325af021a68f707511a");
    const std::string short_sig(63, '\x01');
    CHECK(!verify(b(short_sig), nullptr, 0, b(pk)));
}

// 参数防御：空指针 + 非零长度 / 空指针签名 ⇒ 拒绝。
TEST(ed25519_reject_null_args) {
    const std::string pk = unhex(
        "d75a980182b10ab7d54bfed3c964073a0ee172f3daa62325af021a68f707511a");
    const std::string sig = unhex(
        "e5564300c360ac729086e2cc806e828a84877f1eb8e5d974d873e06522490155"
        "5fb8821590a33bacc61e39701cf9b46bd25bf5f0595bbe24655141438e7a100b");
    CHECK(!verify(b(sig), nullptr, 5, b(pk)));  // msg=nullptr 但 len=5
    CHECK(!verify(nullptr, nullptr, 0, b(pk)));
}
