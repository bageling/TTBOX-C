// motion_store_validate.cpp — 训练样本契约校验（finite/point 工具 + validate_motion_sample）。
//
// 自 ttbox_motion/training.py 逐行移植（因 motion_store.cpp 超 300 行拆出）。
#include "web/infra/motion_store.hpp"

#include <cmath>
#include <string>
#include <utility>

namespace ttbox::core::web {

namespace {

constexpr double kPointsMin = 2;
constexpr double kPointsMax = 2048;
constexpr size_t kSampleMaxBytes = 256 * 1024;

// _finite：bool 拒绝、数值/数值串通过、非有限拒绝。
double finite_of(const JsonValue& v, const std::string& name) {
    if (v.is_bool()) throw MotionError(name + " must be finite");
    double d = 0.0;
    if (v.is_number()) {
        d = v.as_number();
    } else if (v.is_string()) {
        try {
            d = std::stod(v.as_string());
        } catch (...) {
            throw MotionError(name + " must be finite");
        }
    } else {
        throw MotionError(name + " must be finite");
    }
    if (!std::isfinite(d)) throw MotionError(name + " must be finite");
    return d;
}

// 解析 {x,y} 点；非对象或坐标非有限抛 MotionError。
std::pair<double, double> point_of(const JsonValue& v, const std::string& name) {
    if (!v.is_object()) throw MotionError(name + " must be an object");
    return {finite_of(field_or_null(v, "x"), name + ".x"),
            finite_of(field_or_null(v, "y"), name + ".y")};
}

}  // namespace

// 校验训练样本契约（schema/mode/canvas/路径边界/终点命中），合法返回统计对象。
JsonValue validate_motion_sample(const JsonValue& payload) {
    if (!payload.is_object()) throw MotionError("sample must be an object");
    if (field_or_null(payload, "schema").as_string("") != "ttbox.motion-sample.v1") {
        throw MotionError("schema must be ttbox.motion-sample.v1");
    }
    const std::string mode = field_or_null(payload, "mode").as_string("");
    if (mode != "reaction" && mode != "continuous") throw MotionError("mode is invalid");
    if (field_or_null(payload, "completion").as_string("") != "dwell") {
        throw MotionError("completion is invalid");
    }
    const JsonValue canvas = field_or_null(payload, "canvas");
    if (!canvas.is_object()) throw MotionError("canvas is required");
    const double width = finite_of(field_or_null(canvas, "width"), "canvas.width");
    const double height = finite_of(field_or_null(canvas, "height"), "canvas.height");
    if (!(64 <= width && width <= 4096 && 64 <= height && height <= 4096)) {
        throw MotionError("canvas is outside the accepted range");
    }
    const auto start = point_of(field_or_null(payload, "start"), "start");
    const auto target = point_of(field_or_null(payload, "target"), "target");
    const double radius = finite_of(field_or_null(payload, "radius"), "radius");
    if (!(1 <= radius && radius <= 512)) {
        throw MotionError("radius is outside the accepted range");
    }
    const JsonValue points = field_or_null(payload, "points");
    if (!points.is_array() ||
        !(kPointsMin <= static_cast<double>(points.as_array().size()) &&
          static_cast<double>(points.as_array().size()) <= kPointsMax)) {
        throw MotionError("point count is invalid");
    }
    if (payload.dump().size() > kSampleMaxBytes) {
        throw MotionError("motion sample exceeds 256KB");
    }

    double x = start.first;
    double y = start.second;
    double total_ms = 0.0;
    double path_length = 0.0;
    for (size_t index = 0; index < points.as_array().size(); ++index) {
        const JsonValue& item = points.as_array()[index];
        if (!item.is_object()) throw MotionError("point " + std::to_string(index) + " is invalid");
        const double dt = finite_of(field_or_null(item, "dt"), "points[" + std::to_string(index) + "].dt");
        const double dx = finite_of(field_or_null(item, "dx"), "points[" + std::to_string(index) + "].dx");
        const double dy = finite_of(field_or_null(item, "dy"), "points[" + std::to_string(index) + "].dy");
        if (!(0 <= dt && dt <= 2000)) throw MotionError("point dt is outside the accepted range");
        x += dx;
        y += dy;
        if (!(0 <= x && x <= width && 0 <= y && y <= height)) {
            throw MotionError("motion path leaves canvas bounds");
        }
        total_ms += dt;
        path_length += std::hypot(dx, dy);
    }
    if (total_ms > 120000) throw MotionError("motion sample duration is too long");
    if (std::hypot(x - target.first, y - target.second) > radius) {
        throw MotionError("motion path did not finish inside target");
    }
    const double straight = std::hypot(target.first - start.first, target.second - start.second);

    JsonValue out = JsonValue::object();
    out.set("mode", JsonValue::string(mode));
    out.set("point_count", JsonValue::number(static_cast<double>(points.as_array().size())));
    out.set("duration_ms", JsonValue::number(total_ms));
    out.set("path_length", JsonValue::number(path_length));
    out.set("path_efficiency",
            JsonValue::number(path_length > 0 ? straight / path_length : 0.0));
    if (mode == "reaction") out.set("reaction_ms", JsonValue::number(total_ms));
    else out.set("reaction_ms", JsonValue::null());
    return out;
}

}  // namespace ttbox::core::web
