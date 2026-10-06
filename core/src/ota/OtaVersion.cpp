// OtaVersion.cpp — 见 OtaVersion.hpp 的文件头说明。
#include "ota/OtaVersion.hpp"

#include <cctype>

namespace ttbox::core::ota {

namespace {

bool is_sep(char c) { return c == '.' || c == '-' || c == '_' || c == '+'; }

// Python str.isdigit()（ASCII 子集；版本号段必为 ASCII）
bool is_all_digits(const std::string& s) {
    if (s.empty()) return false;
    for (char c : s) {
        if (c < '0' || c > '9') return false;
    }
    return true;
}

// Python str.strip()（ASCII 空白子集）—— 版本号/标识符都是 ASCII。
std::string strip_ascii(const std::string& s) {
    size_t b = 0, e = s.size();
    while (b < e && std::isspace(static_cast<unsigned char>(s[b]))) b++;
    while (e > b && std::isspace(static_cast<unsigned char>(s[e - 1]))) e--;
    return s.substr(b, e - b);
}

}  // namespace

std::vector<VersionSeg> version_key(const std::string& v) {
    std::vector<VersionSeg> parts;
    std::string seg;
    // 末尾补一个分隔符，让循环统一处理最后一段（等价 Python re.split 的收尾）
    const std::string src = v + ".";
    for (size_t i = 0; i < src.size(); ++i) {
        const char c = src[i];
        const bool last_pad = (i + 1 == src.size());
        if (!last_pad && !is_sep(c)) {
            seg.push_back(c);
            continue;
        }
        if (!seg.empty()) {
            VersionSeg s;
            if (is_all_digits(seg)) {
                s.cls = 0;
                // Python int() 无上限；版本段实际很小。用 long long 并饱和处理，
                // 避免溢出成负数导致比较反向（极端输入下宁可"偏大"）。
                long long n = 0;
                bool overflow = false;
                for (char d : seg) {
                    const int digit = d - '0';
                    if (n > (9223372036854775807LL - digit) / 10) {
                        overflow = true;
                        break;
                    }
                    n = n * 10 + digit;
                }
                s.num = overflow ? 9223372036854775807LL : n;
                s.text.clear();
            } else {
                s.cls = 1;
                s.num = 0;
                s.text = seg;
            }
            parts.push_back(std::move(s));
            seg.clear();
        }
    }
    return parts;
}

int compare_version_key(const std::vector<VersionSeg>& a, const std::vector<VersionSeg>& b) {
    const size_t n = a.size() < b.size() ? a.size() : b.size();
    for (size_t i = 0; i < n; ++i) {
        if (a[i].cls != b[i].cls) return a[i].cls < b[i].cls ? -1 : 1;
        if (a[i].cls == 0) {
            if (a[i].num != b[i].num) return a[i].num < b[i].num ? -1 : 1;
        } else {
            const int c = a[i].text.compare(b[i].text);
            if (c != 0) return c < 0 ? -1 : 1;
        }
    }
    // 前缀相同 ⇒ 短的更小（Python tuple 比较语义）
    if (a.size() == b.size()) return 0;
    return a.size() < b.size() ? -1 : 1;
}

bool is_downgrade(const std::string& new_ver, const std::string& cur_ver) {
    // Python: if not cur_ver: return False（版本未知 ⇒ 不拦）
    if (strip_ascii(cur_ver).empty()) return false;
    return compare_version_key(version_key(new_ver), version_key(cur_ver)) <= 0;
}

bool check_safe_id(const std::string& value, std::string* out) {
    const std::string v = strip_ascii(value);
    if (v.empty()) return false;
    // ^[A-Za-z0-9][A-Za-z0-9._-]*$
    const char first = v[0];
    const bool first_ok = (first >= 'A' && first <= 'Z') || (first >= 'a' && first <= 'z') ||
                          (first >= '0' && first <= '9');
    if (!first_ok) return false;
    for (size_t i = 1; i < v.size(); ++i) {
        const char c = v[i];
        const bool ok = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
                        (c >= '0' && c <= '9') || c == '.' || c == '_' || c == '-';
        if (!ok) return false;
    }
    // 拒 ".."（防止拼进 releases/<ver>/ 时越界）
    if (v.find("..") != std::string::npos) return false;
    if (out != nullptr) *out = v;
    return true;
}

}  // namespace ttbox::core::ota
