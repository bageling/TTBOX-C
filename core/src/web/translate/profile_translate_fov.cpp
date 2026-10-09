// profile_translate_fov.cpp — FOV 倍率夹取 + 运行时配置获取器（见 .hpp 头注释）。
#include "web/translate/profile_translate.hpp"

#include <cmath>
#include <string>
#include <utility>

namespace ttbox::core::web {

namespace {

// 对齐 Python capture_geometry.py 的 FOV_FACTOR_MIN。
constexpr double kFovFactorMin = 0.1;

// 进程内单例的运行时配置获取器（默认返回 null，纯函数场景的中性默认）。
RuntimeProfileGetter& mutable_getter() {
    static RuntimeProfileGetter getter = [] { return JsonValue::null(); };
    return getter;
}

}  // namespace

// 注入运行时配置获取器（等价 Python 的 monkeypatch 锚点）。
void set_runtime_profile_getter(RuntimeProfileGetter getter) {
    mutable_getter() = std::move(getter);
}

// 读取当前运行时配置获取器。
RuntimeProfileGetter runtime_profile_getter() {
    return mutable_getter();
}

// 把 FOV 倍率夹取到 [kFovFactorMin, 1.0]；非数/NaN/字符串不可解析时回退 default_value。
double fov_factor_clamp(const JsonValue& v, double default_value) {
    double f = 0.0;
    if (v.is_number()) {
        f = v.as_number();
    } else if (v.is_string()) {
        try {
            f = std::stod(v.as_string());
        } catch (...) {
            return default_value;
        }
    } else {
        return default_value;
    }
    if (std::isnan(f)) return default_value;  // 对齐 Python `f != f`
    if (f < kFovFactorMin) f = kFovFactorMin;
    if (f > 1.0) f = 1.0;
    return f;
}

// 面板半径 + 启用开关 → 面板倍率（未启用恒为 1.0，否则按 radius 夹取）。
double fov_radius_to_factor(const JsonValue& radius, const JsonValue& enabled) {
    if (!json_truthy(enabled)) return 1.0;  // 对齐 Python `if not enabled`
    return fov_factor_clamp(radius, 1.0);
}

}  // namespace ttbox::core::web
