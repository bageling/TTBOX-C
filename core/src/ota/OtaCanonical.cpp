// OtaCanonical.cpp — 见 OtaCanonical.hpp 的文件头说明。
#include "ota/OtaCanonical.hpp"

#include <cmath>
#include <cstdio>
#include <cstdlib>

namespace ttbox::core::ota {

namespace {

// JSON 字符串转义（等价 Python json.dumps(..., ensure_ascii=False) 的转义表）。
// ★ 非 ASCII **不转义**（原样输出 UTF-8 字节）—— 这是 ensure_ascii=False 的语义，
//   也是对拍样本里"中文字符"那条要验证的行为。
void append_escaped(std::string& out, const std::string& s) {
    out.push_back('"');
    for (unsigned char c : s) {
        switch (c) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\b': out += "\\b"; break;
            case '\f': out += "\\f"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (c < 0x20) {
                    char buf[8];
                    std::snprintf(buf, sizeof(buf), "\\u%04x", c);
                    out += buf;
                } else {
                    out.push_back(static_cast<char>(c));  // 含 >=0x80：原样
                }
                break;
        }
    }
    out.push_back('"');
}

// Python repr(float) 是「最短往返表示」⇒ 这里用逐步增加有效位直到 strtod 能往返，
// 复刻同一语义。为什么不能直接用 std::to_string：它固定 6 位小数（1.5 → "1.500000"）。
// ★ 整数特判：Python 的 int 输出无小数点（如 built_at 1791286707）；
//   而项目 JsonValue 把数字统一存成 double，无法区分 int/float
//   ⇒ 凡"值等于整数的 double"一律按整数输出。签名数据里只有 built_at 一个数字且必为整数，
//     故该近似不影响验签（详见对拍样本 E_meta 的说明）。
std::string format_number(double d) {
    if (!std::isfinite(d)) return "null";  // JSON 无 NaN/Inf
    if (d == std::floor(d) && std::fabs(d) < 1e15) {
        char buf[32];
        std::snprintf(buf, sizeof(buf), "%lld", static_cast<long long>(d));
        return std::string(buf);
    }
    char buf[48];
    for (int prec = 1; prec <= 17; ++prec) {
        std::snprintf(buf, sizeof(buf), "%.*g", prec, d);
        if (std::strtod(buf, nullptr) == d) break;  // 已能往返 ⇒ 这就是最短表示
    }
    return std::string(buf);
}

void dump_into(std::string& out, const JsonValue& v) {
    if (v.is_null()) {
        out += "null";
    } else if (v.is_bool()) {
        out += v.as_bool(false) ? "true" : "false";
    } else if (v.is_number()) {
        out += format_number(v.as_number(0.0));
    } else if (v.is_string()) {
        append_escaped(out, v.as_string(""));
    } else if (v.is_array()) {
        out.push_back('[');
        bool first = true;
        for (const JsonValue& item : v.as_array()) {
            if (!first) out.push_back(',');
            first = false;
            dump_into(out, item);
        }
        out.push_back(']');
    } else if (v.is_object()) {
        // as_object() 是 std::map<std::string, JsonValue> ⇒ 已按**字节序**升序。
        // UTF-8 的字节序与 Unicode 码点序一致 ⇒ 等价于 Python 的 sort_keys（按码点）。
        out.push_back('{');
        bool first = true;
        for (const auto& kv : v.as_object()) {
            if (!first) out.push_back(',');
            first = false;
            append_escaped(out, kv.first);
            out.push_back(':');
            dump_into(out, kv.second);
        }
        out.push_back('}');
    }
}

}  // namespace

std::string canonical_dump(const JsonValue& v) {
    std::string out;
    dump_into(out, v);
    return out;
}

std::string canonical_signed_fields(const JsonValue& rec,
                                    const std::vector<std::string>& signed_fields) {
    JsonValue sub = JsonValue::object();
    for (const std::string& k : signed_fields) {
        const JsonValue* p = rec.find(k);
        if (p != nullptr) sub.set(k, *p);
    }
    return canonical_dump(sub);
}

}  // namespace ttbox::core::ota
