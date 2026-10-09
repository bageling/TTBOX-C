// profile_translate_body.cpp — web_body_to_profile 编排入口（见 .hpp 头注释）。
//
// mouse 段已拆到 profile_translate_body_mouse.cpp（控制单文件 ≤300 行），本文件负责
// 推理/采集/FOV/预览四段 + 最终组装。
#include "web/translate/profile_translate.hpp"

#include <cmath>
#include <string>
#include <utility>

#include "web/translate/controller_params.hpp"
#include "web/translate/profile_translate_body_internal.hpp"

namespace ttbox::core::web {

// 面板提交体 → core RuntimeProfile：装配 mouse（拆到 build_mouse）+ 推理/采集/FOV/预览四段。
JsonValue web_body_to_profile(const JsonValue& body, const JsonValue* prev_profile) {
    detail::MouseBuildResult built = detail::build_mouse(body);
    JsonValue& mouse = built.mouse;

    // 6) 推理参数。
    JsonValue inference = JsonValue::object();
    const JsonValue& conf = json_field(body, "video_detection_confidence");
    if (!conf.is_null()) inference.set("confidence", conf);
    const JsonValue& iou = json_field(body, "video_detection_iou");
    if (!iou.is_null()) inference.set("iou", iou);
    if (built.class_union_mask != -1) {
        JsonValue cf = JsonValue::array();
        for (int i = 0; i < 32; ++i) {
            if ((built.class_union_mask & (1LL << i)) != 0) {
                cf.push_back(JsonValue::number(static_cast<double>(i)));
            }
        }
        inference.set("class_filter", std::move(cf));
    }

    // 7) 采集（crop_size 归一化，绝不产生 1~63）。
    JsonValue capture = JsonValue::object();
    const JsonValue& cap = json_field(body, "capture");
    const JsonValue& crop_v = json_field(cap, "crop_size");
    if (!crop_v.is_null()) {
        const int64_t crop = normalize_capture_crop_size(crop_v);
        capture.set("width", JsonValue::number(static_cast<double>(crop)));
        capture.set("height", JsonValue::number(static_cast<double>(crop)));
    }

    // 8) FOV 基准半径：prev_profile 优先；无 prev 时经注入 getter 取（失败走中性默认）。
    JsonValue prev_fov = JsonValue::object();
    if (prev_profile != nullptr) {
        const JsonValue& pfov = json_field(*prev_profile, "fov");
        if (pfov.is_object()) prev_fov = pfov;
    } else {
        JsonValue prof = JsonValue::null();
        try {
            prof = runtime_profile_getter()();
        } catch (...) {
            prof = JsonValue::null();
        }
        const JsonValue& pfov = json_field(prof, "fov");
        if (pfov.is_object()) prev_fov = pfov;
    }

    JsonValue fov = JsonValue::object();
    const JsonValue& shape = json_field(prev_fov, "shape");
    fov.set("shape", shape.is_null() ? JsonValue::number(0.0) : shape);
    const JsonValue& cx = json_field(prev_fov, "center_x");
    fov.set("center_x", cx.is_null() ? JsonValue::number(0.5) : cx);
    const JsonValue& cy = json_field(prev_fov, "center_y");
    fov.set("center_y", cy.is_null() ? JsonValue::number(0.5) : cy);

    const JsonValue& range_factor = json_field(body, "range_factor");
    if (range_factor.is_null()) {
        const JsonValue& en = json_field(prev_fov, "enabled");
        fov.set("enabled", en.is_null() ? JsonValue::boolean(false) : en);
        const JsonValue& radius = json_field(prev_fov, "radius");
        fov.set("radius", radius.is_null() ? JsonValue::number(0.5) : radius);
    } else {
        fov.set("enabled", JsonValue::boolean(true));
        const double r =
            std::nearbyint(fov_factor_clamp(range_factor, 1.0) * 1000000.0) / 1000000.0;
        fov.set("radius", JsonValue::number(r));
    }

    // 9) 预览帧率。
    JsonValue preview = JsonValue::object();
    const JsonValue& lat = json_field(body, "latency");
    const JsonValue& piv = json_field(lat, "preview_interval_ms");
    if (!piv.is_null()) {
        const int64_t iv = to_int64(piv, 0);
        if (iv > 0) {
            int64_t fps = 1000 / iv;
            if (fps < 1) fps = 1;
            if (fps > 60) fps = 60;
            preview.set("fps", JsonValue::number(static_cast<double>(fps)));
        }
    }

    JsonValue prof = JsonValue::object();
    prof.set("mouse", std::move(mouse));
    prof.set("inference", std::move(inference));
    prof.set("capture", std::move(capture));
    prof.set("fov", std::move(fov));
    if (!preview.as_object().empty()) prof.set("preview", std::move(preview));
    const JsonValue& model_id = json_field(body, "model_id");
    if (!model_id.is_null()) prof.set("model_id", model_id);
    return prof;
}

}  // namespace ttbox::core::web
