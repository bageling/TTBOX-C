// calibration_api.cpp — 标定 HTTP 编排面（payload / start / cancel / clear / manual_update）。
//
// 自 plugins/web/lib/calibration.py::_calibration_payload 与 api/calib.py 逐行移植。
#include "web/infra/calibration_controller.hpp"

#include <string>
#include <thread>

#include "web/domain/domain_internal.hpp"

namespace ttbox::core::web {

namespace {

// 从 record 按 ':' 分段的 prefix 取成员；缺省返回 null（对齐 Python _rec）。
JsonValue rec_field(const JsonValue& record, const std::string& key, const std::string& prefix) {
    const JsonValue* node = &record;
    if (!prefix.empty()) {
        std::string cur;
        for (char c : prefix) {
            if (c == ':') {
                node = node->is_object() ? node->find(cur) : nullptr;
                if (node == nullptr) return JsonValue::null();
                cur.clear();
            } else {
                cur.push_back(c);
            }
        }
        if (!cur.empty()) {
            node = node->is_object() ? node->find(cur) : nullptr;
            if (node == nullptr) return JsonValue::null();
        }
    }
    const JsonValue* v = node->is_object() ? node->find(key) : nullptr;
    return v != nullptr ? *v : JsonValue::null();
}

// 带默认值的 rec_field。
JsonValue rec_or(const JsonValue& record, const std::string& key, const std::string& prefix,
                 const JsonValue& def) {
    const JsonValue v = rec_field(record, key, prefix);
    return v.is_null() ? def : v;
}

}  // namespace

// /api/control/calibration GET：组装运行态（runtime）+ 留档 + 生效值（calibration）投影。
JsonValue CalibrationController::payload() {
    JsonValue runtime;
    {
        std::lock_guard<std::mutex> lk(state_mutex());
        const CalibrationRuntimeState& s = state_;
        runtime = JsonValue::object();
        runtime.set("running", JsonValue::boolean(thread_running_.load()));
        runtime.set("phase", JsonValue::string(s.phase));
        runtime.set("state", JsonValue::string(s.state));
        runtime.set("status", JsonValue::string(s.status));
        runtime.set("ready", JsonValue::boolean(s.ready));
        runtime.set("reason", JsonValue::string(s.reason));
        runtime.set("total_rounds", JsonValue::number(static_cast<double>(s.total_rounds)));
        runtime.set("round", JsonValue::number(static_cast<double>(s.round)));
        runtime.set("progress", JsonValue::number(s.progress));
        runtime.set("current_axis", JsonValue::string(s.current_axis));
        runtime.set("valid_sample_count",
                    JsonValue::number(static_cast<double>(s.valid_sample_count)));
        runtime.set("axis_fits", s.axis_fits);
        runtime.set("candidate_count", JsonValue::number(static_cast<double>(s.candidate_count)));
        runtime.set("candidate_track_id", JsonValue::number(static_cast<double>(s.candidate_track_id)));
        runtime.set("candidate_class_id", JsonValue::number(static_cast<double>(s.candidate_class_id)));
        runtime.set("candidate_width", JsonValue::number(s.candidate_width));
        runtime.set("candidate_height", JsonValue::number(s.candidate_height));
        JsonValue rect = JsonValue::object();
        rect.set("x", JsonValue::number(0.0));
        rect.set("y", JsonValue::number(0.0));
        rect.set("width", JsonValue::number(static_cast<double>(static_cast<int>(s.candidate_width))));
        rect.set("height", JsonValue::number(static_cast<double>(static_cast<int>(s.candidate_height))));
        runtime.set("candidate_rect", std::move(rect));
        runtime.set("stable_frames", JsonValue::number(static_cast<double>(s.stable_frames)));
        runtime.set("stable_ms", JsonValue::number(static_cast<double>(s.stable_ms)));
        runtime.set("center_jitter_px", JsonValue::number(s.center_jitter_px));
        runtime.set("size_variation", JsonValue::number(s.size_variation));
        runtime.set("elapsed_ms", JsonValue::number(static_cast<double>(s.elapsed_ms)));
        runtime.set("amplitude_counts", JsonValue::number(static_cast<double>(s.amplitude_counts)));
        runtime.set("amplitude_px", JsonValue::number(s.amplitude_px));
        runtime.set("settle_ms", JsonValue::number(static_cast<double>(s.settle_ms)));
        runtime.set("settled", JsonValue::boolean(s.settled));
        runtime.set("dropped_sample_count",
                    JsonValue::number(static_cast<double>(s.dropped_sample_count)));
        runtime.set("max_tracked_amp", JsonValue::number(s.max_tracked_amp));
        runtime.set("error", JsonValue::string(s.status == "failed" ? s.reason : ""));
    }

    JsonValue record;
    const bool has_record = read_calibration(&record);

    // 生效值（core 当前在用的 gain/pid）—— 读不到则留空，绝不拿留档冒充。
    JsonValue eff = JsonValue::object();
    {
        JsonValue prof;
        std::string err;
        if (get_runtime_profile(ipc_, &prof, &err)) {
            const JsonValue emo = json_field(prof, "mouse");
            const char* keys[] = {"gain_x_px_per_count", "gain_y_px_per_count",
                                  "kp_x", "kd_x", "predict_x"};
            for (const char* k : keys) {
                const JsonValue* v = emo.find(k);
                if (v != nullptr && v->is_number()) {
                    eff.set(k, JsonValue::number(round_to(v->as_number(0.0), 4)));
                }
            }
        }
    }

    const JsonValue kZero = JsonValue::number(0.0);
    const JsonValue kEmpty = JsonValue::string("");
    JsonValue calib = JsonValue::object();
    calib.set("valid", JsonValue::boolean(has_record ? json_truthy(json_field(record, "valid"))
                                                     : false));
    const JsonValue gain_x_eff = json_field(eff, "gain_x_px_per_count");
    calib.set("gain_x_px_per_count",
              gain_x_eff.is_number() ? gain_x_eff
                                     : rec_or(record, "mouse_gain_x_px_per_count", "", JsonValue::null()));
    const JsonValue gain_y_eff = json_field(eff, "gain_y_px_per_count");
    calib.set("gain_y_px_per_count",
              gain_y_eff.is_number() ? gain_y_eff
                                     : rec_or(record, "mouse_gain_y_px_per_count", "", JsonValue::null()));
    calib.set("response_delay_ms",
              rec_or(record, "mouse_response_delay_ms", "", JsonValue::null()));
    calib.set("confidence", rec_or(record, "confidence", "", kZero));
    calib.set("model_id", rec_or(record, "model_id", "", kEmpty));
    calib.set("calibrated_at", rec_or(record, "calibrated_at", "", kEmpty));
    const JsonValue crop = rec_or(record, "crop_size", "capture", kZero);
    calib.set("capture_width", crop);
    calib.set("capture_height", crop);
    calib.set("crop_size", crop);
    const JsonValue pid = rec_or(record, "pid_params", "", JsonValue::null());
    calib.set("pid_params", pid.is_object() ? pid : JsonValue::object());
    calib.set("effective", std::move(eff));

    JsonValue out = JsonValue::object();
    out.set("runtime", std::move(runtime));
    out.set("calibration", std::move(calib));
    return out;
}

// 启动标定：查重（409）/推理未运行或无目标（400），通过则起后台线程返回 200。
int CalibrationController::start(std::string* error) {
    if (thread_running_.load()) {
        if (error != nullptr) *error = "标定已在运行中";
        return 409;
    }
    const JsonValue st = get_status_data(ipc_);
    if (!json_truthy(json_field(st, "runtime_running"))) {
        if (error != nullptr)
            *error = "推理服务未运行或目标反馈未就绪（请先启动推理）";
        return 400;
    }
    if (sample_target().is_null()) {
        if (error != nullptr)
            *error = "未识别到目标，无法开始标定（请将准星对准画面中的目标，等待检测框稳定出现）";
        return 400;
    }
    thread_running_.store(true);
    std::thread(&CalibrationController::thread_entry, this).detach();
    return 200;
}

// 取消标定：置 cancelled 状态并清 mouse.calibrating（worker 会自行感知并收尾）。
void CalibrationController::cancel() {
    {
        std::lock_guard<std::mutex> lk(state_mutex());
        state_.state = "cancelled";
        state_.status = "idle";
        state_.phase = "cancelled";
        state_.ready = false;
        state_.reason = "cancelled";
    }
    try {
        // ── 配置读-改-写持 config_write_lock：防与面板写入整份覆盖 ──
        std::lock_guard<std::recursive_mutex> lk(config_write_lock());
        JsonValue prof;
        std::string err;
        if (get_runtime_profile(ipc_, &prof, &err)) {
            JsonValue mo = json_field(prof, "mouse").is_object() ? json_field(prof, "mouse")
                                                                 : JsonValue::object();
            mo.set("calibrating", JsonValue::boolean(false));
            prof.set("mouse", std::move(mo));
            JsonValue params = JsonValue::object();
            params.set("profile", prof);
            ipc_.call("SET_CONFIG", params, kIpcTimeoutDefaultMs);
        }
    } catch (...) {
    }
}

// 清除标定留档文件。
void CalibrationController::clear() { clear_calibration_file(); }

// 手动填入 gain/延迟：写留档 → 写回 gain → 联动推导并写回 PID → 落 manual 状态 → 回 payload。
JsonValue CalibrationController::manual_update(double gain_x, double gain_y, double delay,
                                              bool* ok, std::string* detail) {
    JsonValue calib = JsonValue::object();
    calib.set("mouse_gain_x_px_per_count", JsonValue::number(round_to(gain_x, 4)));
    calib.set("mouse_gain_y_px_per_count", JsonValue::number(round_to(gain_y, 4)));
    calib.set("mouse_response_delay_ms", JsonValue::number(round_to(delay, 3)));
    calib.set("mouse_calibration_applied", JsonValue::boolean(true));
    calib.set("valid", JsonValue::boolean(true));
    calib.set("confidence", JsonValue::number(0.0));
    calib.set("calibrated_at", JsonValue::string(timestamp_name()));
    calib.set("model_id", JsonValue::string(read_active_model()));

    bool write_ok_local = write_calibration(calib, detail);
    bool apply_ok = false;
    if (write_ok_local) {
        auto [ok2, detail2] = apply_gain(calib);
        apply_ok = ok2;
        *detail = *detail + "；" + detail2;
        // 手动填增益同样联动自动调参（同一推导函数，保证行为一致）。
        try {
            const PidParams pid = derive_pid(gain_x, gain_y, delay);
            JsonValue pid_obj = JsonValue::object();
            pid_obj.set("kp", JsonValue::number(pid.kp));
            pid_obj.set("kd", JsonValue::number(pid.kd));
            pid_obj.set("predict", JsonValue::number(pid.predict));
            calib.set("pid_params", std::move(pid_obj));
        } catch (...) {
            calib.set("pid_params", JsonValue::object());
        }
        if (apply_ok && json_field(calib, "pid_params").is_object() &&
            !json_field(calib, "pid_params").as_object().empty()) {
            auto [ok3, detail3] = apply_pid(calib);
            *detail = *detail + "；" + detail3;
            write_ok_local = write_ok_local && ok3;
            write_calibration(calib, detail);
        }
        {
            std::lock_guard<std::mutex> lk(state_mutex());
            if (write_ok_local) {
                state_.status = "manual";
                state_.phase = "done";
                state_.ready = true;
                state_.reason = "completed";
            } else {
                state_.status = "manual";
                state_.phase = "saved";
                state_.ready = false;
                state_.reason = *detail;
            }
        }
    }
    if (ok != nullptr) *ok = write_ok_local;
    return payload();
}

}  // namespace ttbox::core::web
