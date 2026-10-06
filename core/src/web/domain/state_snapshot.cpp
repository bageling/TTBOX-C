// state_snapshot.cpp — /api/state 状态快照装配（collect_web_state）。
//
// 自 plugins/web/lib/state_snapshot.py 逐行为移植。GET_STATUS(metrics) +
// GET_CONFIG(profile) + MODEL_LIST(models) 三源投影装配。
// fan/loopout/calibration 三个 T04/T05 硬件与标定子块的诚实默认投影在
// domain_payloads.hpp，真实现分别下沉 hardware / calibration 域。
#include "web/domain/domain_routes.hpp"

#include <ctime>
#include <string>
#include <utility>

#include "common/Json.hpp"
#include "web/domain/domain_internal.hpp"
#include "web/domain/domain_payloads.hpp"
#include "web/infra/ipc_client.hpp"
#include "web/translate/profile_translate.hpp"

namespace ttbox::core::web {

namespace {

JsonValue point2(double x, double y) {
    JsonValue p = JsonValue::object();
    p.set("x", JsonValue::number(x));
    p.set("y", JsonValue::number(y));
    return p;
}

}  // namespace

JsonValue collect_web_state(IpcClient& ipc) {
    const JsonValue st = get_status_data(ipc);
    JsonValue prof;
    std::string err;
    if (!get_runtime_profile(ipc, &prof, &err)) {
        return JsonValue::null();  // core 离线哨兵 → 调用方映射 503
    }

    JsonValue ml_data;
    {
        const JsonValue ml = ipc.call("MODEL_LIST", JsonValue::object(), kIpcTimeoutDefaultMs);
        const JsonValue* d = (ipc_status(ml) == 0) ? ml.find("data") : nullptr;
        ml_data = (d != nullptr && d->is_object()) ? *d : JsonValue::object();
    }
    // 真源统一：registry active 覆盖 profile.model_id（防旧缓存回写跳回）。
    const std::string registry_active = json_field(ml_data, "active").as_string("");
    if (!registry_active.empty()) prof.set("model_id", JsonValue::string(registry_active));
    const std::string active_model =
        !registry_active.empty() ? registry_active : json_field(prof, "model_id").as_string("");

    const JsonValue m = json_field(st, "metrics");
    const bool running = json_field(st, "running").as_bool(false) &&
                         json_field(st, "runtime_running").as_bool(false);
    const double capture_fps = mnum(m, "capture_fps");
    const int64_t in_w = mint(m, "input_width");
    const int64_t in_h = mint(m, "input_height");
    const bool degraded = running && (capture_fps <= 0.0 || in_w <= 0 || in_h <= 0);
    const std::string runtime_status = degraded ? "degraded" : (running ? "running" : "stopped");
    const std::string runtime_error =
        !mstr(m, "last_error").empty() ? mstr(m, "last_error")
                                       : (degraded ? "HDMI 输入未锁定或尚未收到帧" : "");
    const std::string no_model_msg =
        json_field(prof, "model_id").as_string("").empty() ? "未导入模型" : "";
    const std::string last_err = !runtime_error.empty() ? runtime_error : no_model_msg;

    const JsonValue lic = license_block(ipc);
    const std::string brand = json_field(lic, "ui_brand").as_string("ttbox");
    const JsonValue ui = ui_block(brand);
    const std::string ota_ver = ota_current_version(ipc);
    const std::string st_ver = json_field(st, "version").as_string("");
    const std::string app_ver = !ota_ver.empty() ? ota_ver : (!st_ver.empty() ? st_ver : kAppVersion);
    const std::string mc = machine_code(ipc);

    // ---- state 子块 ----
    JsonValue aim = JsonValue::object();
    aim.set("active", JsonValue::boolean(mbool(m, "aim_active")));
    aim.set("active_hotkey", JsonValue::string(""));
    aim.set("active_target_track_id",
            JsonValue::number(static_cast<double>(mint(m, "aim_target_id"))));
    JsonValue alt = JsonValue::array();
    alt.push_back(JsonValue::boolean(false));
    aim.set("aim_profile_alternate_offset_states", std::move(alt));
    aim.set("hotkeys_suspended", JsonValue::boolean(mbool(m, "aim_hotkeys_suspended")));
    aim.set("last_error", JsonValue::string(last_err));
    aim.set("locked", JsonValue::boolean(false));

    JsonValue capture = JsonValue::object();
    capture.set("input_width", JsonValue::number(static_cast<double>(in_w)));
    capture.set("input_height", JsonValue::number(static_cast<double>(in_h)));
    capture.set("capture_fps", JsonValue::number(capture_fps));
    capture.set("buffer_age_ms", JsonValue::number(mnum(m, "buffer_age_ms")));
    capture.set("last_dequeued_count",
                JsonValue::number(static_cast<double>(mint(m, "last_dequeued_count"))));
    capture.set("buffer_count", JsonValue::number(static_cast<double>(mint(m, "buffer_count"))));

    JsonValue preview = JsonValue::object();
    preview.set("fps", JsonValue::number(mnum(m, "preview_fps")));
    preview.set("encode_ms", JsonValue::number(mnum(m, "preview_encode_ms")));
    preview.set("width", JsonValue::number(static_cast<double>(mint(m, "preview_width"))));
    preview.set("height", JsonValue::number(static_cast<double>(mint(m, "preview_height"))));
    preview.set("bytes", JsonValue::number(static_cast<double>(mint(m, "preview_bytes"))));
    preview.set("frames", JsonValue::number(static_cast<double>(mint(m, "preview_frames"))));
    preview.set("dropped", JsonValue::number(static_cast<double>(mint(m, "preview_dropped"))));
    preview.set("watermark", JsonValue::boolean(mbool(m, "preview_watermark")));

    JsonValue crosshair = JsonValue::object();
    crosshair.set("color_index", JsonValue::number(-1.0));
    crosshair.set("component_area", JsonValue::number(0.0));
    JsonValue bbox = JsonValue::object();
    bbox.set("height", JsonValue::number(0.0));
    bbox.set("width", JsonValue::number(0.0));
    bbox.set("x", JsonValue::number(0.0));
    bbox.set("y", JsonValue::number(0.0));
    crosshair.set("component_bbox", std::move(bbox));
    crosshair.set("enabled", JsonValue::boolean(false));
    crosshair.set("preset_color", JsonValue::string(""));
    crosshair.set("score", JsonValue::number(0.0));
    crosshair.set("valid", JsonValue::boolean(false));
    crosshair.set("x", JsonValue::number(0.0));
    crosshair.set("y", JsonValue::number(0.0));

    JsonValue detection = JsonValue::object();
    detection.set("detections", JsonValue::number(static_cast<double>(mint(m, "detect_count"))));
    detection.set("tracks", JsonValue::number(static_cast<double>(mint(m, "tracks"))));
    detection.set("inference_fps", JsonValue::number(mnum(m, "fps")));
    detection.set("inference_ms", JsonValue::number(mnum(m, "infer_ms")));
    detection.set("model_loaded",
                  JsonValue::boolean(!json_field(prof, "model_id").as_string("").empty()));
    detection.set("frame_id", JsonValue::number(static_cast<double>(mint(m, "last_frame"))));
    detection.set("timestamp_us",
                  JsonValue::number(static_cast<double>(mint(m, "last_timestamp_us"))));
    if (mbool(m, "aim_has_target")) {
        JsonValue tb = JsonValue::object();
        tb.set("x1", JsonValue::number(mnum(m, "aim_target_x1")));
        tb.set("y1", JsonValue::number(mnum(m, "aim_target_y1")));
        tb.set("x2", JsonValue::number(mnum(m, "aim_target_x2")));
        tb.set("y2", JsonValue::number(mnum(m, "aim_target_y2")));
        tb.set("class_id", JsonValue::number(static_cast<double>(mint(m, "aim_target_class_id"))));
        tb.set("target_id", JsonValue::number(static_cast<double>(mint(m, "aim_target_id"))));
        detection.set("target_box", std::move(tb));
    } else {
        detection.set("target_box", JsonValue::null());
    }
    detection.set("boxes", json_field(m, "detection_boxes").is_array()
                               ? json_field(m, "detection_boxes")
                               : JsonValue::array());

    JsonValue latency = JsonValue::object();
    latency.set("capture_to_mouse_send_ms", JsonValue::number(mnum(m, "e2e_ms")));
    latency.set("preprocess_to_track_ms",
                JsonValue::number(mnum(m, "resize_ms") + mnum(m, "infer_run_ms") +
                                  mnum(m, "decode_ms")));
    latency.set("raw_preprocess_backend", JsonValue::string(mstr(m, "raw_preprocess_backend")));
    latency.set("raw_preprocess_error", JsonValue::string(mstr(m, "raw_preprocess_error")));
    latency.set("queue_wait_ms", JsonValue::number(mnum(m, "buffer_age_ms")));
    latency.set("rga_ms", JsonValue::number(mnum(m, "resize_ms")));
    latency.set("rknn_set_input_ms", JsonValue::number(mnum(m, "infer_set_input_ms")));
    latency.set("rknn_ms", JsonValue::number(mnum(m, "infer_run_ms")));
    latency.set("rknn_output_ms", JsonValue::number(mnum(m, "infer_output_ms")));
    latency.set("decode_ms", JsonValue::number(mnum(m, "decode_ms")));
    latency.set("e2e_ms", JsonValue::number(mnum(m, "e2e_ms")));
    latency.set("e2e_p95_ms", JsonValue::number(mnum(m, "e2e_p95_ms")));
    latency.set("e2e_p99_ms", JsonValue::number(mnum(m, "e2e_p99_ms")));

    JsonValue model_input = JsonValue::object();
    model_input.set("pass_mode", JsonValue::string(mstr(m, "model_input_pass_mode")));
    model_input.set("fast_path_active", JsonValue::boolean(mbool(m, "model_fast_path_active")));
    model_input.set("zero_copy_ready", JsonValue::boolean(mbool(m, "model_zero_copy_ready")));
    model_input.set("tensor_type", JsonValue::string(mstr(m, "model_input_type_name")));
    model_input.set("tensor_format", JsonValue::string(mstr(m, "model_input_fmt_name")));
    model_input.set("quant_type", JsonValue::string(mstr(m, "model_input_qnt_name")));
    model_input.set("zero_point", JsonValue::number(static_cast<double>(mint(m, "model_input_zp"))));
    model_input.set("scale", JsonValue::number(mnum(m, "model_input_scale")));
    model_input.set("model_width",
                    JsonValue::number(static_cast<double>(mint(m, "model_input_width"))));
    model_input.set("model_height",
                    JsonValue::number(static_cast<double>(mint(m, "model_input_height"))));
    model_input.set("external_dma_requested",
                    JsonValue::boolean(mbool(m, "model_external_dma_requested")));
    model_input.set("external_dma_bound", JsonValue::boolean(mbool(m, "model_external_dma_bound")));
    model_input.set("workers_total",
                    JsonValue::number(static_cast<double>(mint(m, "model_workers_total"))));
    model_input.set("workers_zero_copy",
                    JsonValue::number(static_cast<double>(mint(m, "model_workers_zero_copy"))));
    model_input.set("workers_fast_path",
                    JsonValue::number(static_cast<double>(mint(m, "model_workers_fast_path"))));
    model_input.set("note", JsonValue::string(mstr(m, "model_input_note")));

    JsonValue motion = JsonValue::object();
    motion.set("collection_active", JsonValue::boolean(false));
    motion.set("lease_remaining_ms", JsonValue::number(0.0));
    motion.set("model_error", JsonValue::string(""));
    motion.set("model_loaded", JsonValue::boolean(false));
    motion.set("model_quality", JsonValue::number(0.0));
    motion.set("model_status", JsonValue::string("disabled"));
    motion.set("profile_id", JsonValue::string(""));
    motion.set("session_id", JsonValue::string(""));

    JsonValue trace = JsonValue::object();
    trace.set("target_point", point2(mnum(m, "target_point_x"), mnum(m, "target_point_y")));
    trace.set("reference", point2(mnum(m, "reference_x"), mnum(m, "reference_y")));
    trace.set("error", point2(mnum(m, "aim_error_x"), mnum(m, "aim_error_y")));
    trace.set("control_y", JsonValue::number(mnum(m, "aim_control_y")));
    trace.set("pid_output", point2(mnum(m, "pid_output_x"), mnum(m, "pid_output_y")));
    trace.set("scheduler_input",
              point2(mnum(m, "scheduler_input_x"), mnum(m, "scheduler_input_y")));
    JsonValue recoil = JsonValue::object();
    recoil.set("add_y", JsonValue::number(mnum(m, "recoil_add_y")));
    recoil.set("acc_px", JsonValue::number(mnum(m, "recoil_acc_px")));
    recoil.set("rate_px_s", JsonValue::number(mnum(m, "recoil_rate_px_s")));
    trace.set("recoil", std::move(recoil));
    trace.set("hid_move", point2(static_cast<double>(mint(m, "mouse_dx")),
                                 static_cast<double>(mint(m, "mouse_dy"))));
    trace.set("injection_allowed", JsonValue::boolean(mbool(m, "injection_allowed")));
    trace.set("mouse_control_connected", JsonValue::boolean(mbool(m, "mouse_control_connected")));
    trace.set("mouse_control_socket_write_ok",
              JsonValue::number(static_cast<double>(mint(m, "mouse_control_socket_write_ok"))));
    trace.set("mouse_control_socket_write_fail",
              JsonValue::number(static_cast<double>(mint(m, "mouse_control_socket_write_fail"))));
    trace.set("mouse_control_send_count",
              JsonValue::number(static_cast<double>(mint(m, "mouse_control_send_count"))));
    trace.set("last_mouse_control_dx",
              JsonValue::number(static_cast<double>(mint(m, "last_mouse_control_dx"))));
    trace.set("last_mouse_control_dy",
              JsonValue::number(static_cast<double>(mint(m, "last_mouse_control_dy"))));
    trace.set("last_mouse_control_wheel",
              JsonValue::number(static_cast<double>(mint(m, "last_mouse_control_wheel"))));
    trace.set("last_mouse_control_timestamp_us",
              JsonValue::number(static_cast<double>(mint(m, "last_mouse_control_timestamp_us"))));

    // 物理移动屏蔽掩码：真实来源 = RuntimeProfile mouse 配置。
    const JsonValue& prof_mouse = json_field(prof, "mouse");
    int block_mask = 0;
    if (json_truthy(json_field(prof_mouse, "block_physical_x"))) block_mask |= 1;
    if (json_truthy(json_field(prof_mouse, "block_physical_y"))) block_mask |= 2;
    JsonValue mouse_output = JsonValue::object();
    mouse_output.set("mode", JsonValue::string("full_passthrough"));
    mouse_output.set("physical_motion_block_support", JsonValue::string("supported"));
    mouse_output.set("physical_motion_block_mask",
                     JsonValue::number(static_cast<double>(block_mask)));
    mouse_output.set("physical_motion_block_error", JsonValue::string(""));

    JsonValue state = JsonValue::object();
    state.set("aim", std::move(aim));
    state.set("last_error", JsonValue::string(last_err));
    state.set("calibration", calibration_runtime());
    state.set("capture", std::move(capture));
    state.set("preview", std::move(preview));
    state.set("core", core_state_payload());
    state.set("crosshair", std::move(crosshair));
    state.set("fan_control", fan_control_payload());
    state.set("detection", std::move(detection));
    state.set("latency", std::move(latency));
    state.set("loopout", loopout_payload());
    state.set("model_input", std::move(model_input));
    state.set("motion_training", std::move(motion));
    state.set("updated_at_ms",
              JsonValue::number(static_cast<double>(std::time(nullptr) * 1000LL)));
    state.set("control_trace", std::move(trace));
    state.set("license", lic);
    state.set("cloud", cloud_license_subblock(mc));
    state.set("mouse_output", std::move(mouse_output));
    state.set("preview_path", JsonValue::string("/api/preview.mjpg"));
    state.set("running", JsonValue::boolean(running && !degraded));
    state.set("selected_model_id", JsonValue::string(json_field(prof, "model_id").as_string("")));
    state.set("status", JsonValue::string(runtime_status));

    // ---- 顶层 data ----
    JsonValue data = JsonValue::object();
    data.set("app_version", JsonValue::string(app_ver));
    data.set("version", JsonValue::string(app_ver));
    data.set("config", profile_to_web(prof));
    data.set("auto_start", auto_start_payload());
    JsonValue preview_alive = JsonValue::object();
    preview_alive.set("alive", JsonValue::boolean(false));  // T04 preview 域接管
    preview_alive.set("active_conns", JsonValue::number(0.0));
    data.set("preview", std::move(preview_alive));
    data.set("models", models_view(ipc, ml_data));
    data.set("selected_model_id", JsonValue::string(active_model));
    data.set("presets", preset_names());
    data.set("state", std::move(state));
    data.set("ui", ui);
    data.set("ui_brand", JsonValue::string(json_field(ui, "ui_brand").as_string("ttbox")));

    JsonValue envelope = JsonValue::object();
    envelope.set("ok", JsonValue::boolean(true));
    envelope.set("data", std::move(data));
    return envelope;
}

}  // namespace ttbox::core::web
