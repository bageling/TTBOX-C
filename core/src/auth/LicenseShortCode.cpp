// LicenseShortCode.cpp — M2.05：短码 + 校验位（纯逻辑实现；见头文件的设计与红线说明）
//
// 算法（跨语言逐字节一致；Python 侧镜像见 tools/license/ttbox_license_gen.py）：
//   ① FNV-1a-32：h = 0x811C9DC5；逐字节 h ^= b; h *= 0x01000193（32-bit 回绕）。
//   ② 7 个 Crockford 字符：32 位按 5 位一组、从高位到低位取；
//      7×5 = 35 > 32 ⇒ 最高字符只用到 bit31..30（取值域 0..3）。
//   ③ Luhn mod 32 校验字符（alphabet 下标参与；标准 Luhn-mod-N）：
//        从右往左，权重 factor 交替 2,1,2,1,…（最右数据位权重 2）；
//        addend = factor*idx；addend = (addend / 32) + (addend % 32)（对 N 归约）；
//        sum 累加；check = (32 - sum % 32) % 32。
//      · 可检测**全部**单字符错误（0/248 漏检，实测）。
//      · 相邻换位：仅 {值 0, 值 31} 这一对（字母 '0'↔'Z'）漏检——Luhn mod N 的
//        已知边界（N 为偶数时差 N-1 的对），单测显式登记，不假装全覆盖。
//        （两字符相同者交换 ⇒ 串不变，无"错误"可言。）
//
// 归一（人手抄写容错）：大小写不敏感；O→0，I→1，L→1；分隔符 '-'/空格 与前缀 'TTB' 不参与计算。
// ★ 非安全边界：只查校验位，不查签名。任何准入路径不得以本模块为判据。
#include "auth/LicenseShortCode.hpp"

#include <cstddef>

namespace ttbox::core::auth {

namespace {

// FNV-1a-32 常量（写死，勿改；两侧同步锚）。
constexpr uint32_t kFnvOffsetBasis = 0x811C9DC5u;
constexpr uint32_t kFnvPrime = 0x01000193u;

constexpr std::size_t kBodyDataLen = 7;   // 7 位数据（不含校验位）
constexpr int kRadix = 32;                // kShortCodeRadix（本地别名，避免每次取常量）

// 字符 → alphabet 下标；非 alphabet 字符返回 -1。
// 查单字符在短码字母表中的下标；非表内字符返回 -1。
int alphabet_index(char c) {
    const char* a = kShortCodeAlphabet;
    for (int i = 0; i < kRadix; ++i) {
        if (a[i] == c) return i;
    }
    return -1;
}

// Crockford 归一的单字符形式：大写化 + O→0 / I→1 / L→1。
// 注意：'U' 属闭集外（Crockford 排除了 U 以防脏话与误读）⇒ 保持 'U'，随后被 alphabet_index 判非法。
// Crockford 单字符归一：转大写 + O→0 / I、L→1（其余原样）。
char normalize_char(char c) {
    if (c >= 'a' && c <= 'z') c = static_cast<char>(c - 'a' + 'A');
    if (c == 'O') return '0';
    if (c == 'I' || c == 'L') return '1';
    return c;
}

}  // namespace

// FNV-1a-32 摘要：逐字节 XOR 后乘素数并 32 位回绕。
uint32_t short_code_fnv1a32(const std::string& bytes) {
    uint32_t h = kFnvOffsetBasis;
    for (std::size_t i = 0; i < bytes.size(); ++i) {
        // 显式转 unsigned char：避免 char 为有符号时把高位字节当负数符号扩展。
        h ^= static_cast<uint32_t>(static_cast<unsigned char>(bytes[i]));
        h *= kFnvPrime;  // uint32_t 乘法天然按 2^32 回绕
    }
    return h;
}

// 32 位摘要 → 7 个 Crockford 字符（按 5 位一组从高位到低位）。
std::string short_code_body7(uint32_t h) {
    std::string out;
    out.reserve(kBodyDataLen);
    // 从高位到低位：bit31..30、bit29..25、…、bit4..0（步长 5，起点 30）。
    for (int i = 0; i < static_cast<int>(kBodyDataLen); ++i) {
        const int shift = 30 - i * 5;
        out.push_back(kShortCodeAlphabet[(h >> shift) & 0x1Fu]);
    }
    return out;
}

// 由 7 位数据算 Luhn mod 32 校验字符；body 含表外字符返回 '\0'。
char short_code_luhn_check(const std::string& body) {
    int factor = 2;  // 最右数据位权重 2
    int total = 0;
    for (std::string::const_reverse_iterator it = body.rbegin(); it != body.rend(); ++it) {
        const int idx = alphabet_index(*it);
        if (idx < 0) return '\0';  // body 含闭集外字符 ⇒ 不可派生
        int addend = factor * idx;
        factor = (factor == 2) ? 1 : 2;  // 交替 2,1,2,1,…
        // 对 N 归约：高位进位 + 低位余数（标准 Luhn-mod-N 的 addend 折叠）。
        addend = (addend / kRadix) + (addend % kRadix);
        total += addend;
    }
    const int check = (kRadix - (total % kRadix)) % kRadix;
    return kShortCodeAlphabet[check];
}

// 由 license_id 派生人读短码 "TTB-XXXX-XXXX"；id 非法则返回空串。
std::string license_short_code_derive(const std::string& license_id) {
    if (!license_short_code_is_valid_id(license_id)) return std::string();  // fail-closed：不臆造
    const std::string body = short_code_body7(short_code_fnv1a32(license_id));
    const char check = short_code_luhn_check(body);
    if (check == '\0') return std::string();
    std::string out;
    out.reserve(13);
    out.append(kShortCodePrefix);        // "TTB"
    out.push_back('-');
    out.append(body.substr(0, 4));       // 4 位数据
    out.push_back('-');
    out.append(body.substr(4, 3));       // 3 位数据
    out.push_back(check);                // 1 位校验
    return out;                          // "TTB-XXXX-XXXX"（13 字符）
}

// 校验短码：归一化去分隔/前缀后跑 Luhn 校验位（仅防笔误，非安全判据）。
bool license_short_code_verify(const std::string& short_code) {
    // 归一：去分隔符（'-' 与空格），做 Crockford 字符归一（大写 + O/I/L 映射）。
    std::string norm;
    norm.reserve(short_code.size());
    for (std::size_t i = 0; i < short_code.size(); ++i) {
        const char c = short_code[i];
        if (c == '-' || c == ' ' || c == '\t' || c == '\r' || c == '\n') continue;
        norm.push_back(normalize_char(c));
    }
    // 去前缀 "TTB"（归一后比较；派生 body 首位恒 ∈ {'0','1','2','3'}
    // ⇒ body 绝不以 'T' 开头 ⇒ 无"误剥前缀"歧义）。
    if (norm.size() >= 3 && norm.compare(0, 3, kShortCodePrefix) == 0) {
        norm.erase(0, 3);
    }
    if (norm.size() != static_cast<std::size_t>(kShortCodeBodyLen)) return false;
    for (std::size_t i = 0; i < norm.size(); ++i) {
        if (alphabet_index(norm[i]) < 0) return false;
    }
    const char check = short_code_luhn_check(norm.substr(0, kBodyDataLen));
    return check != '\0' && check == norm[kBodyDataLen];
}

// license_id 合法性：[A-Za-z0-9_-]{1,64}（与 LicenseCard 契约一致）。
bool license_short_code_is_valid_id(const std::string& license_id) {
    if (license_id.empty() || license_id.size() > 64) return false;
    for (std::size_t i = 0; i < license_id.size(); ++i) {
        const char c = license_id[i];
        const bool ok = (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') ||
                        (c >= 'A' && c <= 'Z') || c == '_' || c == '-';
        if (!ok) return false;
    }
    return true;
}

}  // namespace ttbox::core::auth
