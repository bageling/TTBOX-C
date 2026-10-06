// profile_translate_fov.cpp — FOV 倍率夹取 + 运行时配置获取器（见 .hpp 头注释）。
#include "web/translate/profile_translate.hpp"

#include <cmath>
#include <string>
#include <utility>

namespace ttbox::core::web {

namespace {

// 对齐 Python capture_geometry.py 的 FOV_FACTOR_MIN。
constexpr double kFovFactorMin = 0.1;

RuntimeProfileGetter& mutable_getter() {
    static RuntimeProfileGetter getter = [] { return JsonValue::null(); };
    return getter;
}

}  // namespace

void set_runtime_profile_getter(RuntimeProfileGetter getter) {
    mutable_getter() = std::move(getter);
}

RuntimeProfileGetter runtime_profile_getter() {
    return mutable_getter();
}

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

double fov_radius_to_factor(const JsonValue& radius, const JsonValue& enabled) {
    if (!json_truthy(enabled)) return 1.0;  // 对齐 Python `if not enabled`
    return fov_factor_clamp(radius, 1.0);
}

}  // namespace ttbox::core::web
