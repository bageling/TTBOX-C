// calibration_worker_sample.cpp — 标定线程体（X/Y 分轴采样）。
//
// 自 plugins/web/lib/calibration.py::_calib_worker 逐行移植（拆半）。拟合 / 写回 /
// finally 恢复在 calibration_worker_finish.cpp。
#include "web/infra/calibration_controller.hpp"

#include <chrono>
#include <string>
#include <thread>

#include "web/domain/domain_internal.hpp"

namespace ttbox::core::web {

// X/Y 分轴采样：逐幅度注入偏置、测位移与响应延迟，收集有效观测（不足则失败）。
bool CalibrationController::worker_sample(WorkerContext& ctx) {
    if (!out_counts().has_value()) {
        fail_state("当前 Core 不提供 aim_out_counts_*（需 1.5.51 及以上）："
                   "拿不到真实注入 count，无法测出物理 gain");
        return false;
    }

    for (int ai = 0; ai < 2; ++ai) {
        const CalibrationAxis axis = ai == 0 ? CalibrationAxis::kX : CalibrationAxis::kY;
        const std::string av = axis_value(axis);
        {
            std::lock_guard<std::mutex> lk(state_mutex());
            state_.state = "stabilize_" + av;
            state_.phase = "stabilize_" + av;
            state_.current_axis = av;
            state_.round = 0;
            state_.progress = ai == 1 ? 0.5 : 0.0;
        }
        double max_tracked_amp = 0.0;
        int miss_streak = 0;

        for (int index = 0; index < kCalibAmplitudeCount; ++index) {
            const double amp = kCalibAmplitudes[index];
            if (!is_running()) {
                cancel_state();
                return false;
            }
            // 轮间回零：清偏置并等瞄点静止。
            if (!apply_bias(axis, 0.0)) {
                fail_state("Core 配置应用失败");
                return false;
            }
            const auto [settled, settle_ms] = wait_settled(axis);
            {
                std::lock_guard<std::mutex> lk(state_mutex());
                state_.state = "sampling_" + av;
                state_.phase = "measure_" + av + "_response";
                state_.current_axis = av;
                state_.round = index + 1;
                state_.amplitude_px = amp;
                state_.amplitude_counts = static_cast<int>(amp);
                state_.settle_ms = static_cast<int>(settle_ms);
                state_.settled = settled;
                state_.progress = (index + (ai == 0 ? 0 : 8)) / 16.0;
            }
            auto start_opt = sample_pair(axis);
            if (!start_opt.has_value()) {
                fail_state("no_target");
                return false;
            }
            const SamplePair start = *start_opt;
            const int write0 = write_ok();
            if (!apply_bias(axis, amp)) {
                fail_state("Core 配置应用失败");
                return false;
            }

            // 采样窗：位移够 / 到窗口上限；同时测真实响应延迟（≥0.3px 首响应）。
            const auto injected_at = std::chrono::steady_clock::now();
            const double target_px = amp * kCalibAmpTrackRatio;
            double first_response_ms = -1.0;
            bool tracked = false;
            const auto win_deadline = injected_at + std::chrono::milliseconds(800);
            while (std::chrono::steady_clock::now() < win_deadline) {
                if (!is_running()) {
                    apply_bias(axis, 0.0);
                    cancel_state();
                    return false;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(8));
                const JsonValue cur = sample_target();
                if (cur.is_null() ||
                    json_field(cur, "target_id").as_int(-1) !=
                        json_field(start.target, "target_id").as_int(-1)) {
                    continue;
                }
                const double cur_px = ai == 0 ? json_field(cur, "x").as_number(0.0)
                                              : json_field(cur, "y").as_number(0.0);
                const double moved = std::abs(cur_px - start.px);
                if (first_response_ms < 0 && moved >= 0.3) {
                    first_response_ms = steady_ms_since(injected_at);
                }
                if (moved >= std::abs(target_px)) {
                    tracked = true;
                    break;
                }
            }

            auto end_opt = sample_pair(axis);
            apply_bias(axis, 0.0);  // 本轮结束立即回零
            {
                std::lock_guard<std::mutex> lk(state_mutex());
                state_.valid_sample_count = static_cast<int>(ctx.obs[0].size() + ctx.obs[1].size());
            }
            if (!end_opt.has_value()) continue;
            const SamplePair end = *end_opt;

            const double d_px = std::abs(end.px - start.px);
            const double d_counts = std::abs(end.counts - start.counts);
            const bool same_target =
                json_field(end.target, "target_id").as_int(-1) ==
                    json_field(start.target, "target_id").as_int(-1) &&
                json_field(end.target, "class_id").as_int(-1) ==
                    json_field(start.target, "class_id").as_int(-1);
            const bool wrote = (write_ok() - write0) > 0;

            if (same_target && d_px >= kCalibMinDeltaPx && d_counts >= kCalibMinCounts && wrote) {
                CalibrationObservation obs;
                obs.axis = axis;
                obs.injected_count = d_counts;
                obs.measured_delta_px = d_px;
                obs.response_delay_ms =
                    first_response_ms >= 0 ? first_response_ms : steady_ms_since(injected_at);
                obs.target_id = std::to_string(json_field(start.target, "target_id").as_int(-1)) +
                                ":" +
                                std::to_string(json_field(start.target, "class_id").as_int(-1));
                obs.valid = true;
                ctx.obs[ai].push_back(std::move(obs));
            } else {
                ctx.dropped[ai] += 1;
                if (d_counts >= kCalibMinCounts && !wrote) {
                    ctx.no_write = true;
                } else if (wrote && d_counts >= kCalibMinCounts && d_px < kCalibMinDeltaPx) {
                    // 注入生效但位移不够 ⇒ 温和档太低：轮间抬 KP（不超过用户原值）。
                    const double kp_cap = ctx.has_saved_kp ? ctx.saved_kp : kCalibPidKpMax;
                    const double new_kp =
                        std::min(ctx.calib_kp * 1.7, std::max(kp_cap, kCalibPidKpMax));
                    if (new_kp > ctx.calib_kp + 1e-6) {
                        ctx.calib_kp = new_kp;
                        bool raised = false;
                        {
                            // ── 配置读-改-写持 config_write_lock：防与面板/其它线程整份覆盖 ──
                            std::lock_guard<std::recursive_mutex> lk(config_write_lock());
                            JsonValue prof;
                            std::string err;
                            if (get_runtime_profile(ipc_, &prof, &err)) {
                                JsonValue mo = mouse_of(prof);
                                mo.set("kp_x", JsonValue::number(new_kp));
                                mo.set("kp_y", JsonValue::number(new_kp));
                                mo.set("kd_x", JsonValue::number(new_kp * kCalibPidKdRatio));
                                mo.set("kd_y", JsonValue::number(new_kp * kCalibPidKdRatio));
                                prof.set("mouse", std::move(mo));
                                set_config(prof);
                                raised = true;
                            }
                        }
                        if (raised) {
                            std::lock_guard<std::mutex> lk(state_mutex());
                            state_.reason = "低增益：已抬高标定期 KP 继续测量";
                        }
                    }
                }
            }
            {
                std::lock_guard<std::mutex> lk(state_mutex());
                state_.dropped_sample_count = ctx.dropped[0] + ctx.dropped[1];
            }
            // 摆动幅度闭环：追得上 → 记最快可追幅度；连续追不上 → 提前收敛停摆。
            if (tracked) {
                max_tracked_amp = std::max(max_tracked_amp, std::abs(amp));
                {
                    std::lock_guard<std::mutex> lk(state_mutex());
                    state_.max_tracked_amp = max_tracked_amp;
                }
                miss_streak = 0;
            } else if (wrote) {
                miss_streak += 1;
                if (miss_streak >= kCalibAmpMissLimit) {
                    std::lock_guard<std::mutex> lk(state_mutex());
                    state_.max_tracked_amp = max_tracked_amp;
                    state_.reason = av + "轴摆动收敛：最快可追幅度 ≈ " +
                                    std::to_string(static_cast<int>(max_tracked_amp)) + "px";
                    break;
                }
            }
        }

        if (ctx.obs[ai].empty()) {
            fail_state(av + "轴无有效样本（" + std::to_string(kCalibAmplitudeCount) +
                       " 轮全部低于门槛：位移需 ≥" + std::to_string(kCalibMinDeltaPx) +
                       "px 且 count 需 ≥" + std::to_string(kCalibMinCounts) +
                       "，或目标在采样中被甩出画面）");
            return false;
        }
        {
            std::lock_guard<std::mutex> lk(state_mutex());
            state_.state = "analyzing_" + av;
            state_.phase = "measure_" + av + "_settle";
            state_.current_axis = av;
        }
        ctx.max_tracked_amps[ai] = max_tracked_amp;
    }

    if (ctx.no_write) {
        fail_state("注入的 count 没有落到 usbproxy（检查输出后端与连线）");
        return false;
    }
    return true;
}

}  // namespace ttbox::core::web
