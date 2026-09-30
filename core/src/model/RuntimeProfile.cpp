// RuntimeProfile.cpp — RuntimeProfile JSON 序列化/校验
/*
 * TTBOX 文件说明
 *
 * 文件：RuntimeProfile.cpp
 *
 * 作用：
 *   运行时配置文件的定义和翻译。
 *   定义所有可调参数，并在配置格式和 TTBOX 内部格式之间转换。
 *
 * 小白理解：
 *   你在 Web 页面上看到的每个参数（置信度、截取尺寸、PID 参数等），
 *   都在这里定义。它还负责把配置格式的参数翻译成 TTBOX 内部格式。
 *
 * 注意：
 *   本注释仅用于说明代码，不改变程序逻辑。
 */

#include <algorithm>
#include <cmath>
#include <cstdio>

#include "model/RuntimeProfile.hpp"

namespace ttbox::core {

// ---------------------------------------------------------------------------
// CaptureProfile
// ---------------------------------------------------------------------------

bool CaptureProfile::valid(uint32_t frame_w, uint32_t frame_h,
                           std::string* error) const {
    // 0 表示"用全帧"，等价于合法
    const uint32_t w = (width == 0) ? frame_w : width;
    const uint32_t h = (height == 0) ? frame_h : height;
    if (w == 0 || h == 0) {
        if (error) *error = "ROI 尺寸不能为 0";
        return false;
    }
    if (w > frame_w || h > frame_h) {
        if (error) *error = "ROI 尺寸超全帧: " + std::to_string(w) + "x" +
                            std::to_string(h) + " > " + std::to_string(frame_w) +
                            "x" + std::to_string(frame_h);
        return false;
    }
    // V1.0.13：裁剪区恒以屏幕中心为心（offset 已删），clamp 后必然界内
    const int32_t cx = static_cast<int32_t>(frame_w / 2);
    const int32_t cy = static_cast<int32_t>(frame_h / 2);
    const int32_t rx = std::max<int32_t>(0, std::min<int32_t>(
        cx - static_cast<int32_t>(w / 2), static_cast<int32_t>(frame_w - w)));
    const int32_t ry = std::max<int32_t>(0, std::min<int32_t>(
        cy - static_cast<int32_t>(h / 2), static_cast<int32_t>(frame_h - h)));
    (void)rx; (void)ry;  // 计算即校验（clamp 后必界内）；实际起点由 apply_runtime_profile 计算
    return true;
}

// ---------------------------------------------------------------------------
// 工具：读 object 成员（key 不存在返回默认值）
// ---------------------------------------------------------------------------
namespace {

// ---------------------------------------------------------------------------
// capture ROI 合法范围：kMinCaptureRoiPx / kMaxCaptureRoiPx 定义已提升到
// RuntimeProfile.hpp（单一权威源，DUP-10），此处与 Application SET_CONFIG 校验共用。
// ---------------------------------------------------------------------------

// 退化值消毒：落在 [1, kMinCaptureRoiPx) 的非法小值纠正为 0（全帧）。
// 用于 from_json 加载历史坏配置时自愈，避免服务因一个坏字段起不来或瞎跑。
uint32_t sanitize_capture_roi(uint32_t v) {
    return (v > 0 && v < kMinCaptureRoiPx) ? 0u : v;
}

// 热键位掩码合法范围（1=left 2=right 4=middle 8=back 16=forward，可组合）。
// ★ 2026-09-25 修：此前直接 static_cast<uint8_t>(obj_int(...))，负数会绕回成 255
//   （如 -1 ⇒ 0xFF），而命中判据是 `buttons & hotkey != 0` ⇒ 255 对**任意**物理键成立
//   ⇒ 按任何键都瞄准（热键闸门 fail-open）。这里改成「越界一律 0（永不命中）」= fail-closed。
//   hotkey2 允许 0（=不使用副键），所以 0 本身要原样保留。
uint8_t sanitize_hotkey_bits(int64_t v) {
    if (v < 0 || v > 0x1F) return 0;  // 负数 / 超出 5 个位 ⇒ 永不命中
    return static_cast<uint8_t>(v);
}

int64_t obj_int(const JsonValue& o, const char* key, int64_t def) {
    const JsonValue* v = o.find(key);
    return (v && v->is_number()) ? v->as_int(def) : def;
}
double obj_num(const JsonValue& o, const char* key, double def) {
    const JsonValue* v = o.find(key);
    return (v && v->is_number()) ? v->as_number(def) : def;
}
bool obj_bool(const JsonValue& o, const char* key, bool def) {
    const JsonValue* v = o.find(key);
    return (v && v->is_bool()) ? v->as_bool(def) : def;
}
std::string obj_str(const JsonValue& o, const char* key, const std::string& def) {
    const JsonValue* v = o.find(key);
    return (v && v->is_string()) ? v->as_string(def) : def;
}

}  // namespace

// ---------------------------------------------------------------------------
// RuntimeProfile
// ---------------------------------------------------------------------------

bool RuntimeProfile::validate(std::string* error) const {
    // 非有限值总闸：JSON 1e999 等可产生 inf，NaN/inf 进入 PID 会输出乱飞（fail-closed 防线）。
    const float mouse_nums[] = {
        mouse.kp_x, mouse.kp_y, mouse.kd_x, mouse.kd_y,
        mouse.predict_x, mouse.predict_y, mouse.rate_x, mouse.rate_y,
        mouse.fov_range, mouse.confidence, mouse.sensitivity, mouse.output_scale,
        mouse.deadzone_x, mouse.deadzone_y, mouse.output_deadzone,
        mouse.hfov, mouse.vfov, mouse.move_speed_x, mouse.move_speed_y,
        mouse.aim_point.offset_x, mouse.aim_point.offset_y,
        mouse.personal_motion.curve_blend,
        inference.confidence, inference.iou,
        fov.center_x, fov.center_y, fov.radius,
    };
    for (const float v : mouse_nums) {
        if (!std::isfinite(v)) {
            if (error) *error = "配置含非有限数值（NaN/Infinity），已拒绝";
            return false;
        }
    }
    if (inference.confidence < 0.0f || inference.confidence > 1.0f) {
        if (error) *error = "confidence 必须在 [0,1]";
        return false;
    }
    if (inference.iou < 0.0f || inference.iou > 1.0f) {
        if (error) *error = "iou 必须在 [0,1]";
        return false;
    }
    for (const int c : inference.class_filter) {
        if (c < 0) {
            if (error) *error = "class_filter 含负类别";
            return false;
        }
    }
    if (inference.max_detections < 0) {
        if (error) *error = "max_detections 不能为负";
        return false;
    }
    if (fov.enabled) {
        if (fov.center_x < 0.0f || fov.center_x > 1.0f ||
            fov.center_y < 0.0f || fov.center_y > 1.0f) {
            if (error) *error = "FOV 中心必须在 [0,1]";
            return false;
        }
        if (fov.radius <= 0.0f || fov.radius > 1.0f) {
            if (error) *error = "FOV 半径必须在 (0,1]";
            return false;
        }
    }
    if (mouse.fov_range < 0.0f || mouse.fov_range > 1.0f) {
        if (error) *error = "mouse.fov_range 必须在 [0,1]";
        return false;
    }
    if (mouse.confidence < 0.0f || mouse.confidence > 1.0f) {
        if (error) *error = "mouse.confidence 必须在 [0,1]";
        return false;
    }
    if (mouse.kp_x < 0.0f || mouse.kp_y < 0.0f) {
        if (error) *error = "mouse.kp 不能为负";
        return false;
    }
    // ★ 2026-09-26（第四轮审计）：kd 为负 = 阻尼变正反馈，必须挡住。
    //   （原先这里还校验 smooth 的 [0,9999]；V1.0.13 起 smooth 已从参数面删除。）
    if (mouse.kd_x < 0.0f || mouse.kd_y < 0.0f) {
        if (error) *error = "mouse.kd 不能为负";
        return false;
    }
    if (mouse.hfov <= 0.0f || mouse.hfov >= 180.0f ||
        mouse.vfov <= 0.0f || mouse.vfov >= 180.0f) {
        if (error) *error = "mouse.hfov/vfov 必须在 (0,180)";
        return false;
    }
    if (mouse.move_speed_x < 0.0f || mouse.move_speed_y < 0.0f) {
        if (error) *error = "mouse.move_speed 不能为负";
        return false;
    }
    if (mouse.rate_x < 0.0f || mouse.rate_y < 0.0f ||
        mouse.sensitivity < 0.0f || mouse.output_scale < 0.0f) {
        if (error) *error = "mouse 输出系数不能为负";
        return false;
    }

    if (mouse.lost_grace_ms < 0.0f) {
        if (error) *error = "mouse.lost_grace_ms 不能为负";
        return false;
    }
    if (mouse.switch_hysteresis < 0.0f) {
        if (error) *error = "mouse.switch_hysteresis 不能为负";
        return false;
    }
    if (mouse.switch_cooldown_ms < 0.0f) {
        if (error) *error = "mouse.switch_cooldown_ms 不能为负";
        return false;
    }
    // V1.0.07：贴裁剪区剔除 / 锁定尺寸一致性（默认开，见 TargetSelector.hpp）
    if (mouse.clip_margin_px < 0.0f || mouse.clip_center_max_px < 0.0f ||
        mouse.track_size_ratio < 0.0f) {
        if (error) *error = "mouse.clip_margin_px / clip_center_max_px / track_size_ratio 不能为负";
        return false;
    }
    // 选靶四项机制（2026-09-24 补通路）：均为"加进来才生效"的可选增强，默认全关。
    if (mouse.lock_hold_ms < 0.0f || mouse.switch_threshold_px < 0.0f) {
        if (error) *error = "mouse.lock_hold_ms / switch_threshold_px 不能为负";
        return false;
    }
    if (mouse.weight_dist < 0.0f || mouse.weight_size < 0.0f || mouse.stickiness < 0.0f) {
        if (error) *error = "mouse 选靶评分权重不能为负";
        return false;
    }
    // 自动扳机 v7.26（TriggerConfig）已于 2026-09-29 整段删除 ⇒ 对应的两段校验一并删除。
    // 旧配置里残留的 `mouse.trigger` 段会被下面的解析逻辑直接忽略。
    // ---- 压枪（recoil，2026-09-30 对照 yu 重做）----
    // 参数面收敛到 4 个数值项；越界一律拒绝（yu 也是在解析时钳制的）。
    if (mouse.recoil.target_lost_release_ms < 0.0f || mouse.recoil.target_lost_release_ms > 3000.0f ||
        mouse.recoil.trigger_delay_ms < 0.0f ||
        mouse.recoil.strength < 0.0f || mouse.recoil.strength > 300.0f ||
        mouse.recoil.curve_strength < 0.0f || mouse.recoil.curve_strength > 1.0f ||
        mouse.recoil.roi_h < 0.0f) {
        if (error) *error = "recoil 参数越界（strength[0,300] release_ms[0,3000] curve[0,1] roi_h>=0）";
        return false;
    }
    if (mouse.recoil.speed < 0.1f || mouse.recoil.speed > 3.0f) {
        if (error) *error = "recoil.speed 需在 [0.1,3.0]（对齐 yu）";
        return false;
    }
    if (mouse.trigger2.first_err < 0.0f || mouse.trigger2.first_delay < 0.0f ||
        mouse.trigger2.fire_interval < 0.0f || mouse.trigger2.fire_random < 0.0f ||
        mouse.trigger2.press_duration < 0.0f || mouse.trigger2.precision_range < 0.0f ||
        mouse.trigger2.retarget_reset_ms < 0.0f || mouse.trigger2.move_throttle_frames < 0 ||
        mouse.trigger2.precision_frames < 0 || mouse.trigger2.fire_count < 0 ||
        mouse.trigger2.stop_detect_tolerance < 0 || mouse.trigger2.stop_detect_range < 0 ||
        mouse.trigger2.stop_detect_interval < 0) {
        if (error) *error = "trigger2 时长/距离/计数不能为负";
        return false;
    }
    if (mouse.trigger2.confidence < 0.0f || mouse.trigger2.confidence > 1.0f) {
        if (error) *error = "trigger2.confidence 超出 [0,1]";
        return false;
    }
    if (mouse.personal_motion.curve_blend < 0.0f || mouse.personal_motion.curve_blend > 1.0f) {
        if (error) *error = "personal_motion 混合参数超出范围";
        return false;
    }
    for (const float knot : mouse.personal_motion.knots) {
        if (!std::isfinite(knot) || knot < 0.0f || knot > 1.0f) {
            if (error) *error = "personal_motion knots 必须在 [0,1]";
            return false;
        }
    }
    if (mouse.personal_motion.knots.size() > 32) {
        if (error) *error = "personal_motion knots 最多 32 个";
        return false;
    }
    // capture ROI 退化值防线（fail-closed）：0=全帧合法；非零则必须落在
    // [kMinCaptureRoiPx, kMaxCaptureRoiPx]。拦截 1×1 这类"静默摧毁流水线"的配置。
    // 注意：此处不校验"是否超全帧"（validate 无 frame_w/h 上下文），
    // 越界由 WorkerPool::apply_runtime_profile 的 rw<=fw/rh<=fh 守卫兜底（超界即不应用 ROI）。
    for (const uint32_t v : {capture.width, capture.height}) {
        if (v != 0 && (v < kMinCaptureRoiPx || v > kMaxCaptureRoiPx)) {
            if (error) *error = "capture 截取尺寸非法: " + std::to_string(v) +
                                "（0=全帧，或需在 " + std::to_string(kMinCaptureRoiPx) +
                                "~" + std::to_string(kMaxCaptureRoiPx) + " 之间）";
            return false;
        }
    }
    // P-ZC-1 采集层裁剪：0 = 沿用全局配置（合法）；非零走与 capture 同一道
    // 退化值防线，拦住 1×1 这种会把采集流做成 1 像素的配置。
    for (const uint32_t v : {video.crop_width, video.crop_height}) {
        if (v != 0 && (v < kMinCaptureRoiPx || v > kMaxCaptureRoiPx)) {
            if (error) *error = "video 采集截取尺寸非法: " + std::to_string(v) +
                                "（0=沿用全局配置，或需在 " + std::to_string(kMinCaptureRoiPx) +
                                "~" + std::to_string(kMaxCaptureRoiPx) + " 之间）";
            return false;
        }
    }
    if (preview.width == 0 || preview.height == 0 ||
        preview.width > 3840 || preview.height > 2160 ||
        preview.roi_w == 0 || preview.roi_h == 0) {
        if (error) *error = "preview 尺寸/ROI 必须为正";
        return false;
    }
    return true;
}

JsonValue RuntimeProfile::to_json() const {
    JsonValue root = JsonValue::object();
    root.set("model_id", JsonValue::string(model_id));

    JsonValue cap = JsonValue::object();
    cap.set("width", JsonValue::number(static_cast<double>(capture.width)));
    cap.set("height", JsonValue::number(static_cast<double>(capture.height)));
    root.set("capture", std::move(cap));

    JsonValue inf = JsonValue::object();
    inf.set("confidence", JsonValue::number(static_cast<double>(inference.confidence)));
    inf.set("iou", JsonValue::number(static_cast<double>(inference.iou)));
    JsonValue cf = JsonValue::array();
    for (const int c : inference.class_filter) cf.push_back(JsonValue::number(static_cast<double>(c)));
    inf.set("class_filter", std::move(cf));
    inf.set("max_detections", JsonValue::number(static_cast<double>(inference.max_detections)));
    root.set("inference", std::move(inf));

    // P-ZC-1：采集层裁剪 + 零拷贝开关（缺段/缺键 = 沿用全局配置，老机器升级行为不变）
    JsonValue vid = JsonValue::object();
    vid.set("crop_width", JsonValue::number(static_cast<double>(video.crop_width)));
    vid.set("crop_height", JsonValue::number(static_cast<double>(video.crop_height)));
    vid.set("zero_copy_input", JsonValue::boolean(video.zero_copy_input));
    root.set("video", std::move(vid));

    JsonValue gf = JsonValue::object();
    gf.set("enabled", JsonValue::boolean(geometry_filter.enabled));
    gf.set("min_head_conf", JsonValue::number(geometry_filter.min_head_conf));
    gf.set("min_body_conf", JsonValue::number(geometry_filter.min_body_conf));
    gf.set("paired_head_min_conf", JsonValue::number(geometry_filter.paired_head_min_conf));
    gf.set("head_only_min_conf", JsonValue::number(geometry_filter.head_only_min_conf));
    gf.set("head_only_center_max_px", JsonValue::number(geometry_filter.head_only_center_max_px));
    gf.set("min_body_width_px", JsonValue::number(geometry_filter.min_body_width_px));
    gf.set("min_body_height_px", JsonValue::number(geometry_filter.min_body_height_px));
    gf.set("border_reject_enabled", JsonValue::boolean(geometry_filter.reject_border));
    root.set("geometry_filter", std::move(gf));

    JsonValue fobj = JsonValue::object();
    fobj.set("enabled", JsonValue::boolean(fov.enabled));
    fobj.set("shape", JsonValue::number(static_cast<double>(fov.shape == FovShape::kRect ? 1 : 0)));
    fobj.set("radius", JsonValue::number(static_cast<double>(fov.radius)));
    fobj.set("center_x", JsonValue::number(static_cast<double>(fov.center_x)));
    fobj.set("center_y", JsonValue::number(static_cast<double>(fov.center_y)));
    root.set("fov", std::move(fobj));

    // A10：鼠标 AI 注入配置
    JsonValue m = JsonValue::object();
    m.set("enabled", JsonValue::boolean(mouse.enabled));
    // 平铺热键 key：**只写不读**（解析时仅在 aim_profiles 缺失的场合当合成源）。
    // 继续写出去是为了让旧版 Core / 外部工具仍能读懂配置，便于回退排查。
    // 值 = 第 0 档的镜像；真源始终是下面的 mouse.aim_profiles。
    {
        const auto& ap0 = aim::aim_profile_at(mouse, 0);
        m.set("aim_hotkey", JsonValue::number(static_cast<double>(ap0.hotkey)));
        m.set("aim_hotkey2", JsonValue::number(static_cast<double>(ap0.hotkey2)));
        m.set("aim_hotkey_mode", JsonValue::string(aim::mouse_hotkey_mode_name(ap0.hotkey_mode)));
    }
    // ---- 瞄准档位（真源）----
    JsonValue aps = JsonValue::array();
    for (const auto& ap : mouse.aim_profiles) {
        JsonValue j = JsonValue::object();
        j.set("hotkey", JsonValue::number(static_cast<double>(ap.hotkey)));
        j.set("hotkey2", JsonValue::number(static_cast<double>(ap.hotkey2)));
        j.set("hotkey_mode", JsonValue::string(aim::mouse_hotkey_mode_name(ap.hotkey_mode)));
        j.set("offset_x", JsonValue::number(static_cast<double>(ap.offset_x)));
        j.set("offset_y", JsonValue::number(static_cast<double>(ap.offset_y)));
        j.set("sensitivity", JsonValue::number(static_cast<double>(ap.sensitivity)));
        j.set("fov_scale", JsonValue::number(static_cast<double>(ap.fov_scale)));
        JsonValue ap_cos = JsonValue::array();
        for (const auto& c : ap.class_offsets) {
            JsonValue o = JsonValue::object();
            o.set("class_id", JsonValue::number(static_cast<double>(c.class_id)));
            o.set("offset_x", JsonValue::number(static_cast<double>(c.offset_x)));
            o.set("offset_y", JsonValue::number(static_cast<double>(c.offset_y)));
            o.set("priority", JsonValue::number(static_cast<double>(c.priority)));
            ap_cos.push_back(std::move(o));
        }
        j.set("class_offsets", std::move(ap_cos));
        JsonValue ap_cf = JsonValue::array();
        for (const int c : ap.class_filter) {
            ap_cf.push_back(JsonValue::number(static_cast<double>(c)));
        }
        j.set("class_filter", std::move(ap_cf));
        aps.push_back(std::move(j));
    }
    m.set("aim_profiles", std::move(aps));
    m.set("fov_range", JsonValue::number(static_cast<double>(mouse.fov_range)));
    m.set("confidence", JsonValue::number(static_cast<double>(mouse.confidence)));
    m.set("kp_x", JsonValue::number(static_cast<double>(mouse.kp_x)));
    m.set("kp_y", JsonValue::number(static_cast<double>(mouse.kp_y)));
    m.set("kd_x", JsonValue::number(static_cast<double>(mouse.kd_x)));
    m.set("kd_y", JsonValue::number(static_cast<double>(mouse.kd_y)));
    m.set("fov_mode", JsonValue::boolean(mouse.fov_mode));
    m.set("hfov", JsonValue::number(static_cast<double>(mouse.hfov)));
    m.set("vfov", JsonValue::number(static_cast<double>(mouse.vfov)));
    m.set("move_speed_x", JsonValue::number(static_cast<double>(mouse.move_speed_x)));
    m.set("move_speed_y", JsonValue::number(static_cast<double>(mouse.move_speed_y)));
    m.set("rate_x", JsonValue::number(static_cast<double>(mouse.rate_x)));
    m.set("rate_y", JsonValue::number(static_cast<double>(mouse.rate_y)));
    m.set("sensitivity", JsonValue::number(static_cast<double>(mouse.sensitivity)));
    m.set("output_scale", JsonValue::number(static_cast<double>(mouse.output_scale)));
    m.set("deadzone_x", JsonValue::number(static_cast<double>(mouse.deadzone_x)));
    m.set("deadzone_y", JsonValue::number(static_cast<double>(mouse.deadzone_y)));
    // 对齐参数
    m.set("predict_x", JsonValue::number(static_cast<double>(mouse.predict_x)));
    m.set("predict_y", JsonValue::number(static_cast<double>(mouse.predict_y)));
    m.set("output_deadzone", JsonValue::number(static_cast<double>(mouse.output_deadzone)));
    // 插件配置（pull_curve / recoil / personal_motion / personal_trajectory）
    JsonValue pc = JsonValue::object();
    pc.set("enabled", JsonValue::boolean(mouse.pull_curve.enabled));
    pc.set("strength", JsonValue::number(static_cast<double>(mouse.pull_curve.strength)));
    pc.set("jitter_px", JsonValue::number(static_cast<double>(mouse.pull_curve.jitter_px)));
    pc.set("min_distance", JsonValue::number(static_cast<double>(mouse.pull_curve.min_distance)));
    m.set("pull_curve", std::move(pc));
    // 持续提前量（continuous_lead）：字段名逐一对齐 yu 的 controller.continuous_lead_*，
    // 使「对标 yu」的配置可直搬、可逐字段比对（yu 默认 enabled=false 且这 6 个字段齐全）。
    JsonValue lc = JsonValue::object();
    lc.set("enabled", JsonValue::boolean(mouse.continuous_lead.enabled));
    lc.set("enter_distance", JsonValue::number(static_cast<double>(mouse.continuous_lead.enter_distance)));
    lc.set("scale", JsonValue::number(static_cast<double>(mouse.continuous_lead.scale)));
    lc.set("fade_in_ms", JsonValue::number(static_cast<double>(mouse.continuous_lead.fade_in_ms)));
    lc.set("fade_out_ms", JsonValue::number(static_cast<double>(mouse.continuous_lead.fade_out_ms)));
    lc.set("near_disable_ratio", JsonValue::number(static_cast<double>(mouse.continuous_lead.near_disable_ratio)));
    m.set("continuous_lead", std::move(lc));
    JsonValue pm = JsonValue::object();
    pm.set("enabled", JsonValue::boolean(mouse.personal_motion.enabled));
    pm.set("curve_blend", JsonValue::number(static_cast<double>(mouse.personal_motion.curve_blend)));
    // ★ speed_blend / reaction_blend / max_reaction_delay_ms 已从配置里删掉（2026-09-26）：
    //   PersonalMotion 只读 enabled / curve_blend，那三个从头到尾没人读，属死参数。
    JsonValue knots = JsonValue::array();
    for (const float knot : mouse.personal_motion.knots) {
        knots.push_back(JsonValue::number(static_cast<double>(knot)));
    }
    pm.set("knots", std::move(knots));
    m.set("personal_motion", std::move(pm));
    JsonValue pt = JsonValue::object();
    pt.set("enabled", JsonValue::boolean(mouse.personal_trajectory.enabled));
    pt.set("fitts_intercept_ms", JsonValue::number(static_cast<double>(mouse.personal_trajectory.fitts_intercept_ms)));
    pt.set("fitts_slope_ms_per_bit", JsonValue::number(static_cast<double>(mouse.personal_trajectory.fitts_slope_ms_per_bit)));
    pt.set("speed_scale", JsonValue::number(static_cast<double>(mouse.personal_trajectory.speed_scale)));
    pt.set("stability_scale", JsonValue::number(static_cast<double>(mouse.personal_trajectory.stability_scale)));
    pt.set("variation_scale", JsonValue::number(static_cast<double>(mouse.personal_trajectory.variation_scale)));
    pt.set("max_extra_px", JsonValue::number(static_cast<double>(mouse.personal_trajectory.max_extra_px)));
    pt.set("max_visual_variation_px", JsonValue::number(static_cast<double>(mouse.personal_trajectory.max_visual_variation_px)));
    pt.set("curve_time_constant_ms", JsonValue::number(static_cast<double>(mouse.personal_trajectory.curve_time_constant_ms)));
    pt.set("curve_rms_px", JsonValue::number(static_cast<double>(mouse.personal_trajectory.curve_rms_px)));
    pt.set("jitter_amp_px", JsonValue::number(static_cast<double>(mouse.personal_trajectory.jitter_amp_px)));
    pt.set("adaptive_enabled", JsonValue::boolean(mouse.personal_trajectory.adaptive_enabled));
    pt.set("min_error_px", JsonValue::number(static_cast<double>(mouse.personal_trajectory.min_error_px)));
    pt.set("urgent_error_px", JsonValue::number(static_cast<double>(mouse.personal_trajectory.urgent_error_px)));
    pt.set("urgent_speed_px_s", JsonValue::number(static_cast<double>(mouse.personal_trajectory.urgent_speed_px_s)));
    pt.set("max_target_age_ms", JsonValue::number(static_cast<double>(mouse.personal_trajectory.max_target_age_ms)));
    pt.set("capture_priority_ms", JsonValue::number(static_cast<double>(mouse.personal_trajectory.capture_priority_ms)));
    pt.set("transport_gain", JsonValue::number(static_cast<double>(mouse.personal_trajectory.transport_gain)));
    pt.set("direction_change_cosine", JsonValue::number(static_cast<double>(mouse.personal_trajectory.direction_change_cosine)));
    pt.set("response_px_per_count", JsonValue::number(static_cast<double>(mouse.personal_trajectory.response_px_per_count)));
    m.set("personal_trajectory", std::move(pt));
    JsonValue lk = JsonValue::object();
    lk.set("confirmation_frames", JsonValue::number(static_cast<double>(mouse.lock_confirm.confirmation_frames)));
    lk.set("enter_conf", JsonValue::number(static_cast<double>(mouse.lock_confirm.enter_conf)));
    lk.set("hold_conf", JsonValue::number(static_cast<double>(mouse.lock_confirm.hold_conf)));
    lk.set("instant_enter_enabled", JsonValue::boolean(mouse.lock_confirm.instant_enter_enabled));
    lk.set("instant_enter_dist", JsonValue::number(static_cast<double>(mouse.lock_confirm.instant_enter_dist)));
    lk.set("instant_enter_conf", JsonValue::number(static_cast<double>(mouse.lock_confirm.instant_enter_conf)));
    m.set("lock_confirm", std::move(lk));
    // 压枪（recoil）：2026-09-30 对照 yu 重做后的**唯一**配置面（速率 + 释放渐出 + ROI）
    JsonValue rc = JsonValue::object();
    rc.set("enabled", JsonValue::boolean(mouse.recoil.enabled));
    rc.set("hotkey", JsonValue::number(static_cast<double>(mouse.recoil.hotkey)));
    rc.set("hotkey2", JsonValue::number(static_cast<double>(mouse.recoil.hotkey2)));
    rc.set("hotkey_mode", JsonValue::number(static_cast<double>(mouse.recoil.hotkey_mode)));
    rc.set("only_when_target_visible", JsonValue::boolean(mouse.recoil.only_when_target_visible));
    rc.set("target_lost_release_ms", JsonValue::number(static_cast<double>(mouse.recoil.target_lost_release_ms)));
    rc.set("trigger_delay_enabled", JsonValue::boolean(mouse.recoil.trigger_delay_enabled));
    rc.set("trigger_delay_ms", JsonValue::number(static_cast<double>(mouse.recoil.trigger_delay_ms)));
    rc.set("strength", JsonValue::number(static_cast<double>(mouse.recoil.strength)));
    rc.set("speed", JsonValue::number(static_cast<double>(mouse.recoil.speed)));
    rc.set("curve_strength", JsonValue::number(static_cast<double>(mouse.recoil.curve_strength)));
    rc.set("roi_h", JsonValue::number(static_cast<double>(mouse.recoil.roi_h)));
    m.set("recoil", std::move(rc));
    // 自动扳机 v7.26（TriggerConfig）已于 2026-09-29 删除 ⇒ 不再序列化 trigger 段；
    // 只剩 2.0（Trigger2Config）一套。运行时状态（激活态 / 发数 / 计时器）不落盘。
    JsonValue tg2 = JsonValue::object();
    tg2.set("enabled", JsonValue::boolean(mouse.trigger2.enabled));
    tg2.set("key1", JsonValue::number(static_cast<double>(mouse.trigger2.key1)));
    tg2.set("key2", JsonValue::number(static_cast<double>(mouse.trigger2.key2)));
    tg2.set("fire_button", JsonValue::number(static_cast<double>(mouse.trigger2.fire_button)));
    tg2.set("with_aim", JsonValue::boolean(mouse.trigger2.with_aim));
    tg2.set("with_simple_recoil", JsonValue::boolean(mouse.trigger2.with_simple_recoil));
    tg2.set("confidence", JsonValue::number(static_cast<double>(mouse.trigger2.confidence)));
    tg2.set("first_err", JsonValue::number(static_cast<double>(mouse.trigger2.first_err)));
    tg2.set("first_delay", JsonValue::number(static_cast<double>(mouse.trigger2.first_delay)));
    tg2.set("fire_interval", JsonValue::number(static_cast<double>(mouse.trigger2.fire_interval)));
    tg2.set("fire_random", JsonValue::number(static_cast<double>(mouse.trigger2.fire_random)));
    tg2.set("fire_count", JsonValue::number(static_cast<double>(mouse.trigger2.fire_count)));
    tg2.set("press_duration", JsonValue::number(static_cast<double>(mouse.trigger2.press_duration)));
    tg2.set("move_throttle_frames", JsonValue::number(static_cast<double>(mouse.trigger2.move_throttle_frames)));
    tg2.set("precision_enabled", JsonValue::boolean(mouse.trigger2.precision_enabled));
    tg2.set("precision_range", JsonValue::number(static_cast<double>(mouse.trigger2.precision_range)));
    tg2.set("precision_frames", JsonValue::number(static_cast<double>(mouse.trigger2.precision_frames)));
    tg2.set("retarget_reset_ms", JsonValue::number(static_cast<double>(mouse.trigger2.retarget_reset_ms)));
    tg2.set("lite_mode", JsonValue::boolean(mouse.trigger2.lite_mode));
    tg2.set("stop_detect_enabled", JsonValue::boolean(mouse.trigger2.stop_detect_enabled));
    tg2.set("stop_detect_color_id", JsonValue::number(static_cast<double>(mouse.trigger2.stop_detect_color_id)));
    tg2.set("stop_detect_tolerance", JsonValue::number(static_cast<double>(mouse.trigger2.stop_detect_tolerance)));
    tg2.set("stop_detect_range", JsonValue::number(static_cast<double>(mouse.trigger2.stop_detect_range)));
    tg2.set("stop_detect_interval", JsonValue::number(static_cast<double>(mouse.trigger2.stop_detect_interval)));
    m.set("trigger2", std::move(tg2));
    // 热键保护（hotkey_guard）：按一次 toggle_hotkey 切换「热键挂起」。
    // 挂起状态本身是运行时状态（AimThread 成员），**不落盘**；这里只持久化配置。
    JsonValue hg = JsonValue::object();
    hg.set("enabled", JsonValue::boolean(mouse.hotkey_guard.enabled));
    hg.set("toggle_hotkey", JsonValue::number(static_cast<double>(mouse.hotkey_guard.toggle_hotkey)));
    m.set("hotkey_guard", std::move(hg));
    JsonValue ha = JsonValue::object();
    ha.set("enabled", JsonValue::boolean(mouse.aim_point.head_aim.enabled));
    ha.set("head_offset_top_fraction", JsonValue::number(static_cast<double>(mouse.aim_point.head_aim.head_offset_top_fraction)));
    ha.set("head_height_fraction", JsonValue::number(static_cast<double>(mouse.aim_point.head_aim.head_height_fraction)));
    ha.set("safe_inset_fraction", JsonValue::number(static_cast<double>(mouse.aim_point.head_aim.safe_inset_fraction)));
    ha.set("max_lag_fraction", JsonValue::number(static_cast<double>(mouse.aim_point.head_aim.max_lag_fraction)));
    ha.set("max_lag_px", JsonValue::number(static_cast<double>(mouse.aim_point.head_aim.max_lag_px)));
    m.set("head_aim", std::move(ha));
    m.set("offset_x", JsonValue::number(static_cast<double>(mouse.aim_point.offset_x)));
    m.set("offset_y", JsonValue::number(static_cast<double>(mouse.aim_point.offset_y)));
    // V1.0.08：框底被裁剪区截断时的落点外推（默认开 = 缺陷修复，可关做 A/B）
    m.set("clip_bottom_extrapolate", JsonValue::boolean(mouse.aim_point.clip_bottom_extrapolate));
    m.set("body_w_over_h", JsonValue::number(static_cast<double>(mouse.aim_point.body_w_over_h)));
    m.set("clip_bottom_margin_px", JsonValue::number(static_cast<double>(mouse.aim_point.clip_bottom_margin_px)));
    // switch_delay_ms 已删（尸体字段，见 MouseTypes.hpp 的说明）；老配置里带着会被忽略。
    m.set("lost_grace_ms", JsonValue::number(static_cast<double>(mouse.lost_grace_ms)));
    m.set("switch_hysteresis", JsonValue::number(static_cast<double>(mouse.switch_hysteresis)));
    m.set("switch_cooldown_ms", JsonValue::number(static_cast<double>(mouse.switch_cooldown_ms)));
    // V1.0.07：贴裁剪区剔除 + 锁定尺寸一致性（缺陷修复，出厂即生效，可单独关）
    m.set("reject_clip_horizontal", JsonValue::boolean(mouse.reject_clip_horizontal));
    m.set("reject_clip_top", JsonValue::boolean(mouse.reject_clip_top));
    m.set("clip_margin_px", JsonValue::number(static_cast<double>(mouse.clip_margin_px)));
    m.set("clip_center_max_px", JsonValue::number(static_cast<double>(mouse.clip_center_max_px)));
    m.set("track_size_ratio", JsonValue::number(static_cast<double>(mouse.track_size_ratio)));
    // 选靶四项机制（默认 0/false ⇒ 不开时选靶输出与加入前逐字节一致）
    m.set("lock_hold_ms", JsonValue::number(static_cast<double>(mouse.lock_hold_ms)));
    m.set("priority_scoring", JsonValue::boolean(mouse.priority_scoring));
    m.set("weight_dist", JsonValue::number(static_cast<double>(mouse.weight_dist)));
    m.set("weight_size", JsonValue::number(static_cast<double>(mouse.weight_size)));
    m.set("stickiness", JsonValue::number(static_cast<double>(mouse.stickiness)));
    m.set("switch_threshold_px", JsonValue::number(static_cast<double>(mouse.switch_threshold_px)));
    m.set("head_body_stable", JsonValue::boolean(mouse.head_body_stable));
    m.set("hb_body1", JsonValue::number(static_cast<double>(mouse.hb_body1)));
    m.set("hb_head1", JsonValue::number(static_cast<double>(mouse.hb_head1)));
    m.set("hb_body2", JsonValue::number(static_cast<double>(mouse.hb_body2)));
    m.set("hb_head2", JsonValue::number(static_cast<double>(mouse.hb_head2)));
    // V3 阶段 3a：滤波按框高自适应（默认关 ⇒ 老配置读回来仍是关）
    {
        JsonValue ba = JsonValue::object();
        ba.set("enabled", JsonValue::boolean(mouse.box_adaptive.enabled));
        ba.set("ref_box_h_px", JsonValue::number(static_cast<double>(mouse.box_adaptive.ref_box_h_px)));
        ba.set("max_cutoff_hz", JsonValue::number(static_cast<double>(mouse.box_adaptive.max_cutoff_hz)));
        ba.set("min_cutoff_hz", JsonValue::number(static_cast<double>(mouse.box_adaptive.min_cutoff_hz)));
        ba.set("box_h_ema_alpha", JsonValue::number(static_cast<double>(mouse.box_adaptive.box_h_ema_alpha)));
        m.set("box_adaptive", ba);
    }
    m.set("calibrating", JsonValue::boolean(mouse.calibrating));
    m.set("calibration_bias_x", JsonValue::number(static_cast<double>(mouse.calibration_bias_x)));
    m.set("calibration_bias_y", JsonValue::number(static_cast<double>(mouse.calibration_bias_y)));
    // 自动标定产物：游戏灵敏度（每 count 对应画面多少 px）。压枪（recoil_px_per_count）
    // 与拟人化（response_px_per_count 同语义）都依赖此值，标定后必须落盘生效。
    m.set("gain_x_px_per_count", JsonValue::number(static_cast<double>(mouse.gain_x_px_per_count)));
    m.set("gain_y_px_per_count", JsonValue::number(static_cast<double>(mouse.gain_y_px_per_count)));
    // V3 阶段 5 前置：实测回路延迟（ms）。0 = 未标定。
    m.set("response_delay_ms", JsonValue::number(static_cast<double>(mouse.response_delay_ms)));
    // V3 阶段 5：拟人化抖动前馈扣除（默认关 ⇒ 序列化出来也是关的，老配置行为不变）。
    {
        JsonValue jf = JsonValue::object();
        jf.set("enabled", JsonValue::boolean(mouse.jitter_feedforward.enabled));
        jf.set("delay_ms", JsonValue::number(static_cast<double>(mouse.jitter_feedforward.delay_ms)));
        jf.set("gain_px_per_count",
               JsonValue::number(static_cast<double>(mouse.jitter_feedforward.gain_px_per_count)));
        jf.set("scale", JsonValue::number(static_cast<double>(mouse.jitter_feedforward.scale)));
        jf.set("max_px", JsonValue::number(static_cast<double>(mouse.jitter_feedforward.max_px)));
        m.set("jitter_feedforward", std::move(jf));
    }
    JsonValue cos = JsonValue::array();
    for (const auto& c : mouse.aim_point.class_offsets) {
        JsonValue o = JsonValue::object();
        o.set("class_id", JsonValue::number(static_cast<double>(c.class_id)));
        o.set("offset_x", JsonValue::number(static_cast<double>(c.offset_x)));
        o.set("offset_y", JsonValue::number(static_cast<double>(c.offset_y)));
        o.set("priority", JsonValue::number(static_cast<double>(c.priority)));
        cos.push_back(std::move(o));
    }
    m.set("class_offsets", std::move(cos));
    root.set("mouse", std::move(m));

    JsonValue pv = JsonValue::object();
    pv.set("width", JsonValue::number(static_cast<double>(preview.width)));
    pv.set("height", JsonValue::number(static_cast<double>(preview.height)));
    pv.set("roi_w", JsonValue::number(static_cast<double>(preview.roi_w)));
    pv.set("roi_h", JsonValue::number(static_cast<double>(preview.roi_h)));
    pv.set("center_crop", JsonValue::boolean(preview.center_crop));
    pv.set("fps", JsonValue::number(static_cast<double>(preview.fps)));
    root.set("preview", std::move(pv));

    return root;
}

RuntimeProfile RuntimeProfile::from_json(const JsonValue& v) {
    RuntimeProfile p;
    if (!v.is_object()) return p;

    p.model_id = obj_str(v, "model_id", "");

    if (const JsonValue* c = v.find("capture"); c && c->is_object()) {
        // 退化值自愈：历史坏配置（如前端把 0 钳成 1 产生的 1×1）加载时纠正为 0=全帧，
        // 保证 Core 重启不会因一个坏字段而瞎跑（AI ROI 1px=推理停摆）或起不来。
        p.capture.width = sanitize_capture_roi(
            static_cast<uint32_t>(std::max<int64_t>(obj_int(*c, "width", 0), 0)));
        p.capture.height = sanitize_capture_roi(
            static_cast<uint32_t>(std::max<int64_t>(obj_int(*c, "height", 0), 0)));
        // offset 相对屏幕中心，允许负值
    }
    if (const JsonValue* i = v.find("inference"); i && i->is_object()) {
        p.inference.confidence = static_cast<float>(obj_num(*i, "confidence", 0.0));
        p.inference.iou = static_cast<float>(obj_num(*i, "iou", 0.0));
        p.inference.max_detections = static_cast<int>(obj_int(*i, "max_detections", 0));
        if (const JsonValue* cf = i->find("class_filter"); cf && cf->is_array()) {
            for (const auto& e : cf->as_array()) {
                if (e.is_number()) p.inference.class_filter.push_back(static_cast<int>(e.as_int()));
            }
        }
    }
    if (const JsonValue* gf = v.find("geometry_filter"); gf && gf->is_object()) {
        p.geometry_filter.enabled = obj_bool(*gf, "enabled", false);
        p.geometry_filter.min_head_conf = static_cast<float>(obj_num(*gf, "min_head_conf", 0.18));
        p.geometry_filter.min_body_conf = static_cast<float>(obj_num(*gf, "min_body_conf", 0.26));
        p.geometry_filter.paired_head_min_conf = static_cast<float>(obj_num(*gf, "paired_head_min_conf", 0.20));
        p.geometry_filter.head_only_min_conf = static_cast<float>(obj_num(*gf, "head_only_min_conf", 0.75));
        p.geometry_filter.head_only_center_max_px = static_cast<float>(obj_num(*gf, "head_only_center_max_px", 175.0));
        p.geometry_filter.min_body_width_px = static_cast<float>(obj_num(*gf, "min_body_width_px", 8.0));
        p.geometry_filter.min_body_height_px = static_cast<float>(obj_num(*gf, "min_body_height_px", 26.0));
        p.geometry_filter.reject_border = obj_bool(*gf, "border_reject_enabled", true);
    }
    if (const JsonValue* f = v.find("fov"); f && f->is_object()) {
        p.fov.enabled = obj_bool(*f, "enabled", false);
        p.fov.shape = (obj_int(*f, "shape", 0) == 1) ? FovShape::kRect : FovShape::kCircle;
        p.fov.radius = static_cast<float>(obj_num(*f, "radius", 0.5));
        p.fov.center_x = static_cast<float>(obj_num(*f, "center_x", 0.5));
        p.fov.center_y = static_cast<float>(obj_num(*f, "center_y", 0.5));
    }
    // A10：鼠标 AI 注入配置
    if (const JsonValue* m = v.find("mouse"); m && m->is_object()) {
        p.mouse.enabled = obj_bool(*m, "enabled", false);
        // 平铺 aim_hotkey / aim_hotkey2 / aim_hotkey_mode 不再在这里读 ——
        // 热键的唯一真源是 mouse.aim_profiles（见本段末尾的档位解析，老配置在那里合成第 0 档）。
        p.mouse.fov_range = static_cast<float>(obj_num(*m, "fov_range", 1.0));
        p.mouse.confidence = static_cast<float>(obj_num(*m, "confidence", 0.25));
        // ★ V1.0.13（2026-09-30）：kp/kd 语义改为真实有效值，smooth 从参数面删除。
        //   老配置（带 smooth_x/smooth_y）必须**原样折算**，否则 kp 会突然放大 100 倍：
        //     kp_eff = kp * (10000 - smooth) / 10000
        //   判据用"键是否存在"而不是值 —— 新配置压根不写 smooth，用值判会把默认 9900
        //   当成"要削 99%"再削一次。
        //   为什么这么折是等价的：pid1 的 smoothTerm 对小量就是 (v/10000)*(10000-smooth)，
        //   即纯乘法；离线闭环（core/tools/pid_sim/aim_replay.py）实测移动靶偏差 ≤0.23%、
        //   静止靶绝对差 0.26px（< 半个 count 量化级）。
        {
            const JsonValue* smx = m->find("smooth_x");
            const JsonValue* smy = m->find("smooth_y");
            const bool has_smx = smx && smx->is_number();
            const bool has_smy = smy && smy->is_number();
            const float kp_dflt_x = has_smx ? 25.0f : 0.25f;
            const float kp_dflt_y = has_smy ? 25.0f : 0.25f;
            const float kd_dflt_x = has_smx ? 25.0f : 0.25f;
            const float kd_dflt_y = has_smy ? 25.0f : 0.25f;
            p.mouse.kp_x = static_cast<float>(obj_num(*m, "kp_x", kp_dflt_x));
            p.mouse.kp_y = static_cast<float>(obj_num(*m, "kp_y", kp_dflt_y));
            p.mouse.kd_x = static_cast<float>(obj_num(*m, "kd_x", kd_dflt_x));
            p.mouse.kd_y = static_cast<float>(obj_num(*m, "kd_y", kd_dflt_y));
            if (has_smx) {
                const float fac = (10000.0f - static_cast<float>(obj_num(*m, "smooth_x", 9900.0))) / 10000.0f;
                p.mouse.kp_x *= fac;
                p.mouse.kd_x *= fac;
            }
            if (has_smy) {
                const float fac = (10000.0f - static_cast<float>(obj_num(*m, "smooth_y", 9900.0))) / 10000.0f;
                p.mouse.kp_y *= fac;
                p.mouse.kd_y *= fac;
            }
        }
        p.mouse.fov_mode = obj_bool(*m, "fov_mode", false);
        p.mouse.hfov = static_cast<float>(obj_num(*m, "hfov", 83.105));
        p.mouse.vfov = static_cast<float>(obj_num(*m, "vfov", 53.0));
        p.mouse.move_speed_x = static_cast<float>(obj_num(*m, "move_speed_x", 500.0));
        p.mouse.move_speed_y = static_cast<float>(obj_num(*m, "move_speed_y", 500.0));
        p.mouse.rate_x = static_cast<float>(obj_num(*m, "rate_x", 0.3));
                p.mouse.rate_y = static_cast<float>(obj_num(*m, "rate_y", 0.3));
        p.mouse.sensitivity = static_cast<float>(obj_num(*m, "sensitivity", 1.0));
        p.mouse.output_scale = static_cast<float>(obj_num(*m, "output_scale", 1.0));
        p.mouse.deadzone_x = static_cast<float>(obj_num(*m, "deadzone_x", 1.0));
        p.mouse.deadzone_y = static_cast<float>(obj_num(*m, "deadzone_y", 1.0));
        // 对齐参数
        p.mouse.predict_x = static_cast<float>(obj_num(*m, "predict_x", 1.0));
                p.mouse.predict_y = static_cast<float>(obj_num(*m, "predict_y", 0.0));
        // V1.0.13：smooth_x/smooth_y 已删（上面折算进 kp/kd）。旧键不再读取、也不回写。
        p.mouse.output_deadzone = static_cast<float>(obj_num(*m, "output_deadzone", 1.0));
    // 插件配置（pull_curve / recoil / personal_motion / personal_trajectory）
        if (const JsonValue* pc = m->find("pull_curve"); pc && pc->is_object()) {
            p.mouse.pull_curve.enabled = obj_bool(*pc, "enabled", true);
            p.mouse.pull_curve.strength = static_cast<float>(obj_num(*pc, "strength", 0.8));
            p.mouse.pull_curve.jitter_px = static_cast<float>(obj_num(*pc, "jitter_px", 3.0));
            p.mouse.pull_curve.min_distance = static_cast<float>(obj_num(*pc, "min_distance", 80.0));
        }
        // 持续提前量：缺字段一律取"保守默认"（enabled=false ⇒ 不动输出），
        // 故旧配置/旧预设文件加载后行为与本功能加入前完全一致（向后兼容）。
        if (const JsonValue* lc = m->find("continuous_lead"); lc && lc->is_object()) {
            p.mouse.continuous_lead.enabled = obj_bool(*lc, "enabled", false);
            p.mouse.continuous_lead.enter_distance = static_cast<float>(obj_num(*lc, "enter_distance", 150.0));
            p.mouse.continuous_lead.scale = static_cast<float>(obj_num(*lc, "scale", 0.5));
            p.mouse.continuous_lead.fade_in_ms = static_cast<float>(obj_num(*lc, "fade_in_ms", 300.0));
            p.mouse.continuous_lead.fade_out_ms = static_cast<float>(obj_num(*lc, "fade_out_ms", 300.0));
            p.mouse.continuous_lead.near_disable_ratio = static_cast<float>(obj_num(*lc, "near_disable_ratio", 0.66));
        }
        if (const JsonValue* pm = m->find("personal_motion"); pm && pm->is_object()) {
            p.mouse.personal_motion.enabled = obj_bool(*pm, "enabled", false);
            p.mouse.personal_motion.curve_blend = static_cast<float>(obj_num(*pm, "curve_blend", 1.0));
            // speed_blend / reaction_blend / max_reaction_delay_ms 已删（core 从不读）；
            // 老配置里带着这几个键也无妨 —— 反序列化只认在用的键，多余的被忽略。
            if (const JsonValue* knots = pm->find("knots"); knots && knots->is_array()) {
                for (const auto& item : knots->as_array()) {
                    if (item.is_number() && p.mouse.personal_motion.knots.size() < 32) {
                        p.mouse.personal_motion.knots.push_back(static_cast<float>(item.as_number()));
                    }
                }
            }
        }
        if (const JsonValue* pt = m->find("personal_trajectory"); pt && pt->is_object()) {
            p.mouse.personal_trajectory.enabled = obj_bool(*pt, "enabled", false);
            p.mouse.personal_trajectory.fitts_intercept_ms = static_cast<float>(obj_num(*pt, "fitts_intercept_ms", 120.0));
            p.mouse.personal_trajectory.fitts_slope_ms_per_bit = static_cast<float>(obj_num(*pt, "fitts_slope_ms_per_bit", 85.0));
            p.mouse.personal_trajectory.speed_scale = static_cast<float>(obj_num(*pt, "speed_scale", 1.0));
            p.mouse.personal_trajectory.stability_scale = static_cast<float>(obj_num(*pt, "stability_scale", 1.0));
            p.mouse.personal_trajectory.variation_scale = static_cast<float>(obj_num(*pt, "variation_scale", 1.0));
            p.mouse.personal_trajectory.max_extra_px = static_cast<float>(obj_num(*pt, "max_extra_px", 2.0));
            p.mouse.personal_trajectory.max_visual_variation_px = static_cast<float>(obj_num(*pt, "max_visual_variation_px", 1.5));
            p.mouse.personal_trajectory.curve_time_constant_ms = static_cast<float>(obj_num(*pt, "curve_time_constant_ms", 32.0));
            p.mouse.personal_trajectory.curve_rms_px = static_cast<float>(obj_num(*pt, "curve_rms_px", 0.8));
            p.mouse.personal_trajectory.jitter_amp_px = static_cast<float>(obj_num(*pt, "jitter_amp_px", 0.20));
            p.mouse.personal_trajectory.adaptive_enabled = obj_bool(*pt, "adaptive_enabled", true);
            p.mouse.personal_trajectory.min_error_px = static_cast<float>(obj_num(*pt, "min_error_px", 18.0));
            p.mouse.personal_trajectory.urgent_error_px = static_cast<float>(obj_num(*pt, "urgent_error_px", 72.0));
            p.mouse.personal_trajectory.urgent_speed_px_s = static_cast<float>(obj_num(*pt, "urgent_speed_px_s", 520.0));
            p.mouse.personal_trajectory.max_target_age_ms = static_cast<float>(obj_num(*pt, "max_target_age_ms", 18.0));
            p.mouse.personal_trajectory.capture_priority_ms = static_cast<float>(obj_num(*pt, "capture_priority_ms", 5.0));
            p.mouse.personal_trajectory.transport_gain = static_cast<float>(obj_num(*pt, "transport_gain", 0.16));
            p.mouse.personal_trajectory.direction_change_cosine = static_cast<float>(obj_num(*pt, "direction_change_cosine", 0.15));
            p.mouse.personal_trajectory.response_px_per_count = static_cast<float>(obj_num(*pt, "response_px_per_count", 0.65));
        }
        if (const JsonValue* lk = m->find("lock_confirm"); lk && lk->is_object()) {
            p.mouse.lock_confirm.confirmation_frames = static_cast<int>(obj_int(*lk, "confirmation_frames", 1));
            p.mouse.lock_confirm.enter_conf = static_cast<float>(obj_num(*lk, "enter_conf", 0.0));
            p.mouse.lock_confirm.hold_conf = static_cast<float>(obj_num(*lk, "hold_conf", 0.0));
            p.mouse.lock_confirm.instant_enter_enabled = obj_bool(*lk, "instant_enter_enabled", true);
            p.mouse.lock_confirm.instant_enter_dist = static_cast<float>(obj_num(*lk, "instant_enter_dist", 105.0));
            p.mouse.lock_confirm.instant_enter_conf = static_cast<float>(obj_num(*lk, "instant_enter_conf", 0.50));
        }
        // 压枪（recoil）解析：缺失字段用默认值（全部关/零输出，保持旧行为）
        if (const JsonValue* rk = m->find("recoil"); rk && rk->is_object()) {
            p.mouse.recoil.enabled = obj_bool(*rk, "enabled", false);
            p.mouse.recoil.hotkey = static_cast<int>(obj_int(*rk, "hotkey", 1));
            p.mouse.recoil.hotkey2 = static_cast<int>(obj_int(*rk, "hotkey2", 0));
            p.mouse.recoil.hotkey_mode = static_cast<int>(obj_int(*rk, "hotkey_mode", 1));
            p.mouse.recoil.only_when_target_visible = obj_bool(*rk, "only_when_target_visible", true);
            p.mouse.recoil.target_lost_release_ms = static_cast<float>(obj_num(*rk, "target_lost_release_ms", 300.0));
            p.mouse.recoil.trigger_delay_enabled = obj_bool(*rk, "trigger_delay_enabled", false);
            p.mouse.recoil.trigger_delay_ms = static_cast<float>(obj_num(*rk, "trigger_delay_ms", 120.0));
            p.mouse.recoil.strength = static_cast<float>(obj_num(*rk, "strength", 100.0));
            p.mouse.recoil.speed = static_cast<float>(obj_num(*rk, "speed", 1.0));
            p.mouse.recoil.curve_strength = static_cast<float>(obj_num(*rk, "curve_strength", 0.6));
            p.mouse.recoil.roi_h = static_cast<float>(obj_num(*rk, "roi_h", 300.0));
        }
        // 自动扳机 v7.26（TriggerConfig）已于 2026-09-29 删除 ⇒ 旧配置里的 `trigger` 段
        // 直接忽略，只解析 2.0（`trigger2`）。键位只取低 5 位（左1 右2 中4 侧8 侧16）。
        if (const JsonValue* tg = m->find("trigger2"); tg && tg->is_object()) {
            p.mouse.trigger2.enabled = obj_bool(*tg, "enabled", false);
            p.mouse.trigger2.key1 = static_cast<uint8_t>(obj_int(*tg, "key1", 16) & 0x1F);
            p.mouse.trigger2.key2 = static_cast<uint8_t>(obj_int(*tg, "key2", 0) & 0x1F);
            p.mouse.trigger2.fire_button = static_cast<uint8_t>(obj_int(*tg, "fire_button", 1) & 0x1F);
            p.mouse.trigger2.with_aim = obj_bool(*tg, "with_aim", true);
            p.mouse.trigger2.with_simple_recoil = obj_bool(*tg, "with_simple_recoil", false);
            p.mouse.trigger2.confidence = static_cast<float>(obj_num(*tg, "confidence", 0.5));
            p.mouse.trigger2.first_err = static_cast<float>(obj_num(*tg, "first_err", 30.0));
            p.mouse.trigger2.first_delay = static_cast<float>(obj_num(*tg, "first_delay", 0.0));
            p.mouse.trigger2.fire_interval = static_cast<float>(obj_num(*tg, "fire_interval", 1.0));
            p.mouse.trigger2.fire_random = static_cast<float>(obj_num(*tg, "fire_random", 0.0));
            p.mouse.trigger2.fire_count = static_cast<int>(obj_int(*tg, "fire_count", 1));
            p.mouse.trigger2.press_duration = static_cast<float>(obj_num(*tg, "press_duration", 50.0));
            p.mouse.trigger2.move_throttle_frames = static_cast<int>(obj_int(*tg, "move_throttle_frames", 2));
            p.mouse.trigger2.precision_enabled = obj_bool(*tg, "precision_enabled", false);
            p.mouse.trigger2.precision_range = static_cast<float>(obj_num(*tg, "precision_range", 10.0));
            p.mouse.trigger2.precision_frames = static_cast<int>(obj_int(*tg, "precision_frames", 5));
            p.mouse.trigger2.retarget_reset_ms = static_cast<float>(obj_num(*tg, "retarget_reset_ms", 1000.0));
            p.mouse.trigger2.lite_mode = obj_bool(*tg, "lite_mode", false);
            p.mouse.trigger2.stop_detect_enabled = obj_bool(*tg, "stop_detect_enabled", false);
            p.mouse.trigger2.stop_detect_color_id = static_cast<int>(obj_int(*tg, "stop_detect_color_id", 2));
            p.mouse.trigger2.stop_detect_tolerance = static_cast<int>(obj_int(*tg, "stop_detect_tolerance", 60));
            p.mouse.trigger2.stop_detect_range = static_cast<int>(obj_int(*tg, "stop_detect_range", 80));
            p.mouse.trigger2.stop_detect_interval = static_cast<int>(obj_int(*tg, "stop_detect_interval", 10));
        }
        // 热键保护（hotkey_guard）解析：缺字段一律取"保守默认"（enabled=false ⇒ 不翻转、位图原样透传），
        // 故旧配置/旧预设文件加载后行为与本功能加入前完全一致（向后兼容）。
        // toggle_hotkey 只取鼠标五键位图（左1 右2 中4 侧8 侧16）的合法子集，
        // 越界/负数一律夹成 0（永不翻转）而不是掩成 31 —— 掩成 31 会让**任意键**都能翻转保护。
        if (const JsonValue* hg = m->find("hotkey_guard"); hg && hg->is_object()) {
            p.mouse.hotkey_guard.enabled = obj_bool(*hg, "enabled", false);
            p.mouse.hotkey_guard.toggle_hotkey =
                sanitize_hotkey_bits(obj_int(*hg, "toggle_hotkey", 4));
        }
        if (const JsonValue* ha = m->find("head_aim"); ha && ha->is_object()) {
            auto obj_num2 = [&](const char* k, double d) {
                const JsonValue* v = ha->find(k);
                return (v && v->is_number()) ? v->as_number() : d;
            };
            const JsonValue* en = ha->find("enabled");
            p.mouse.aim_point.head_aim.enabled = (en && en->is_bool()) ? en->as_bool(false) : false;
            p.mouse.aim_point.head_aim.head_offset_top_fraction = static_cast<float>(obj_num2("head_offset_top_fraction", 0.04));
            p.mouse.aim_point.head_aim.head_height_fraction = static_cast<float>(obj_num2("head_height_fraction", 0.28));
            p.mouse.aim_point.head_aim.safe_inset_fraction = static_cast<float>(obj_num2("safe_inset_fraction", 0.12));
            p.mouse.aim_point.head_aim.max_lag_fraction = static_cast<float>(obj_num2("max_lag_fraction", 0.18));
            p.mouse.aim_point.head_aim.max_lag_px = static_cast<float>(obj_num2("max_lag_px", 1.25));
        }
        p.mouse.aim_point.offset_x = static_cast<float>(obj_num(*m, "offset_x", 0.5));
        p.mouse.aim_point.offset_y = static_cast<float>(obj_num(*m, "offset_y", 0.5));
        p.mouse.aim_point.clip_bottom_extrapolate = obj_bool(*m, "clip_bottom_extrapolate", true);
        p.mouse.aim_point.body_w_over_h = static_cast<float>(obj_num(*m, "body_w_over_h", 0.32));
        p.mouse.aim_point.clip_bottom_margin_px =
            static_cast<float>(obj_num(*m, "clip_bottom_margin_px", 12.0));
        p.mouse.lost_grace_ms = static_cast<float>(obj_num(*m, "lost_grace_ms", 78.0));
        p.mouse.switch_hysteresis = static_cast<float>(obj_num(*m, "switch_hysteresis", 0.5));
        // 选靶四项机制（默认 0/false，见 MouseProfile 注释）
        p.mouse.lock_hold_ms = static_cast<float>(obj_num(*m, "lock_hold_ms", 0.0));
        p.mouse.priority_scoring = obj_bool(*m, "priority_scoring", false);
        p.mouse.weight_dist = static_cast<float>(obj_num(*m, "weight_dist", 1.0));
        p.mouse.weight_size = static_cast<float>(obj_num(*m, "weight_size", 0.3));
        p.mouse.stickiness = static_cast<float>(obj_num(*m, "stickiness", 1.0));
        p.mouse.switch_threshold_px = static_cast<float>(obj_num(*m, "switch_threshold_px", 60.0));
        p.mouse.head_body_stable = obj_bool(*m, "head_body_stable", false);
        p.mouse.hb_body1 = static_cast<int>(obj_int(*m, "hb_body1", 0));
        p.mouse.hb_head1 = static_cast<int>(obj_int(*m, "hb_head1", 1));
        p.mouse.hb_body2 = static_cast<int>(obj_int(*m, "hb_body2", -1));
        p.mouse.hb_head2 = static_cast<int>(obj_int(*m, "hb_head2", -1));
        // V3 阶段 3a：滤波按框高自适应（缺键 ⇒ 全吃默认 = enabled false，行为不变）
        if (const JsonValue* ba = m->find("box_adaptive"); ba && ba->is_object()) {
            p.mouse.box_adaptive.enabled = obj_bool(*ba, "enabled", false);
            p.mouse.box_adaptive.ref_box_h_px =
                static_cast<float>(obj_num(*ba, "ref_box_h_px", 100.0));
            p.mouse.box_adaptive.max_cutoff_hz =
                static_cast<float>(obj_num(*ba, "max_cutoff_hz", 0.8));
            p.mouse.box_adaptive.min_cutoff_hz =
                static_cast<float>(obj_num(*ba, "min_cutoff_hz", 0.15));
            p.mouse.box_adaptive.box_h_ema_alpha =
                static_cast<float>(obj_num(*ba, "box_h_ema_alpha", 0.10));
        }
        p.mouse.switch_cooldown_ms = static_cast<float>(obj_num(*m, "switch_cooldown_ms", 600.0));
        // V1.0.07：贴裁剪区剔除 + 锁定尺寸一致性。缺键 ⇒ 吃结构体默认（修复默认开）。
        p.mouse.reject_clip_horizontal = obj_bool(*m, "reject_clip_horizontal", true);
        p.mouse.reject_clip_top = obj_bool(*m, "reject_clip_top", false);
        p.mouse.clip_margin_px = static_cast<float>(obj_num(*m, "clip_margin_px", 6.0));
        p.mouse.clip_center_max_px = static_cast<float>(obj_num(*m, "clip_center_max_px", 105.0));
        // V1.0.11：默认 2.0 → 1.35（2.0 太宽，实测 92.3% 的坏量测从这过）
        p.mouse.track_size_ratio = static_cast<float>(obj_num(*m, "track_size_ratio", 1.35));
        p.mouse.calibrating = obj_bool(*m, "calibrating", false);
        p.mouse.calibration_bias_x = static_cast<float>(obj_num(*m, "calibration_bias_x", 0.0));
        p.mouse.calibration_bias_y = static_cast<float>(obj_num(*m, "calibration_bias_y", 0.0));
        p.mouse.gain_x_px_per_count = static_cast<float>(obj_num(*m, "gain_x_px_per_count", 0.65));
        p.mouse.gain_y_px_per_count = static_cast<float>(obj_num(*m, "gain_y_px_per_count", 0.65));
        // V3 阶段 5 前置：实测回路延迟（ms）。老配置没有这个键 ⇒ 0（未标定）。
        p.mouse.response_delay_ms = static_cast<float>(obj_num(*m, "response_delay_ms", 0.0));
        // V3 阶段 5：抖动前馈扣除。老配置没有这个键 ⇒ 默认关（enabled=false）。
        if (const JsonValue* jf = m->find("jitter_feedforward"); jf && jf->is_object()) {
            auto& c = p.mouse.jitter_feedforward;
            c.enabled = obj_bool(*jf, "enabled", false);
            c.delay_ms = static_cast<float>(obj_num(*jf, "delay_ms", 0.0));
            c.gain_px_per_count = static_cast<float>(obj_num(*jf, "gain_px_per_count", 0.0));
            c.scale = static_cast<float>(obj_num(*jf, "scale", 1.0));
            c.max_px = static_cast<float>(obj_num(*jf, "max_px", 40.0));
        }
        if (const JsonValue* co = m->find("class_offsets"); co && co->is_array()) {
            for (const auto& e : co->as_array()) {
                if (!e.is_object()) continue;
                aim::ClassOffset c;
                c.class_id = static_cast<int>(obj_int(e, "class_id", 0));
                c.offset_x = static_cast<float>(obj_num(e, "offset_x", 0.5));
                c.offset_y = static_cast<float>(obj_num(e, "offset_y", 0.5));
                c.priority = static_cast<int>(obj_int(e, "priority", 0));
                p.mouse.aim_point.class_offsets.push_back(c);
            }
        }
        // ---- 瞄准档位（2026-09-24）：热键的唯一真源 ----
        // JSON 里有非空数组 ⇒ 逐档读；没有（老配置，或旧版 Core 写回的配置）⇒
        // 用平铺 key 合成第 0 档。**不做 config.d 迁移**：OTA 不覆盖 config.d，
        // 合成这条路保证老设备升级后行为逐位不变。
        bool got_profiles = false;
        if (const JsonValue* aps = m->find("aim_profiles"); aps && aps->is_array() && !aps->as_array().empty()) {
            p.mouse.aim_profiles.clear();
            for (const auto& j : aps->as_array()) {
                if (!j.is_object()) continue;
                aim::AimHotkeyProfile ap;
                ap.hotkey = sanitize_hotkey_bits(obj_int(j, "hotkey", 2));
                ap.hotkey2 = sanitize_hotkey_bits(obj_int(j, "hotkey2", 0));
                ap.hotkey_mode = aim::mouse_hotkey_mode_from_string(obj_str(j, "hotkey_mode", "any").c_str());
                ap.offset_x = static_cast<float>(obj_num(j, "offset_x", 0.5));
                ap.offset_y = static_cast<float>(obj_num(j, "offset_y", 0.5));
                ap.sensitivity = static_cast<float>(obj_num(j, "sensitivity", 1.0));
                ap.fov_scale = static_cast<float>(obj_num(j, "fov_scale", 1.0));
                // V1.0.12（2026-09-30）：本档 zoom_scale / gain_px_per_count 已删（不区分倍镜）。
                // 旧配置里出现这两个键一律**静默忽略** —— 等价 1.0 / 0，与删除前行为一致。
                if (const JsonValue* co = j.find("class_offsets"); co && co->is_array()) {
                    for (const auto& e : co->as_array()) {
                        if (!e.is_object()) continue;
                        aim::ClassOffset c;
                        c.class_id = static_cast<int>(obj_int(e, "class_id", 0));
                        c.offset_x = static_cast<float>(obj_num(e, "offset_x", 0.5));
                        c.offset_y = static_cast<float>(obj_num(e, "offset_y", 0.5));
                        c.priority = static_cast<int>(obj_int(e, "priority", 0));
                        ap.class_offsets.push_back(c);
                    }
                }
                if (const JsonValue* cf = j.find("class_filter"); cf && cf->is_array()) {
                    for (const auto& e : cf->as_array()) {
                        if (!e.is_number()) continue;
                        ap.class_filter.push_back(static_cast<int>(e.as_number()));
                    }
                }
                p.mouse.aim_profiles.push_back(std::move(ap));
            }
            got_profiles = !p.mouse.aim_profiles.empty();
        }
        if (!got_profiles) {
            // 老配置回退：平铺 key → 第 0 档。
            // class_offsets 留空 = 沿用 mouse.aim_point 的全局类别偏移表；
            // sensitivity / fov_scale 取结构体默认 1.0 = 不影响全局量 ⇒ 与老代码等价。
            // 偏移直接取上面已解析的全局瞄准点，保证"平铺 offset_x/offset_y"只有一个入口。
            aim::AimHotkeyProfile ap;
            ap.hotkey = sanitize_hotkey_bits(obj_int(*m, "aim_hotkey", 2));
            ap.hotkey2 = sanitize_hotkey_bits(obj_int(*m, "aim_hotkey2", 0));
            ap.hotkey_mode =
                aim::mouse_hotkey_mode_from_string(obj_str(*m, "aim_hotkey_mode", "any").c_str());
            ap.offset_x = p.mouse.aim_point.offset_x;
            ap.offset_y = p.mouse.aim_point.offset_y;
            const std::vector<int>& global_cf = p.inference.class_filter;
            ap.class_filter = global_cf;  // 全局类别过滤 → 第 0 档，单档语义不变
            p.mouse.aim_profiles.assign(1, std::move(ap));
        }
    }
    if (const JsonValue* pv = v.find("preview"); pv && pv->is_object()) {
        p.preview.width = static_cast<uint32_t>(std::max<int64_t>(obj_int(*pv, "width", 640), 1));
        p.preview.height = static_cast<uint32_t>(std::max<int64_t>(obj_int(*pv, "height", 640), 1));
        p.preview.roi_w = static_cast<uint32_t>(std::max<int64_t>(obj_int(*pv, "roi_w", 640), 1));
        p.preview.roi_h = static_cast<uint32_t>(std::max<int64_t>(obj_int(*pv, "roi_h", 640), 1));
        p.preview.center_crop = obj_bool(*pv, "center_crop", true);
        p.preview.fps = static_cast<uint32_t>(std::max<int64_t>(obj_int(*pv, "fps", 0), 0));
    }
    // P-ZC-1：缺 video 段时保持默认值（crop=0 沿用全局、zero_copy_input=true）。
    // 注意 zero_copy_input 的默认是 true：这一项本就是为了让已装机设备
    // （全局配置里 rknn_external_dma_input:false 且 OTA 覆盖不到）能在面板打开零拷贝。
    // 但"拿来即用"的前提是 profile 里**真的有**这个键 —— 老配置没有 video 段时，
    // 仍以全局配置为准，见 Application::apply_video_profile 的三态处理。
    p.video.zero_copy_input_set = false;
    if (const JsonValue* vd = v.find("video"); vd && vd->is_object()) {
        p.video.crop_width = static_cast<uint32_t>(std::max<int64_t>(obj_int(*vd, "crop_width", 0), 0));
        p.video.crop_height = static_cast<uint32_t>(std::max<int64_t>(obj_int(*vd, "crop_height", 0), 0));
        if (vd->find("zero_copy_input")) {
            p.video.zero_copy_input = obj_bool(*vd, "zero_copy_input", true);
            p.video.zero_copy_input_set = true;
        }
    }
    return p;
}

RuntimeProfile RuntimeProfile::from_json_file(const std::string& path,
                                              std::string* error) {
    auto res = json_parse_file(path);
    if (!res.ok) {
        if (error) *error = "解析失败(" + path + "): " + res.error;
        return {};
    }
    return from_json(res.value);
}

}  // namespace ttbox::core
