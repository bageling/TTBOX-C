// calibration_worker.cpp — 标定线程体（前置段 + 稳定检测）。
//
// 自 plugins/web/lib/calibration.py::_calib_worker 逐行移植（拆半）。
// 采样/拟合/写回在 calibration_worker_sample.cpp。
#include "web/infra/calibration_controller.hpp"

#include <chrono>
#include <stdexcept>
#include <thread>

#include "web/domain/domain_internal.hpp"

namespace ttbox::core::web {

// 判定当前是否仍在 running 阶段（持 state 锁读状态）。
bool CalibrationController::is_running() {
    std::lock_guard<std::mutex> lk(state_mutex());
    return state_.status == "running";
}

// 把标定状态落到 failed 终态并记录原因。
void CalibrationController::fail_state(const std::string& reason) {
    std::lock_guard<std::mutex> lk(state_mutex());
    state_.state = "failed";
    state_.status = "failed";
    state_.phase = "error";
    state_.ready = false;
    state_.reason = reason;
}

// 把标定状态落到 cancelled 终态。
void CalibrationController::cancel_state() {
    std::lock_guard<std::mutex> lk(state_mutex());
    state_.state = "cancelled";
    state_.phase = "cancelled";
    state_.reason = "cancelled";
}

// 标定线程入口：跑 worker，异常一律落 failed，最后清运行标志。
void CalibrationController::thread_entry() {
    try {
        worker();
    } catch (const std::exception& e) {
        fail_state("标定异常：" + std::string(e.what()));
    } catch (...) {
        fail_state("标定异常：未知错误");
    }
    thread_running_.store(false);
}

// 标定主流程：前置 → 稳定检测 → 分轴采样 → 收尾，无论成败都走 cleanup 恢复现场。
void CalibrationController::worker() {
    WorkerContext ctx;
    worker_prepare(ctx);  // 前置段抛异常由 thread_entry 兜底（对齐 Python 结构）
    try {
        if (worker_stabilize(ctx) && worker_sample(ctx)) {
            worker_finish(ctx);
        }
    } catch (const std::exception& e) {
        fail_state("标定异常：" + std::string(e.what()));
    } catch (...) {
        fail_state("标定异常：未知错误");
    }
    worker_cleanup(ctx);  // finally 恢复段
}

// 前置段（持配置锁）：备份用户 KP/KD，切到温和标定档并清零偏置，首次 SET_CONFIG 失败即抛。
bool CalibrationController::worker_prepare(WorkerContext& ctx) {
    std::lock_guard<std::recursive_mutex> lk(config_write_lock());
    JsonValue prof;
    std::string err;
    if (!get_runtime_profile(ipc_, &prof, &err)) {
        throw std::runtime_error("标定前置 SET_CONFIG 失败：Core 离线");
    }
    const JsonValue& mouse = json_field(prof, "mouse");
    ctx.was_enabled = json_truthy(json_field(mouse, "enabled"));

    JsonValue mo = mouse_of(prof);
    mo.set("enabled", JsonValue::boolean(true));
    mo.set("calibrating", JsonValue::boolean(true));
    mo.set("calibration_bias_x", JsonValue::number(0.0));
    mo.set("calibration_bias_y", JsonValue::number(0.0));

    const JsonValue* kp_v = mo.find("kp_x");
    const JsonValue* kd_v = mo.find("kd_x");
    ctx.has_saved_kp = (kp_v != nullptr && !kp_v->is_null());
    ctx.has_saved_kd = (kd_v != nullptr && !kd_v->is_null());
    if (ctx.has_saved_kp) ctx.saved_kp = kp_v->as_number(0.0);
    if (ctx.has_saved_kd) ctx.saved_kd = kd_v->as_number(0.0);

    // calib_kp = min(float(saved_kp), CALIB_PID_KP_MAX)；saved_kp 缺失/非数 ⇒ 取上限。
    double calib_kp = kCalibPidKpMax;
    if (ctx.has_saved_kp && kp_v->is_number()) {
        calib_kp = std::min(kp_v->as_number(0.0), kCalibPidKpMax);
    }
    ctx.calib_kp = calib_kp;
    mo.set("kp_x", JsonValue::number(calib_kp));
    mo.set("kp_y", JsonValue::number(calib_kp));
    mo.set("kd_x", JsonValue::number(calib_kp * kCalibPidKdRatio));
    mo.set("kd_y", JsonValue::number(calib_kp * kCalibPidKdRatio));
    prof.set("mouse", std::move(mo));

    // 首次 SET_CONFIG 必须查结果：失败还继续 = 全程用实战 KP 采数据。
    const JsonValue r0 = set_config(prof);
    if (ipc_status(r0) != 0) {
        throw std::runtime_error("标定前置 SET_CONFIG 失败：" +
                                 json_field(r0, "error").as_string("未知原因"));
    }
    return true;
}

// 稳定检测：12s 内要求目标连续静止（抖动/尺寸变化低于阈值）并保持 800ms。
bool CalibrationController::worker_stabilize(WorkerContext& /*ctx*/) {
    {
        std::lock_guard<std::mutex> lk(state_mutex());
        state_.state = "preparing";
        state_.status = "running";
        state_.phase = "preparing";
        state_.reason = "准备标定环境";
        state_.round = 0;
        state_.progress = 0.0;
        state_.round_gains.clear();
        state_.candidate_count = 0;
        state_.stable_frames = 0;
        state_.stable_ms = 0;
        state_.valid_sample_count = 0;
        state_.axis_fits = JsonValue::object();
        state_.amplitude_px = 0.0;
        state_.amplitude_counts = 0;
        state_.settle_ms = 0;
        state_.settled = false;
        state_.dropped_sample_count = 0;
    }
    {
        std::lock_guard<std::mutex> lk(state_mutex());
        state_.state = "stabilize_x";
        state_.phase = "stabilize_x";
        state_.current_axis = "x";
    }

    std::vector<JsonValue> win;
    bool stable_start_set = false;
    auto stable_start = std::chrono::steady_clock::now();
    const auto t0 = std::chrono::steady_clock::now();
    const auto deadline = t0 + std::chrono::milliseconds(12000);
    bool stabilized = false;

    while (std::chrono::steady_clock::now() < deadline) {
        if (!is_running()) {
            cancel_state();
            return false;
        }
        JsonValue target = sample_target();
        if (target.is_null()) {
            win.clear();
            stable_start_set = false;
            std::lock_guard<std::mutex> lk(state_mutex());
            state_.reason = "no_target";
            state_.candidate_count = 0;
            state_.stable_frames = 0;
            state_.stable_ms = 0;
            state_.elapsed_ms = static_cast<int>(steady_ms_since(t0));
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            continue;
        }
        if (!win.empty() &&
            (json_field(target, "target_id").as_int(-1) !=
                 json_field(win.back(), "target_id").as_int(-1) ||
             json_field(target, "class_id").as_int(-1) !=
                 json_field(win.back(), "class_id").as_int(-1))) {
            win.clear();
            stable_start_set = false;
        }
        win.push_back(std::move(target));
        if (win.size() > 10) win.erase(win.begin());

        double min_x = 1e18, max_x = -1e18, min_y = 1e18, max_y = -1e18;
        double min_w = 1e18, max_w = -1e18, min_h = 1e18, max_h = -1e18;
        double all_max = 0.0;
        for (const JsonValue& item : win) {
            const double x = json_field(item, "x").as_number(0.0);
            const double y = json_field(item, "y").as_number(0.0);
            const double w = json_field(item, "width").as_number(0.0);
            const double h = json_field(item, "height").as_number(0.0);
            min_x = std::min(min_x, x);
            max_x = std::max(max_x, x);
            min_y = std::min(min_y, y);
            max_y = std::max(max_y, y);
            min_w = std::min(min_w, w);
            max_w = std::max(max_w, w);
            min_h = std::min(min_h, h);
            max_h = std::max(max_h, h);
            all_max = std::max(all_max, std::max(w, h));
        }
        const double jx = max_x - min_x;
        const double jy = max_y - min_y;
        const double size_var = std::max(max_w - min_w, max_h - min_h) / std::max(all_max, 1.0);

        const JsonValue& last = win.back();
        {
            std::lock_guard<std::mutex> lk(state_mutex());
            state_.candidate_count = static_cast<int>(win.size());
            state_.candidate_track_id = json_field(last, "target_id").as_int(-1);
            state_.candidate_class_id = json_field(last, "class_id").as_int(-1);
            state_.candidate_width = json_field(last, "width").as_number(0.0);
            state_.candidate_height = json_field(last, "height").as_number(0.0);
            state_.center_jitter_px = std::max(jx, jy);
            state_.size_variation = size_var;
            state_.stable_frames = static_cast<int>(win.size());
        }

        if (win.size() >= 10 && jx < 1.0 && jy < 1.0 && size_var < 0.05) {
            if (!stable_start_set) {
                stable_start = std::chrono::steady_clock::now();
                stable_start_set = true;
            }
            const int stable_ms = static_cast<int>(steady_ms_since(stable_start));
            {
                std::lock_guard<std::mutex> lk(state_mutex());
                state_.state = "stabilize_x";
                state_.stable_ms = stable_ms;
                state_.ready = true;
                state_.reason = "ready";
                state_.elapsed_ms = static_cast<int>(steady_ms_since(t0));
            }
            if (stable_ms >= 800) {
                stabilized = true;
                break;
            }
        } else {
            stable_start_set = false;
            std::lock_guard<std::mutex> lk(state_mutex());
            state_.ready = false;
            state_.reason = "target_unstable";
            state_.stable_ms = 0;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }

    if (!stabilized) {
        fail_state("目标稳定检测超时");
        return false;
    }
    return true;
}

}  // namespace ttbox::core::web
