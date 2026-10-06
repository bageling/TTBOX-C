// calibration_controller.hpp — 自动标定控制器（线程体 + 共享状态 + 文件 + 写回）。
//
// 自 plugins/web/lib/calibration.py 逐行移植。职责：
//   · _cal 共享状态（等价 Python `_cal` dict）由 CalibrationRuntimeState 承载；
//   · _cal_lock 由 state_mutex() 承载，payload / cancel / worker 三方共用；
//   · 拟合数学（derive_pid_params / fit_axis_measurements）在 calibration_math.hpp；
//   · 线程体 worker() 在 calibration_worker.cpp（因 >300 行拆出）。
#pragma once

#include <atomic>
#include <chrono>
#include <ctime>
#include <mutex>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "common/Json.hpp"
#include "web/infra/calibration_math.hpp"
#include "web/infra/ipc_client.hpp"

namespace ttbox::core::web {

// 配置读-改-写串行锁（对齐 plugins/web/lib/locks.py::_CFG_WRITE_LOCK，RLock）。
// 所有 GET_CONFIG→改→SET_CONFIG 序列必须持此锁，防止 64 线程 + 标定线程互相整份覆盖。
inline std::recursive_mutex& config_write_lock() {
    static std::recursive_mutex m;
    return m;
}

// 运行期标定状态（对齐 Python `_cal` dict 的默认值，键名不变）。
struct CalibrationRuntimeState {
    std::string phase = "idle";
    std::string status = "idle";   // idle|running|success|failed|manual
    std::string state = "idle";    // 对外镜像 CalibrationState
    bool ready = false;
    std::string reason = "not_running";
    int total_rounds = 8;
    int round = 0;
    double progress = 0.0;
    std::string current_axis;
    std::vector<double> round_gains;
    JsonValue axis_fits = JsonValue::object();
    int valid_sample_count = 0;
    int candidate_count = 0;
    int64_t candidate_track_id = -1;
    int64_t candidate_class_id = -1;
    double candidate_width = 0.0;
    double candidate_height = 0.0;
    int stable_frames = 0;
    int stable_ms = 0;
    double center_jitter_px = 0.0;
    double size_variation = 0.0;
    int elapsed_ms = 0;
    int amplitude_counts = 0;
    double amplitude_px = 0.0;
    int settle_ms = 0;
    bool settled = false;
    int dropped_sample_count = 0;
    double max_tracked_amp = 0.0;
};

// 采样对（对齐 `_calib_sample_pair` 返回：px / counts / target 同源同时刻）。
struct SamplePair {
    double px = 0.0;
    double counts = 0.0;
    JsonValue target;
};

// 本地时间戳 %Y%m%d_%H%M%S（对齐 Python time.strftime('%Y%m%d_%H%M%S')）。
inline std::string timestamp_name() {
    const std::time_t t = std::time(nullptr);
    std::tm tm{};
#if defined(_WIN32)
    localtime_s(&tm, &t);
#else
    localtime_r(&t, &tm);
#endif
    char buf[32];
    std::strftime(buf, sizeof(buf), "%Y%m%d_%H%M%S", &tm);
    return buf;
}

// 取 RuntimeProfile 的 mouse 子对象（不存在 → 空对象，可 set 后写回）。
inline JsonValue mouse_of(const JsonValue& prof) {
    const JsonValue* mo = prof.is_object() ? prof.find("mouse") : nullptr;
    return (mo != nullptr && mo->is_object()) ? *mo : JsonValue::object();
}

// 自 t0 起的毫秒（单调钟，worker 各阶段计时共用）。
inline double steady_ms_since(std::chrono::steady_clock::time_point t0) {
    return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0)
        .count();
}

class CalibrationController {
public:
    explicit CalibrationController(IpcClient& ipc) : ipc_(ipc) {}

    // ---- HTTP 编排用（5 条路由）----
    bool running();
    JsonValue payload();  // _calibration_payload() 等价
    int start(std::string* error);  // 返回 HTTP 状态：200 成功 / 409 已在运行 / 400 前置不满足
    void cancel();
    void clear();
    JsonValue manual_update(double gain_x, double gain_y, double delay, bool* ok,
                            std::string* detail);

    // ---- 线程入口（std::thread target，由 start() 起）----
    void thread_entry();

    // ---- 供 worker / 其它 .cpp 使用的内部访问 ----
    std::mutex& state_mutex() { return state_mu_; }
    CalibrationRuntimeState& state() { return state_; }
    IpcClient& ipc() { return ipc_; }

    // SET_CONFIG 薄封装（worker / 内部写回共用）。
    JsonValue set_config(const JsonValue& prof) {
        JsonValue params = JsonValue::object();
        params.set("profile", prof);
        return ipc_.call("SET_CONFIG", params, kIpcTimeoutDefaultMs);
    }

    // ---- 段内共享（calibration_worker.cpp 需要）----
    bool apply_bias(CalibrationAxis axis, double value);
    std::pair<bool, double> wait_settled(CalibrationAxis axis);
    std::optional<SamplePair> sample_pair(CalibrationAxis axis, int n = 3);
    JsonValue sample_target();                 // 无目标 → null
    std::optional<std::pair<double, double>> out_counts();
    int write_ok();
    std::pair<bool, std::string> apply_gain(const JsonValue& calib);
    std::pair<bool, std::string> apply_pid(const JsonValue& calib);
    PidParams derive_pid(double gain_x, double gain_y, double delay_ms);
    bool read_calibration(JsonValue* out);
    bool write_calibration(const JsonValue& data, std::string* detail);
    void clear_calibration_file();
    std::string read_active_model();

private:
    // 跨阶段共享的 worker 局部量（拆到两个 .cpp 后仍需共享）。
    struct WorkerContext {
        bool was_enabled = false;
        bool has_saved_kp = false;
        bool has_saved_kd = false;
        double saved_kp = 0.0;
        double saved_kd = 0.0;
        double calib_kp = kCalibPidKpMax;
        std::vector<CalibrationObservation> obs[2];  // [0]=X [1]=Y
        int dropped[2] = {0, 0};
        bool no_write = false;
        double max_tracked_amps[2] = {0.0, 0.0};
    };

    void worker();
    bool is_running();
    void fail_state(const std::string& reason);
    void cancel_state();
    bool worker_prepare(WorkerContext& ctx);      // 前置段（持锁，抛异常由 thread_entry 兜）
    bool worker_stabilize(WorkerContext& ctx);    // 稳定检测（false = 已落终态）
    bool worker_sample(WorkerContext& ctx);       // X/Y 分轴采样（false = 已落终态）
    void worker_finish(WorkerContext& ctx);       // 拟合 + 写回 + 终态
    void worker_cleanup(WorkerContext& ctx);      // finally 恢复段

    IpcClient& ipc_;
    std::mutex state_mu_;
    CalibrationRuntimeState state_;
    std::atomic<bool> thread_running_{false};
};

}  // namespace ttbox::core::web
