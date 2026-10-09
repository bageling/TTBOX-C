// OtaCrypto.cpp — 见 OtaCrypto.hpp 的文件头说明。
#include "ota/OtaCrypto.hpp"

#include <cstdio>
#include <cstring>
#include <fstream>

#include "auth/LicenseCard.hpp"  // license_base64_decode
#include "auth/ed25519/ed25519_verify.hpp"

namespace ttbox::core::ota {

namespace {

// ---- SHA-256（FIPS 180-4）----
struct Sha256Ctx {
    uint32_t h[8] = {0x6a09e667u, 0xbb67ae85u, 0x3c6ef372u, 0xa54ff53au,
                     0x510e527fu, 0x9b05688cu, 0x1f83d9abu, 0x5be0cd19u};
    uint64_t total = 0;
    uint8_t buf[64] = {0};
    size_t buf_len = 0;
};

// SHA-256 轮常量 K[64]。
const uint32_t kK[64] = {
    0x428a2f98u, 0x71374491u, 0xb5c0fbcfu, 0xe9b5dba5u, 0x3956c25bu, 0x59f111f1u, 0x923f82a4u,
    0xab1c5ed5u, 0xd807aa98u, 0x12835b01u, 0x243185beu, 0x550c7dc3u, 0x72be5d74u, 0x80deb1feu,
    0x9bdc06a7u, 0xc19bf174u, 0xe49b69c1u, 0xefbe4786u, 0x0fc19dc6u, 0x240ca1ccu, 0x2de92c6fu,
    0x4a7484aau, 0x5cb0a9dcu, 0x76f988dau, 0x983e5152u, 0xa831c66du, 0xb00327c8u, 0xbf597fc7u,
    0xc6e00bf3u, 0xd5a79147u, 0x06ca6351u, 0x14292967u, 0x27b70a85u, 0x2e1b2138u, 0x4d2c6dfcu,
    0x53380d13u, 0x650a7354u, 0x766a0abbu, 0x81c2c92eu, 0x92722c85u, 0xa2bfe8a1u, 0xa81a664bu,
    0xc24b8b70u, 0xc76c51a3u, 0xd192e819u, 0xd6990624u, 0xf40e3585u, 0x106aa070u, 0x19a4c116u,
    0x1e376c08u, 0x2748774cu, 0x34b0bcb5u, 0x391c0cb3u, 0x4ed8aa4au, 0x5b9cca4fu, 0x682e6ff3u,
    0x748f82eeu, 0x78a5636fu, 0x84c87814u, 0x8cc70208u, 0x90befffau, 0xa4506cebu, 0xbef9a3f7u,
    0xc67178f2u};

// 32 位循环右移 n 位。
inline uint32_t rotr(uint32_t x, int n) { return (x >> n) | (x << (32 - n)); }

// 处理一个 64 字节块并更新哈希状态。
void sha256_block(Sha256Ctx& c, const uint8_t* p) {
    uint32_t w[64];
    for (int i = 0; i < 16; ++i) {
        w[i] = (static_cast<uint32_t>(p[i * 4]) << 24) |
               (static_cast<uint32_t>(p[i * 4 + 1]) << 16) |
               (static_cast<uint32_t>(p[i * 4 + 2]) << 8) |
               static_cast<uint32_t>(p[i * 4 + 3]);
    }
    for (int i = 16; i < 64; ++i) {
        const uint32_t s0 = rotr(w[i - 15], 7) ^ rotr(w[i - 15], 18) ^ (w[i - 15] >> 3);
        const uint32_t s1 = rotr(w[i - 2], 17) ^ rotr(w[i - 2], 19) ^ (w[i - 2] >> 10);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }
    uint32_t a = c.h[0], b = c.h[1], cc = c.h[2], d = c.h[3];
    uint32_t e = c.h[4], f = c.h[5], g = c.h[6], hh = c.h[7];
    for (int i = 0; i < 64; ++i) {
        const uint32_t S1 = rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25);
        const uint32_t ch = (e & f) ^ ((~e) & g);
        const uint32_t t1 = hh + S1 + ch + kK[i] + w[i];
        const uint32_t S0 = rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22);
        const uint32_t maj = (a & b) ^ (a & cc) ^ (b & cc);
        const uint32_t t2 = S0 + maj;
        hh = g; g = f; f = e; e = d + t1;
        d = cc; cc = b; b = a; a = t1 + t2;
    }
    c.h[0] += a; c.h[1] += b; c.h[2] += cc; c.h[3] += d;
    c.h[4] += e; c.h[5] += f; c.h[6] += g; c.h[7] += hh;
}

// 增量喂入数据，按 64 字节自动分块。
void sha256_update(Sha256Ctx& c, const uint8_t* data, size_t len) {
    c.total += static_cast<uint64_t>(len) * 8;
    while (len > 0) {
        const size_t n = (64 - c.buf_len < len) ? (64 - c.buf_len) : len;
        std::memcpy(c.buf + c.buf_len, data, n);
        c.buf_len += n;
        data += n;
        len -= n;
        if (c.buf_len == 64) {
            sha256_block(c, c.buf);
            c.buf_len = 0;
        }
    }
}

// 追加标准填充（0x80 + 0x00* + 64bit 大端长度）并输出 64 字符小写十六进制摘要。
std::string sha256_final_hex(Sha256Ctx& c) {
    uint8_t pad[72] = {0x80};
    const size_t rem = c.buf_len;
    const size_t pad_len = (rem < 56) ? (56 - rem) : (120 - rem);
    // 追加 0x80 + 0x00* + 8 字节大端长度
    uint8_t tail[128];
    std::memset(tail, 0, sizeof(tail));
    tail[0] = 0x80;
    const uint64_t bits = c.total;
    for (int i = 0; i < 8; ++i) {
        tail[pad_len + i] = static_cast<uint8_t>((bits >> (56 - 8 * i)) & 0xFF);
    }
    const uint64_t saved = c.total;
    sha256_update(c, tail, pad_len + 8);
    c.total = saved;  // 填充不应计入长度（update 已加过，这里仅保持字段一致）

    char out[65];
    for (int i = 0; i < 8; ++i) {
        std::snprintf(out + i * 8, 9, "%08x", c.h[i]);
    }
    return std::string(out, 64);
}

// Ed25519 SubjectPublicKeyInfo 的固定 DER 头（14 字节内容 + 长度字节，共 12 字节）
const uint8_t kSpkiPrefix[12] = {0x30, 0x2a, 0x30, 0x05, 0x06, 0x03, 0x2b, 0x65, 0x70, 0x03, 0x21, 0x00};

}  // namespace

// 计算内存缓冲区 (data,len) 的 SHA-256 十六进制摘要。
std::string sha256_hex(const uint8_t* data, size_t len) {
    Sha256Ctx c;
    sha256_update(c, data, len);
    return sha256_final_hex(c);
}

// 计算字符串内容的 SHA-256 十六进制摘要。
std::string sha256_hex(const std::string& s) {
    return sha256_hex(reinterpret_cast<const uint8_t*>(s.data()), s.size());
}

// 分块读取整文件计算 SHA-256 十六进制摘要；打不开返回 false。
bool sha256_file_hex(const std::string& path, std::string* out_hex) {
    std::ifstream f(path, std::ios::binary);
    if (!f.good()) return false;
    Sha256Ctx c;
    char buf[1024 * 1024];
    while (f.read(buf, sizeof(buf)) || f.gcount() > 0) {
        sha256_update(c, reinterpret_cast<const uint8_t*>(buf), static_cast<size_t>(f.gcount()));
        if (f.eof()) break;
    }
    const std::string hex = sha256_final_hex(c);
    if (out_hex != nullptr) *out_hex = hex;
    return true;
}

// 从 PEM 文本提取 raw 32 字节 Ed25519 公钥；头/长度/SPKI 前缀任何不符即 false（fail-closed）。
bool load_ed25519_pem(const std::string& pem, uint8_t out[32]) {
    const std::string begin = "-----BEGIN PUBLIC KEY-----";
    const std::string end = "-----END PUBLIC KEY-----";
    const size_t b = pem.find(begin);
    if (b == std::string::npos) return false;
    const size_t e = pem.find(end, b + begin.size());
    if (e == std::string::npos) return false;
    std::string b64 = pem.substr(b + begin.size(), e - (b + begin.size()));
    // 去掉所有空白（PEM 可能有换行）
    std::string clean;
    for (char ch : b64) {
        if (ch != '\n' && ch != '\r' && ch != ' ' && ch != '\t') clean.push_back(ch);
    }
    std::vector<uint8_t> der;
    if (!base64_decode(clean, &der)) return false;
    // Ed25519 SPKI 固定 44 字节：12 头 + 32 key
    if (der.size() != 44) return false;
    if (std::memcmp(der.data(), kSpkiPrefix, sizeof(kSpkiPrefix)) != 0) return false;
    std::memcpy(out, der.data() + 12, 32);
    return true;
}

// 用给定公钥验证 msg 字节串上的 64 字节 Ed25519 签名。
bool verify_ed25519(const uint8_t sig[64], const std::string& msg, const uint8_t pk[32]) {
    if (sig == nullptr || pk == nullptr) return false;
    return ttbox::core::auth::ed25519::verify(
        sig, reinterpret_cast<const uint8_t*>(msg.data()), msg.size(), pk);
}

// base64 解码（复用 auth 的 license_base64_decode）。
bool base64_decode(const std::string& in, std::vector<uint8_t>* out) {
    return ttbox::core::auth::license_base64_decode(in, out);
}

}  // namespace ttbox::core::ota
