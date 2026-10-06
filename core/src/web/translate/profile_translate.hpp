// profile_translate.hpp — Web 请求体 ↔ RuntimeProfile 双向翻译。
//
// 自 plugins/web/lib/profile_translate.py 逐行为移植。职责：面板提交体 →
// core RuntimeProfile（web_body_to_profile）、core RuntimeProfile → 面板可读体
// （profile_to_web），含 aim_profiles[] 卡组校验（validate_aim_profiles）与热键位掩码换算。
//
// 依赖接缝：Python 的 _get_runtime_profile 是 monkeypatch 锚点（29 处测试注入），
// 此处以 RuntimeProfileGetter（可注入 std::function）等价替代，默认返回 null（中性）。
#pragma once

#include <cctype>
#include <cstdint>
#include <functional>
#include <stdexcept>
#include <string>

#include "common/Json.hpp"

namespace ttbox::core::web {

// 档位数上限（面板能加卡，这里是硬护栏）/ 下限（core 侧 aim_profiles 非空是不变量）。
inline constexpr int kAimProfileMax = 8;
inline constexpr int kAimProfileMin = 1;

// 提交体语义非法（JSON 没坏，是参数配错了）。调用方转成 400 + 可读原因。
class ConfigValidationError : public std::runtime_error {
public:
    explicit ConfigValidationError(const std::string& msg) : std::runtime_error(msg) {}
};

// 运行时配置获取器（等价 Python 的 _get_runtime_profile 补丁锚点）。
using RuntimeProfileGetter = std::function<JsonValue()>;

// 注入/读取运行时配置获取器。默认返回 JsonValue::null()（纯函数场景中性默认）。
void set_runtime_profile_getter(RuntimeProfileGetter getter);
RuntimeProfileGetter runtime_profile_getter();

// ---- 内部共享小工具（inline）----

// strip + lower（对齐 Python str.strip().lower()）。
inline std::string trim_lower(const std::string& raw) {
    size_t b = 0;
    size_t e = raw.size();
    while (b < e && std::isspace(static_cast<unsigned char>(raw[b])) != 0) ++b;
    while (e > b && std::isspace(static_cast<unsigned char>(raw[e - 1])) != 0) --e;
    std::string out;
    out.reserve(e - b);
    for (size_t i = b; i < e; ++i) {
        out.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(raw[i]))));
    }
    return out;
}

// 取对象成员；缺失/非对象/值为 null 统一返回静态 null（对齐 dict.get 的 None 语义）。
inline const JsonValue& json_field(const JsonValue& obj, const std::string& key) {
    static const JsonValue kNull = JsonValue::null();
    if (obj.is_object()) {
        if (const JsonValue* v = obj.find(key)) return *v;
    }
    return kNull;
}

// 对齐 Python 的 truthiness（bool()）。
inline bool json_truthy(const JsonValue& v) {
    switch (v.type()) {
        case JsonType::kNull: return false;
        case JsonType::kBool: return v.as_bool();
        case JsonType::kNumber: return v.as_number() != 0.0;
        case JsonType::kString: return !v.as_string().empty();
        case JsonType::kArray: return !v.as_array().empty();
        case JsonType::kObject: return !v.as_object().empty();
    }
    return false;
}

// 对齐 Python int()：number 截断、string 解析、bool→0/1，其余回退 def。
inline int64_t to_int64(const JsonValue& v, int64_t def = 0) {
    if (v.is_number()) return v.as_int();
    if (v.is_string()) {
        try {
            return std::stoll(trim_lower(v.as_string()));
        } catch (...) {
            return def;
        }
    }
    if (v.is_bool()) return v.as_bool() ? 1 : 0;
    return def;
}

// ---- FOV 倍率夹取 ----
double fov_factor_clamp(const JsonValue& v, double default_value);
double fov_radius_to_factor(const JsonValue& radius, const JsonValue& enabled);

// ---- 热键卡 / aim_profiles ----
JsonValue aim_profile_core_dict(const JsonValue& p);
int64_t aim_profile_key_bits(const JsonValue& p);
JsonValue validate_aim_profiles(const JsonValue& profiles);
JsonValue aim_profiles_to_web(const JsonValue& mouse, const JsonValue& inf);

// ---- 双向翻译入口 ----
JsonValue web_body_to_profile(const JsonValue& body, const JsonValue* prev_profile = nullptr);
JsonValue profile_to_web(const JsonValue& prof);

}  // namespace ttbox::core::web
