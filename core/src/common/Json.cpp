// Json.cpp — 极简 JSON 解析/序列化实现（递归下降解析器）
/*
 * TTBOX 文件说明
 *
 * 文件：Json.cpp
 *
 * 作用：
 *   TTBOX 自用的 JSON 解析和序列化库。
 *   不依赖第三方库，轻量级实现。
 *
 * 小白理解：
 *   JSON 是一种通用的数据格式。
 *   这个模块负责把 JSON 文本转换成 C++ 能用的数据，
 *   也负责把 C++ 数据转换成 JSON 文本。
 *
 * 注意：
 *   本注释仅用于说明代码，不改变程序逻辑。
 */

#include "Json.hpp"

#include <cctype>
#include <cerrno>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <stdexcept>

namespace ttbox::core {

// ---------------------------------------------------------------------------
// JsonValue 构建/访问
// ---------------------------------------------------------------------------

// 构造 null 值。
JsonValue JsonValue::null() {
    return JsonValue();
}

// 构造 bool 值。
JsonValue JsonValue::boolean(bool v) {
    JsonValue j;
    j.type_ = JsonType::kBool;
    j.bool_ = v;
    return j;
}

// 构造 number 值（统一以 double 存放）。
JsonValue JsonValue::number(double v) {
    JsonValue j;
    j.type_ = JsonType::kNumber;
    j.num_ = v;
    return j;
}

// 构造 string 值（移动入参）。
JsonValue JsonValue::string(std::string v) {
    JsonValue j;
    j.type_ = JsonType::kString;
    j.str_ = std::move(v);
    return j;
}

// 构造空数组。
JsonValue JsonValue::array() {
    JsonValue j;
    j.type_ = JsonType::kArray;
    return j;
}

// 由已有向量构造数组。
JsonValue JsonValue::array(std::vector<JsonValue> v) {
    JsonValue j;
    j.type_ = JsonType::kArray;
    j.arr_ = std::move(v);
    return j;
}

// 构造空对象。
JsonValue JsonValue::object() {
    JsonValue j;
    j.type_ = JsonType::kObject;
    return j;
}

// 取 bool；类型不符回退 def。
bool JsonValue::as_bool(bool def) const {
    return type_ == JsonType::kBool ? bool_ : def;
}

// 取 number；类型不符回退 def。
double JsonValue::as_number(double def) const {
    return type_ == JsonType::kNumber ? num_ : def;
}

// 取 int64；非有限/越界的浮点按 def 处理（避免 cast 的 UB）。
int64_t JsonValue::as_int(int64_t def) const {
    if (type_ != JsonType::kNumber) return def;
    if (std::isnan(num_) || std::isinf(num_)) return def;
    // 越界的「有限」浮点转 int64_t 是标准 UB。GCC/Clang 实测会饱和成 INT64_MIN，
    // 但那是实现行为、换编译器或优化档就可能变 —— 不可依赖，所以必须在 cast 前挡住。
    // 触发面是真实存在的：web 请求体 / IPC 传入 1e300 这类值即可走到这里。
    // 边界用 2^63 的精确 double 表示（2 的幂，double 可精确表示）：
    //   下界 -2^63 合法（含），上界 2^63 不合法（int64_t 上界是开区间）。
    // 注：double 无法区分 (2^63 - 1) 与 2^63，故 2^63-1 也会被判越界 —— 这是有意的保守。
    constexpr double kInt64Min = -9223372036854775808.0;
    constexpr double kInt64MaxExclusive = 9223372036854775808.0;
    if (num_ < kInt64Min || num_ >= kInt64MaxExclusive) return def;
    return static_cast<int64_t>(num_);
}

// 取 string；类型不符回退 def。
std::string JsonValue::as_string(const std::string& def) const {
    return type_ == JsonType::kString ? str_ : def;
}

// 取数组引用（非数组时返回空向量引用）。
const std::vector<JsonValue>& JsonValue::as_array() const {
    return arr_;
}

// 取对象引用（非对象时返回空 map 引用）。
const std::map<std::string, JsonValue>& JsonValue::as_object() const {
    return obj_;
}

// 在对象中查键；不存在（或非对象）返回 nullptr。
const JsonValue* JsonValue::find(const std::string& key) const {
    if (type_ != JsonType::kObject) return nullptr;
    auto it = obj_.find(key);
    return it == obj_.end() ? nullptr : &it->second;
}

// 仅对 object 生效：写入/覆盖成员。
void JsonValue::set(const std::string& key, JsonValue v) {
    if (type_ != JsonType::kObject) return;
    obj_[key] = std::move(v);
}

// 仅对 array 生效：尾部追加元素。
void JsonValue::push_back(JsonValue v) {
    if (type_ != JsonType::kArray) return;
    arr_.push_back(std::move(v));
}

// 深比较：类型不同即不等；同类型逐成员比较。
bool JsonValue::operator==(const JsonValue& o) const {
    if (type_ != o.type_) return false;
    switch (type_) {
        case JsonType::kNull: return true;
        case JsonType::kBool: return bool_ == o.bool_;
        case JsonType::kNumber: return num_ == o.num_;
        case JsonType::kString: return str_ == o.str_;
        case JsonType::kArray: return arr_ == o.arr_;
        case JsonType::kObject: return obj_ == o.obj_;
    }
    return false;
}

// ---------------------------------------------------------------------------
// 序列化（dump）
// ---------------------------------------------------------------------------

namespace {

// 转义并写出一个 JSON 字符串字面量（含控制字符与引号/反斜杠）。
void dump_string(std::string& out, const std::string& s) {
    out.push_back('"');
    for (char c : s) {
        switch (c) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\b': out += "\\b"; break;
            case '\f': out += "\\f"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (static_cast<unsigned char>(c) < 0x20) {
                    char buf[8];
                    std::snprintf(buf, sizeof(buf), "\\u%04x", static_cast<unsigned>(c));
                    out += buf;
                } else {
                    out.push_back(c);
                }
        }
    }
    out.push_back('"');
}

// 递归序列化任意 JsonValue（NaN/Inf 归一为 null）。
void dump_value(std::string& out, const JsonValue& v) {
    switch (v.type()) {
        case JsonType::kNull: out += "null"; break;
        case JsonType::kBool: out += v.as_bool() ? "true" : "false"; break;
        case JsonType::kNumber: {
            double d = v.as_number();
            if (std::isnan(d) || std::isinf(d)) {
                out += "null";
            } else if (std::fabs(d) <= 9007199254740991.0 &&
                       d == static_cast<int64_t>(d)) {
                out += std::to_string(static_cast<int64_t>(d));
            } else {
                char buf[32];
                std::snprintf(buf, sizeof(buf), "%.9g", d);
                out += buf;
            }
            break;
        }
        case JsonType::kString: dump_string(out, v.as_string()); break;
        case JsonType::kArray: {
            out.push_back('[');
            bool first = true;
            for (const auto& e : v.as_array()) {
                if (!first) out.push_back(',');
                first = false;
                dump_value(out, e);
            }
            out.push_back(']');
            break;
        }
        case JsonType::kObject: {
            out.push_back('{');
            bool first = true;
            for (const auto& [k, val] : v.as_object()) {
                if (!first) out.push_back(',');
                first = false;
                dump_string(out, k);
                out.push_back(':');
                dump_value(out, val);
            }
            out.push_back('}');
            break;
        }
    }
}

}  // namespace

// 紧凑序列化入口（无多余空白）。
std::string JsonValue::dump() const {
    std::string out;
    dump_value(out, *this);
    return out;
}

// ---------------------------------------------------------------------------
// 解析器
// ---------------------------------------------------------------------------

namespace {

class Parser {
public:
    explicit Parser(const std::string& text) : text_(text) {}

    // 解析整段文本：成功后必须只剩空白，否则报尾部有多余内容。
    JsonParseResult parse() {
        JsonParseResult result;
        skip_ws();
        JsonValue v;
        if (!parse_value(v)) {
            result.ok = false;
            result.error = "JSON 语法错误 @ 位置 " + std::to_string(pos_) + ": " + err_;
            return result;
        }
        skip_ws();
        if (pos_ != text_.size()) {
            result.ok = false;
            result.error = "JSON 尾部存在多余内容 @ 位置 " + std::to_string(pos_);
            return result;
        }
        result.ok = true;
        result.value = std::move(v);
        return result;
    }

private:
    // 递归深度护栏（2026-09-23 复核发现）：parse_value ↔ parse_object/parse_array 是纯递归
    // 下降，此前没有任何深度计数 ⇒ 连续嵌套的 '[' 会按输入长度线性吃栈。IPC 单行实际可到
    // 约 69.6KB（read_line 的上限判断排在 find('\n') 之后）⇒ 约 6.9 万层，远超默认线程栈，
    // 本机同组进程发一次请求即可打崩 Core。256 层对真实请求（配置 / 状态 / 模型参数）足够宽裕。
    // 注：dump_value 侧同样无上限，但它的输入只能来自已解析的结构（这里已挡）；若将来出现
    // "把对端 JSON 原样回吐"的路径，需给 dump 侧一并补护栏。
    static constexpr int kMaxDepth = 256;
    int depth_ = 0;

    // RAII：进容器自增、离开自减 —— 覆盖所有 return / fail 路径，不靠人工配对。
    struct DepthScope {
        Parser* self;
        explicit DepthScope(Parser* p) : self(p) { ++self->depth_; }
        ~DepthScope() { --self->depth_; }
    };

    const std::string& text_;
    size_t pos_ = 0;
    std::string err_;

    // 跳过空白字符。
    void skip_ws() {
        while (pos_ < text_.size() &&
               (text_[pos_] == ' ' || text_[pos_] == '\t' ||
                text_[pos_] == '\n' || text_[pos_] == '\r')) {
            ++pos_;
        }
    }

    // 记录首个错误信息并返回 false（后续错误不覆盖首条）。
    bool fail(const std::string& msg) {
        if (err_.empty()) err_ = msg;
        return false;
    }

    // 按首字符分发到对应子解析器。
    bool parse_value(JsonValue& out) {
        if (pos_ >= text_.size()) return fail("意外的文件结尾");
        char c = text_[pos_];
        switch (c) {
            case '{': return parse_object(out);
            case '[': return parse_array(out);
            case '"': return parse_string(out);
            case 't':
            case 'f': return parse_bool(out);
            case 'n': return parse_null(out);
            default:
                if (c == '-' || (c >= '0' && c <= '9')) return parse_number(out);
                return fail(std::string("无法识别的字符 '") + c + "'");
        }
    }

    // 解析对象（带递归深度护栏）。
    bool parse_object(JsonValue& out) {
        if (depth_ >= kMaxDepth) {
            return fail("JSON 嵌套过深（超过 " + std::to_string(kMaxDepth) + " 层）");
        }
        DepthScope scope(this);
        ++pos_;  // {
        out = JsonValue::object();
        skip_ws();
        if (pos_ < text_.size() && text_[pos_] == '}') {
            ++pos_;
            return true;
        }
        while (true) {
            skip_ws();
            if (pos_ >= text_.size() || text_[pos_] != '"') {
                return fail("对象成员名必须是字符串");
            }
            JsonValue key_v;
            if (!parse_string(key_v)) return false;
            std::string key = key_v.as_string();
            skip_ws();
            if (pos_ >= text_.size() || text_[pos_] != ':') {
                return fail("对象成员名后缺少 ':'");
            }
            ++pos_;
            skip_ws();
            JsonValue val;
            if (!parse_value(val)) return false;
            out.set(key, std::move(val));
            skip_ws();
            if (pos_ >= text_.size()) return fail("对象未闭合");
            char c = text_[pos_];
            if (c == ',') {
                ++pos_;
                continue;
            }
            if (c == '}') {
                ++pos_;
                return true;
            }
            return fail("对象成员之间缺少 ',' 或 '}'");
        }
    }

    // 解析数组（带递归深度护栏）。
    bool parse_array(JsonValue& out) {
        if (depth_ >= kMaxDepth) {
            return fail("JSON 嵌套过深（超过 " + std::to_string(kMaxDepth) + " 层）");
        }
        DepthScope scope(this);
        ++pos_;  // [
        out = JsonValue::array();
        skip_ws();
        if (pos_ < text_.size() && text_[pos_] == ']') {
            ++pos_;
            return true;
        }
        while (true) {
            skip_ws();
            JsonValue val;
            if (!parse_value(val)) return false;
            out.push_back(std::move(val));
            skip_ws();
            if (pos_ >= text_.size()) return fail("数组未闭合");
            char c = text_[pos_];
            if (c == ',') {
                ++pos_;
                continue;
            }
            if (c == ']') {
                ++pos_;
                return true;
            }
            return fail("数组元素之间缺少 ',' 或 ']'");
        }
    }

    // 解析字符串（含 \uXXXX 转义，仅支持 BMP）。
    bool parse_string(JsonValue& out) {
        ++pos_;  // "
        std::string s;
        while (pos_ < text_.size()) {
            char c = text_[pos_++];
            if (c == '"') {
                out = JsonValue::string(std::move(s));
                return true;
            }
            if (c == '\\') {
                if (pos_ >= text_.size()) return fail("字符串转义不完整");
                char e = text_[pos_++];
                switch (e) {
                    case '"': s.push_back('"'); break;
                    case '\\': s.push_back('\\'); break;
                    case '/': s.push_back('/'); break;
                    case 'b': s.push_back('\b'); break;
                    case 'f': s.push_back('\f'); break;
                    case 'n': s.push_back('\n'); break;
                    case 'r': s.push_back('\r'); break;
                    case 't': s.push_back('\t'); break;
                    case 'u': {
                        if (pos_ + 4 > text_.size()) return fail("\\uXXXX 转义不完整");
                        unsigned cp = 0;
                        for (int i = 0; i < 4; ++i) {
                            char h = text_[pos_++];
                            cp <<= 4;
                            if (h >= '0' && h <= '9') cp |= static_cast<unsigned>(h - '0');
                            else if (h >= 'a' && h <= 'f') cp |= static_cast<unsigned>(h - 'a' + 10);
                            else if (h >= 'A' && h <= 'F') cp |= static_cast<unsigned>(h - 'A' + 10);
                            else return fail("\\uXXXX 含非法十六进制字符");
                        }
                        // 仅支持 BMP（U+0000~U+FFFF）；代理对按原始码元追加（UTF-8 简化）
                        if (cp < 0x80) {
                            s.push_back(static_cast<char>(cp));
                        } else if (cp < 0x800) {
                            s.push_back(static_cast<char>(0xC0 | (cp >> 6)));
                            s.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
                        } else {
                            s.push_back(static_cast<char>(0xE0 | (cp >> 12)));
                            s.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
                            s.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
                        }
                        break;
                    }
                    default: return fail("未知转义字符");
                }
            } else {
                s.push_back(c);
            }
        }
        return fail("字符串未闭合");
    }

    // 解析数字（整数/小数/指数），用 strtod 并查 ERANGE。
    bool parse_number(JsonValue& out) {
        size_t start = pos_;
        if (pos_ < text_.size() && text_[pos_] == '-') ++pos_;
        bool any = false;
        while (pos_ < text_.size() && std::isdigit(static_cast<unsigned char>(text_[pos_]))) {
            ++pos_;
            any = true;
        }
        if (!any) return fail("数字格式错误（缺少整数部分）");
        if (pos_ < text_.size() && text_[pos_] == '.') {
            ++pos_;
            bool frac = false;
            while (pos_ < text_.size() && std::isdigit(static_cast<unsigned char>(text_[pos_]))) {
                ++pos_;
                frac = true;
            }
            if (!frac) return fail("数字格式错误（小数部分为空）");
        }
        if (pos_ < text_.size() && (text_[pos_] == 'e' || text_[pos_] == 'E')) {
            ++pos_;
            if (pos_ < text_.size() && (text_[pos_] == '+' || text_[pos_] == '-')) ++pos_;
            bool exp = false;
            while (pos_ < text_.size() && std::isdigit(static_cast<unsigned char>(text_[pos_]))) {
                ++pos_;
                exp = true;
            }
            if (!exp) return fail("数字格式错误（指数部分为空）");
        }
        std::string tok = text_.substr(start, pos_ - start);
        errno = 0;
        char* end = nullptr;
        double d = std::strtod(tok.c_str(), &end);
        if (errno == ERANGE || end == nullptr || *end != '\0') {
            return fail("数字解析失败（超出范围或非法）");
        }
        out = JsonValue::number(d);
        return true;
    }

    // 解析 true/false 字面量。
    bool parse_bool(JsonValue& out) {
        if (text_.compare(pos_, 4, "true") == 0) {
            pos_ += 4;
            out = JsonValue::boolean(true);
            return true;
        }
        if (text_.compare(pos_, 5, "false") == 0) {
            pos_ += 5;
            out = JsonValue::boolean(false);
            return true;
        }
        return fail("无法识别的字面量（应为 true/false）");
    }

    // 解析 null 字面量。
    bool parse_null(JsonValue& out) {
        if (text_.compare(pos_, 4, "null") == 0) {
            pos_ += 4;
            out = JsonValue::null();
            return true;
        }
        return fail("无法识别的字面量（应为 null）");
    }
};

}  // namespace

// 解析一段 JSON 文本，返回结果与（失败时的）错误描述。
JsonParseResult json_parse(const std::string& text) {
    Parser p(text);
    return p.parse();
}

// 读文件并解析为 JSON；打开/读取失败也走 error 字段返回。
JsonParseResult json_parse_file(const std::string& path) {
    JsonParseResult result;
    std::ifstream f(path, std::ios::binary);
    if (!f.is_open()) {
        result.ok = false;
        result.error = "无法打开配置文件: " + path;
        return result;
    }
    std::ostringstream ss;
    ss << f.rdbuf();
    if (f.bad()) {
        result.ok = false;
        result.error = "读取配置文件失败: " + path;
        return result;
    }
    return json_parse(ss.str());
}

}  // namespace ttbox::core
