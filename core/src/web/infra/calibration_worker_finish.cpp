// calibration_worker_finish.cpp — 标定线程收尾（拟合 + 写回 + finally 恢复）。
//
// 自 plugins/web/lib/calibration.py::_calib_worker 逐行移植（因超 300 行拆出）。
#include "web/infra/calibration_controller.hpp"

#include <algorithm>
#include <string>

#include "web/domain/domain_internal.hpp"

namespace ttbox::core::web {

// 收尾：拟合 X/Y 轴 → 组装标定留档 → 写盘并写回 gain/PID → 落终态。
void CalibrationController::worker_finish(WorkerContext& ctx) {
    {
        std::lock_guard<std::mutex> lk(state_mutex());
        state_.state = "validating";
        state_.phase = "validating";
        state_.current_axis = "";
        state_.progress = 0.9;
    }
    const AxisFit fx = fit_axis_measurements(ctx.obs[0], CalibrationAxis::kX);
    const AxisFit fy = fit_axis_measurements(ctx.obs[1], CalibrationAxis::kY);
    auto fit_obj = [](const AxisFit& f) {
        JsonValue o = JsonValue::object();
        o.set("gain_px_per_count", JsonValue::number(f.gain_px_per_count));
        o.set("response_delay_ms", JsonValue::number(f.response_delay_ms));
        o.set("sample_count", JsonValue::number(static_cast<double>(f.sample_count)));
        o.set("rejected_count", JsonValue::number(static_cast<double>(f.rejected_count)));
        o.set("consistency", JsonValue::number(f.consistency));
        o.set("converged", JsonValue::boolean(f.converged));
        o.set("failure_reason", JsonValue::string(f.failure_reason));
        return o;
    };
    JsonValue fits = JsonValue::object();
    fits.set("x", fit_obj(fx));
    fits.set("y", fit_obj(fy));
    {
        std::lock_guard<std::mutex> lk(state_mutex());
        state_.axis_fits = fits;
    }

    if (!(fx.converged && fy.converged)) {
        std::string reason;
        if (!fx.converged) reason = fx.failure_reason;
        if (!fy.converged) {
            if (!reason.empty()) reason += "; ";
            reason += fy.failure_reason;
        }
        fail_state(reason.empty() ? "轴向拟合失败" : reason);
        return;
    }

    const double gain_x = fx.gain_px_per_count;
    const double gain_y = fy.gain_px_per_count;
    const double delay_ms = std::max(fx.response_delay_ms, fy.response_delay_ms);
    const double conf = round_to(std::min(fx.consistency, fy.consistency), 3);
    {
        std::lock_guard<std::mutex> lk(state_mutex());
        state_.round_gains = {gain_x, gain_y};
        state_.progress = 0.98;
        state_.phase = "saving";
        state_.state = "applying";
    }

    JsonValue calib = JsonValue::object();
    calib.set("mouse_gain_x_px_per_count", JsonValue::number(round_to(gain_x, 4)));
    calib.set("mouse_gain_y_px_per_count", JsonValue::number(round_to(gain_y, 4)));
    calib.set("mouse_response_delay_ms", JsonValue::number(round_to(delay_ms, 2)));
    calib.set("mouse_calibration_applied", JsonValue::boolean(true));
    calib.set("valid", JsonValue::boolean(true));
    calib.set("confidence", JsonValue::number(conf));
    calib.set("calibrated_at", JsonValue::string(timestamp_name()));
    calib.set("model_id", JsonValue::string(read_active_model()));
    {
        int64_t crop = 320;
        JsonValue prof;
        std::string err;
        if (get_runtime_profile(ipc_, &prof, &err)) {
            crop = json_field(json_field(prof, "preview"), "roi_w").as_int(320);
            if (crop == 0) crop = 320;  // 对齐 Python `or 320`
        }
        JsonValue capture = JsonValue::object();
        capture.set("crop_size", JsonValue::number(static_cast<double>(crop)));
        calib.set("capture", std::move(capture));
    }
    calib.set("rounds",
              JsonValue::number(static_cast<double>(ctx.obs[0].size() + ctx.obs[1].size())));
    calib.set("max_tracked_amp_x", JsonValue::number(round_to(ctx.max_tracked_amps[0], 2)));
    calib.set("max_tracked_amp_y", JsonValue::number(round_to(ctx.max_tracked_amps[1], 2)));
    calib.set("max_tracked_amp",
              JsonValue::number(
                  round_to(std::min(ctx.max_tracked_amps[0], ctx.max_tracked_amps[1]), 2)));
    try {
        const PidParams pid = derive_pid_params(gain_x, gain_y, delay_ms);
        JsonValue po = JsonValue::object();
        po.set("kp", JsonValue::number(pid.kp));
        po.set("kd", JsonValue::number(pid.kd));
        po.set("predict", JsonValue::number(pid.predict));
        calib.set("pid_params", std::move(po));
    } catch (...) {
        calib.set("pid_params", JsonValue::object());
    }

    std::string detail;
    bool ok = write_calibration(calib, &detail);
    if (ok) {
        const auto [ok2, detail2] = apply_gain(calib);
        detail += "；" + detail2;
        ok = ok && ok2;
        if (ok2 && json_field(calib, "pid_params").is_object() &&
            !json_field(calib, "pid_params").as_object().empty()) {
            const auto [ok3, detail3] = apply_pid(calib);
            detail += "；" + detail3;
            ok = ok && ok3;
        }
    }
    {
        std::lock_guard<std::mutex> lk(state_mutex());
        state_.state = ok ? "completed" : "failed";
        state_.status = ok ? "success" : "failed";
        state_.reason = ok ? "completed" : detail;
        state_.ready = ok;
        state_.progress = ok ? 1.0 : 0.98;
        state_.phase = ok ? "completed" : "error";
    }
}

// finally 恢复段（持配置锁）：清 calibrating/偏置；终态失败时回滚用户 KP/KD，并还原 enabled。
void CalibrationController::worker_cleanup(WorkerContext& ctx) {
    std::lock_guard<std::recursive_mutex> lk(config_write_lock());
    try {
        JsonValue prof;
        std::string err;
        if (!get_runtime_profile(ipc_, &prof, &err)) return;
        JsonValue mo = mouse_of(prof);
        mo.set("calibrating", JsonValue::boolean(false));
        mo.set("calibration_bias_x", JsonValue::number(0.0));
        mo.set("calibration_bias_y", JsonValue::number(0.0));
        bool terminal = false;
        {
            std::lock_guard<std::mutex> slk(state_mutex());
            terminal = (state_.state == "failed" || state_.state == "cancelled");
        }
        if (terminal) {
            if (ctx.has_saved_kp) {
                mo.set("kp_x", JsonValue::number(ctx.saved_kp));
                mo.set("kp_y", JsonValue::number(ctx.saved_kp));
            }
            if (ctx.has_saved_kd) {
                mo.set("kd_x", JsonValue::number(ctx.saved_kd));
                mo.set("kd_y", JsonValue::number(ctx.saved_kd));
            }
        }
        if (!ctx.was_enabled) mo.set("enabled", JsonValue::boolean(false));
        prof.set("mouse", std::move(mo));
        set_config(prof);
    } catch (...) {
    }
}

}  // namespace ttbox::core::web
