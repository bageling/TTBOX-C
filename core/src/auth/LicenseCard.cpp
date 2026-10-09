// LicenseCard.cpp — M2.01：离线卡解析/归一化/canonical（纯逻辑，无 OpenSSL/网络/IO）
#include "auth/LicenseCard.hpp"

#include <algorithm>

#include "common/Json.hpp"
#include "auth/LicenseStateMachine.hpp"  // known_features / normalize_features / sanitize_ui_brand

namespace ttbox::core::auth {

namespace {

// 受限标识符字符集：[A-Za-z0-9_-]
// 判断单字符是否属受限标识符集 [A-Za-z0-9_-]。
bool is_id_char(unsigned char c) {
    return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') ||
           (c >= 'A' && c <= 'Z') || c == '_' || c == '-';
}

// 标识符校验：非空、长度 ≤ max_len、且全部字符属 [A-Za-z0-9_-]。
bool is_valid_id(const std::string& s, size_t max_len) {
    if (s.empty() || s.size() > max_len) return false;
    for (char ch : s) {
        if (!is_id_char(static_cast<unsigned char>(ch))) return false;
    }
    return true;
}

// 从 JSON 数值取非负 int64（拒绝负数与超出 unix 秒域的值）。
bool parse_int64(const JsonValue& v, int64_t* out) {
    if (!v.is_number()) return false;
    const double d = v.as_number();
    if (d < 0 || d > 9.0e15) return false;  // unix 秒域内
    *out = v.as_int();
    return true;
}

}  // namespace

// plan 闭集判定：仅 none|trial|subscription|permanent 合法。
bool is_valid_plan(const std::string& plan) {
    return plan == "none" || plan == "trial" || plan == "subscription" ||
           plan == "permanent";
}

// 标准 base64 解码（支持 '=' 尾部填充）；长度/字符非法即返回 false。
bool license_base64_decode(const std::string& in, std::vector<uint8_t>* out) {
    out->clear();
    if (in.empty() || in.size() % 4 != 0) return false;
    static const char* kTable =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    auto val = [&](char c) -> int {
        for (int i = 0; kTable[i]; ++i) {
            if (kTable[i] == c) return i;
        }
        return -1;
    };
    for (size_t i = 0; i < in.size(); i += 4) {
        const int a = val(in[i]), b = val(in[i + 1]);
        if (a < 0 || b < 0) return false;
        const char c3 = in[i + 2], c4 = in[i + 3];
        if (c4 == '=' && c3 != '=' && i + 4 == in.size()) {
            const int c = val(c3);
            if (c < 0) return false;
            out->push_back(static_cast<uint8_t>((a << 2) | (b >> 4)));
            out->push_back(static_cast<uint8_t>(((b & 0xF) << 4) | (c >> 2)));
            return true;
        }
        if (c3 == '=' && c4 == '=' && i + 4 == in.size()) {
            out->push_back(static_cast<uint8_t>((a << 2) | (b >> 4)));
            return true;
        }
        const int c = val(c3), d = val(c4);
        if (c < 0 || d < 0) return false;
        out->push_back(static_cast<uint8_t>((a << 2) | (b >> 4)));
        out->push_back(static_cast<uint8_t>(((b & 0xF) << 4) | (c >> 2)));
        out->push_back(static_cast<uint8_t>(((c & 0x3) << 6) | d));
    }
    return true;
}

// 按字段名字母序拼出 Ed25519 签名原文（每字段 "key\nvalue\n"，见头注释）。
std::string license_canonical(const LicenseCard& c) {
    std::string s;
    s.reserve(128 + c.license_id.size() + c.device.size() + c.ui_brand.size());
    s += "device\n";        s += c.device;      s += "\n";
    s += "expires_at\n";   s += std::to_string(c.expires_at); s += "\n";
    s += "features\n";
    for (size_t i = 0; i < c.features.size(); ++i) {
        if (i) s += ",";
        s += c.features[i];
    }
    s += "\n";
    s += "is_pro\n";        s += c.is_pro ? "1" : "0"; s += "\n";
    s += "issued_at\n";     s += std::to_string(c.issued_at); s += "\n";
    s += "license_id\n";    s += c.license_id;   s += "\n";
    s += "plan\n";          s += c.plan;         s += "\n";
    s += "ui_brand\n";      s += c.ui_brand;     s += "\n";
    return s;
}

// 解析卡信封 JSON 并归一化各字段；任一字段违例即整卡拒绝（fail-closed，非空 error）。
bool parse_license_card(const std::string& envelope_json, LicenseCard* out,
                        std::string* error) {
    auto fail = [&](const std::string& why) {
        if (error) *error = why;
        return false;
    };
    if (out == nullptr) return fail("internal: out == nullptr");

    const JsonParseResult parsed = json_parse(envelope_json);
    if (!parsed.ok || !parsed.value.is_object()) {
        return fail("license card: not a JSON object");
    }

    // ---- 信封层 ----
    const JsonValue* key_id_v = parsed.value.find("key_id");
    const JsonValue* sig_v = parsed.value.find("signature");
    if (key_id_v == nullptr || !key_id_v->is_string()) {
        return fail("license card: missing key_id");
    }
    if (sig_v == nullptr || !sig_v->is_string()) {
        return fail("license card: missing signature");
    }
    std::vector<uint8_t> sig_raw;
    if (!license_base64_decode(sig_v->as_string(), &sig_raw) ||
        sig_raw.size() != 64) {
        return fail("license card: signature must be base64 of 64 bytes");
    }

    const JsonValue* lic_v = parsed.value.find("license");
    if (lic_v == nullptr || !lic_v->is_object()) {
        return fail("license card: missing 'license' object");
    }
    const JsonValue& lic = *lic_v;

    // ---- license 字段（逐一 fail-closed）----
    LicenseCard c;
    c.key_id = key_id_v->as_string();
    c.signature_b64 = sig_v->as_string();

    const JsonValue* v = nullptr;

    v = lic.find("license_id");
    if (v == nullptr || !v->is_string()) return fail("license card: license_id missing/not string");
    c.license_id = v->as_string();
    if (!is_valid_id(c.license_id, 64)) return fail("license card: license_id charset/len");

    v = lic.find("device");
    if (v == nullptr || !v->is_string()) return fail("license card: device missing/not string");
    c.device = v->as_string();
    if (!is_valid_id(c.device, 64)) return fail("license card: device charset/len");

    v = lic.find("plan");
    if (v == nullptr || !v->is_string()) return fail("license card: plan missing/not string");
    c.plan = v->as_string();
    if (!is_valid_plan(c.plan)) return fail("license card: plan not in {none,trial,subscription,permanent}");

    v = lic.find("is_pro");
    if (v == nullptr || !v->is_bool()) return fail("license card: is_pro missing/not bool");
    c.is_pro = v->as_bool();

    v = lic.find("features");
    if (v == nullptr || !v->is_array()) return fail("license card: features missing/not array");
    std::vector<std::string> raw_features;
    for (const JsonValue& f : v->as_array()) {
        if (!f.is_string()) return fail("license card: feature entry not string");
        raw_features.push_back(f.as_string());
    }
    // 归一化：闭集过滤 + 去重（保留首次出现），再按 known_features() 闭集序重排
    //（canonical 序 = 闭集序 ⇒ 集合相等即串相等，签名与卡内数组顺序无关）。
    {
        const std::vector<std::string> norm = normalize_features(raw_features);
        for (const std::string& k : known_features()) {
            if (std::find(norm.begin(), norm.end(), k) != norm.end()) {
                c.features.push_back(k);
            }
        }
    }

    v = lic.find("ui_brand");
    if (v != nullptr && v->is_string()) {
        // sanitize：非法/超长 ⇒ 回落默认品牌（LicenseStateMachine 唯一拦截点）。
        c.ui_brand = sanitize_ui_brand(v->as_string());
    } else {
        c.ui_brand = default_ui_brand();
    }

    v = lic.find("issued_at");
    if (v == nullptr || !parse_int64(*v, &c.issued_at)) {
        return fail("license card: issued_at missing/not uint");
    }
    v = lic.find("expires_at");
    if (v == nullptr || !parse_int64(*v, &c.expires_at)) {
        return fail("license card: expires_at missing/not uint");
    }

    *out = c;
    return true;
}

}  // namespace ttbox::core::auth
