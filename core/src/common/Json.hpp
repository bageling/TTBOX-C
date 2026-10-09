// Json.hpp — 极简 JSON 解析/序列化（零第三方依赖）
//
// 仅实现 TTBox 配置与 IPC 所需子集：null/bool/number/string/array/object，
// 完整支持标准 JSON 语法（转义/unicode \uXXXX/嵌套），带明确错误信息。
// 不引入任何外部 JSON 库（避免大型依赖）。
#pragma once

#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace ttbox::core {

// JSON 值类型标签。
enum class JsonType { kNull, kBool, kNumber, kString, kArray, kObject };

// 轻量 JSON 值（null/bool/number/string/array/object）：构造、读写、序列化与解析入口。
class JsonValue {
public:
    static JsonValue null();
    static JsonValue boolean(bool v);
    static JsonValue number(double v);
    static JsonValue string(std::string v);
    static JsonValue array();
    static JsonValue array(std::vector<JsonValue> v);
    static JsonValue object();

    JsonType type() const { return type_; }
    bool is_null() const { return type_ == JsonType::kNull; }
    bool is_bool() const { return type_ == JsonType::kBool; }
    bool is_number() const { return type_ == JsonType::kNumber; }
    bool is_string() const { return type_ == JsonType::kString; }
    bool is_array() const { return type_ == JsonType::kArray; }
    bool is_object() const { return type_ == JsonType::kObject; }

    bool as_bool(bool def = false) const;
    double as_number(double def = 0.0) const;
    int64_t as_int(int64_t def = 0) const;
    std::string as_string(const std::string& def = "") const;
    const std::vector<JsonValue>& as_array() const;
    const std::map<std::string, JsonValue>& as_object() const;

    // object 成员访问：不存在返回 nullptr
    const JsonValue* find(const std::string& key) const;

    void set(const std::string& key, JsonValue v);  // 仅 object 有效
    void push_back(JsonValue v);                     // 仅 array 有效

    std::string dump() const;  // 紧凑序列化（JSON 字符串）

    bool operator==(const JsonValue& o) const;
    bool operator!=(const JsonValue& o) const { return !(*this == o); }

private:
    JsonType type_ = JsonType::kNull;
    bool bool_ = false;
    double num_ = 0.0;
    std::string str_;
    std::vector<JsonValue> arr_;
    std::map<std::string, JsonValue> obj_;
};

// 解析结果：ok=false 时 error 为明确错误（含位置）
struct JsonParseResult {
    bool ok = false;
    std::string error;
    JsonValue value;
};

JsonParseResult json_parse(const std::string& text);
JsonParseResult json_parse_file(const std::string& path);

}  // namespace ttbox::core
