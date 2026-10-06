// calibration_controller.cpp — 标定控制器低层原语（文件 / 采样 / 写回）。
//
// 自 plugins/web/lib/calibration.py 逐行移植。设备读写与采样在本文；
// HTTP 编排面（payload / start / cancel / clear / manual_update）在 calibration_api.cpp；
// 线程体 worker() 在 calibration_worker.cpp。
#include "web/infra/calibration_controller.hpp"

#include <chrono>
#include <filesystem>
#include <thread>

#include "web/domain/domain_internal.hpp"

namespace ttbox::core::web {

namespace {

std::string calibration_file() { return join_path(config_dir(), "calibration.json"); }
std::string active_model_file() { return join_path(models_root(), "active_model.txt"); }

}  // namespace

bool CalibrationController::running() { return thread_running_.load(); }

// ---- 标定留档文件（calibration.json）----
bool CalibrationController::read_calibration(JsonValue* out) {
    std::string text;
    if (!read_file(calibration_file(), &text)) return false;
    JsonParseResult r = json_parse(text);
    if (!r.ok || !r.value.is_object()) return false;
    if (out != nullptr) *out = std::move(r.value);
    return true;
}

bool CalibrationController::write_calibration(const JsonValue& data, std::string* detail) {
    std::error_code ec;
    std::filesystem::create_directories(config_dir(), ec);
    const std::string tmp = calibration_file() + ".tmp";
    if (!write_file(tmp, data.dump())) {
        if (detail != nullptr) *detail = "写入失败: 无法打开临时文件";
        return false;
    }
    if (std::rename(tmp.c_str(), calibration_file().c_str()) != 0) {
        if (detail != nullptr) *detail = "写入失败: rename 失败";
        return false;
    }
    if (detail != nullptr) *detail = "标定参数已保存";
    return true;
}

void CalibrationController::clear_calibration_file() {
    std::error_code ec;
    std::filesystem::remove(calibration_file(), ec);
}

std::string CalibrationController::read_active_model() {
    std::string text;
    if (!read_file(active_model_file(), &text)) return "";
    // strip 首尾空白（对齐 Python .read().strip()）。
    size_t b = 0, e = text.size();
    while (b < e && std::isspace(static_cast<unsigned char>(text[b])) != 0) ++b;
    while (e > b && std::isspace(static_cast<unsigned char>(text[e - 1])) != 0) --e;
    return text.substr(b, e - b);
}

// ---- 设备观测原语（等价入口的 _calib_target / _calib_out_counts）----
JsonValue CalibrationController::sample_target() {
    const JsonValue st = get_status_data(ipc_);
    const JsonValue m = json_field(st, "metrics");
    if (!json_truthy(json_field(st, "runtime_running")) ||
        !json_truthy(json_field(m, "aim_has_target"))) {
        return JsonValue::null();
    }
    const int64_t target_id = json_field(m, "aim_target_id").as_int(-1);
    const int64_t class_id = json_field(m, "aim_target_class_id").as_int(-1);
    const double width = json_field(m, "aim_target_width").as_number(0.0);
    const double height = json_field(m, "aim_target_height").as_number(0.0);
    if (target_id < 0 || class_id < 0 || width <= 0 || height <= 0) return JsonValue::null();

    JsonValue t = JsonValue::object();
    t.set("x", JsonValue::number(json_field(m, "aim_pos_x").as_number(0.0)));
    t.set("y", JsonValue::number(json_field(m, "aim_pos_y").as_number(0.0)));
    t.set("target_id", JsonValue::number(static_cast<double>(target_id)));
    t.set("class_id", JsonValue::number(static_cast<double>(class_id)));
    t.set("width", JsonValue::number(width));
    t.set("height", JsonValue::number(height));
    t.set("error_x", JsonValue::number(json_field(m, "aim_error_x").as_number(0.0)));
    t.set("error_y", JsonValue::number(json_field(m, "aim_error_y").as_number(0.0)));
    return t;
}

std::optional<std::pair<double, double>> CalibrationController::out_counts() {
    const JsonValue m = json_field(get_status_data(ipc_), "metrics");
    const JsonValue* cx = m.find("aim_out_counts_x");
    const JsonValue* cy = m.find("aim_out_counts_y");
    if (cx == nullptr || cy == nullptr) return std::nullopt;  // 缺任一轴 ⇒ 整体判读不到
    return std::make_pair(cx->as_number(0.0), cy->as_number(0.0));
}

int CalibrationController::write_ok() {
    const JsonValue m = json_field(get_status_data(ipc_), "metrics");
    return static_cast<int>(json_field(m, "mouse_control_socket_write_ok").as_int(0));
}

// ---- 采样对 / 偏置 / 等静止 ----
std::optional<SamplePair> CalibrationController::sample_pair(CalibrationAxis axis, int n) {
    std::vector<SamplePair> pairs;
    for (int i = 0; i < n; ++i) {
        auto c = out_counts();
        JsonValue t = sample_target();
        if (!t.is_null() && c.has_value()) {
            SamplePair p;
            p.px = axis == CalibrationAxis::kX ? json_field(t, "x").as_number(0.0)
                                               : json_field(t, "y").as_number(0.0);
            p.counts = axis == CalibrationAxis::kX ? c->first : c->second;
            p.target = std::move(t);
            pairs.push_back(std::move(p));
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(4));
    }
    if (pairs.empty()) return std::nullopt;
    // 按 px 排序取中位那一对（px/count/target 同源同时刻）。
    std::sort(pairs.begin(), pairs.end(),
              [](const SamplePair& a, const SamplePair& b) { return a.px < b.px; });
    return pairs[pairs.size() / 2];
}

bool CalibrationController::apply_bias(CalibrationAxis axis, double value) {
    JsonValue prof;
    std::string err;
    if (!get_runtime_profile(ipc_, &prof, &err)) return false;
    JsonValue mo = mouse_of(prof);
    mo.set("calibration_bias_x",
           JsonValue::number(axis == CalibrationAxis::kX ? value : 0.0));
    mo.set("calibration_bias_y",
           JsonValue::number(axis == CalibrationAxis::kY ? value : 0.0));
    mo.set("calibrating", JsonValue::boolean(true));
    prof.set("mouse", std::move(mo));
    return ipc_status(set_config(prof)) == 0;
}

std::pair<bool, double> CalibrationController::wait_settled(CalibrationAxis axis) {
    const auto t0 = std::chrono::steady_clock::now();
    std::vector<double> win;
    while (std::chrono::steady_clock::now() - t0 < std::chrono::milliseconds(800)) {
        auto pair = sample_pair(axis, 1);
        if (!pair.has_value()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
            continue;
        }
        win.push_back(pair->px);
        if (win.size() > 3) win.erase(win.begin());
        if (win.size() >= 3 &&
            (*std::max_element(win.begin(), win.end()) - *std::min_element(win.begin(), win.end())) <
                0.5) {
            const double ms = std::chrono::duration<double, std::milli>(
                                  std::chrono::steady_clock::now() - t0)
                                  .count();
            return {true, ms};
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    const double ms =
        std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    return {false, ms};
}

// ---- 写回（对齐 _calib_apply_gain / _calib_apply_pid / _calib_derive_pid）----
std::pair<bool, std::string> CalibrationController::apply_gain(const JsonValue& calib) {
    const double gain_x = json_field(calib, "mouse_gain_x_px_per_count").as_number(0.0);
    const double gain_y = json_field(calib, "mouse_gain_y_px_per_count").as_number(0.0);
    if (gain_x <= 0 || gain_y <= 0) return {false, "增益必须 > 0"};
    std::lock_guard<std::recursive_mutex> lk(config_write_lock());
    JsonValue prof;
    std::string err;
    if (!get_runtime_profile(ipc_, &prof, &err)) return {false, "读取 RuntimeProfile 失败"};
    JsonValue mo = mouse_of(prof);
    mo.set("gain_x_px_per_count", JsonValue::number(round_to(gain_x, 4)));
    mo.set("gain_y_px_per_count", JsonValue::number(round_to(gain_y, 4)));
    JsonValue pt = json_field(mo, "personal_trajectory").is_object()
                       ? json_field(mo, "personal_trajectory")
                       : JsonValue::object();
    pt.set("response_px_per_count", JsonValue::number(round_to(gain_y, 4)));
    mo.set("personal_trajectory", std::move(pt));
    const double delay_ms = json_field(calib, "mouse_response_delay_ms").as_number(0.0);
    if (delay_ms > 0) mo.set("response_delay_ms", JsonValue::number(round_to(delay_ms, 2)));
    prof.set("mouse", std::move(mo));
    const JsonValue r = set_config(prof);
    const bool ok = ipc_status(r) == 0;
    const std::string detail = json_field(r, "error").as_string("配置已更新");
    return {ok, detail};
}

PidParams CalibrationController::derive_pid(double gain_x, double gain_y, double delay_ms) {
    return derive_pid_params(gain_x, gain_y, delay_ms);
}

std::pair<bool, std::string> CalibrationController::apply_pid(const JsonValue& calib) {
    std::lock_guard<std::recursive_mutex> lk(config_write_lock());
    JsonValue prof;
    std::string err;
    if (!get_runtime_profile(ipc_, &prof, &err)) return {false, "读取 RuntimeProfile 失败"};
    const double gain_x = json_field(calib, "mouse_gain_x_px_per_count").as_number(0.0);
    const double gain_y = json_field(calib, "mouse_gain_y_px_per_count").as_number(0.0);
    const double delay_ms = json_field(calib, "mouse_response_delay_ms").as_number(0.0);
    const PidParams pid = derive_pid_params(gain_x, gain_y, delay_ms);
    JsonValue mo = mouse_of(prof);
    mo.set("kp_x", JsonValue::number(pid.kp));
    mo.set("kp_y", JsonValue::number(pid.kp));
    mo.set("kd_x", JsonValue::number(pid.kd));
    mo.set("kd_y", JsonValue::number(pid.kd));
    mo.set("predict_x", JsonValue::number(pid.predict));
    prof.set("mouse", std::move(mo));
    const JsonValue r = set_config(prof);
    return {ipc_status(r) == 0, json_field(r, "error").as_string("配置已更新")};
}

}  // namespace ttbox::core::web
