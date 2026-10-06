// calibration_math_canary.cpp — 标定拟合数学 Python/C++ 对拍金丝雀（C++ 侧）。
//
// 只 include 头文件（纯函数、零 core 依赖），可与 scripts/calibration_math_canary.py
// 对同一组输入求值并对拍。本侧将结果与 Python 侧产出的黄金值做容差断言，
// 全过返回 0，任一不符返回非 0。编译（无需 core 库）：
//   g++ -std=c++17 -I core/src tools/calibration_math_canary.cpp -o /tmp/calib_canary
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

#include "web/infra/calibration_math.hpp"

using ttbox::core::web::CalibrationAxis;
using ttbox::core::web::CalibrationObservation;
using ttbox::core::web::PidParams;
using ttbox::core::web::derive_pid_params;
using ttbox::core::web::fit_axis_measurements;

namespace {

int failures = 0;

void check(const std::string& name, double got, double want, double tol) {
    const bool ok = std::fabs(got - want) <= tol;
    if (!ok) {
        std::printf("FAIL %s: got=%.10f want=%.10f\n", name.c_str(), got, want);
        ++failures;
    }
}

void check_int(const std::string& name, int got, int want) {
    if (got != want) {
        std::printf("FAIL %s: got=%d want=%d\n", name.c_str(), got, want);
        ++failures;
    }
}

void check_str(const std::string& name, const std::string& got, const std::string& want) {
    if (got != want) {
        std::printf("FAIL %s: got=\"%s\" want=\"%s\"\n", name.c_str(), got.c_str(),
                    want.c_str());
        ++failures;
    }
}

void check_bool(const std::string& name, bool got, bool want) {
    if (got != want) {
        std::printf("FAIL %s: got=%d want=%d\n", name.c_str(), got, want);
        ++failures;
    }
}

void check_pid(const std::string& name, double gx, double gy, double delay, double kp,
               double kd, double predict) {
    const PidParams p = derive_pid_params(gx, gy, delay);
    check(name + ".kp", p.kp, kp, 1e-6);
    check(name + ".kd", p.kd, kd, 1e-6);
    check(name + ".predict", p.predict, predict, 1e-9);
}

}  // namespace

int main() {
    // ---- derive_pid_params 对拍（黄金值 = Python ttbox_motion.calibration）----
    check_pid("p0", 0.686, 0.695, 51.0, 10.2041, 31.0204, 0.18);
    check_pid("p1", 0.686, 0.695, 0.0, 10.2041, 10.2041, 0.35);
    check_pid("p2", 1.5, 1.5, 60.0, 4.0, 13.6, 0.15);
    check_pid("p3", 0.04, 0.04, 0.0, 100.0, 50.0, 0.35);
    check_pid("p4", 8.0, 8.0, 120.0, 4.0, 23.2, 0.1);
    check_pid("p5", 0.686, 0.695, 200.0, 10.2041, 50.0, 0.1);

    // ---- fit_axis_measurements 对拍 ----
    // converged
    {
        std::vector<CalibrationObservation> obs;
        const double vals[] = {0.5, 0.52, 0.48, 0.51, 0.50, 0.53, 0.49};
        for (int i = 0; i < 7; ++i) {
            CalibrationObservation o;
            o.axis = CalibrationAxis::kX;
            o.injected_count = 100.0;
            o.measured_delta_px = vals[i] * 100.0;
            o.response_delay_ms = 51.0 + (i % 3);
            o.target_id = "1:5";
            obs.push_back(o);
        }
        const auto f = fit_axis_measurements(obs, CalibrationAxis::kX);
        check("f0.gain", f.gain_px_per_count, 0.5, 1e-9);
        check("f0.delay", f.response_delay_ms, 52.0, 1e-9);
        check_int("f0.sample", f.sample_count, 7);
        check_int("f0.rejected", f.rejected_count, 0);
        check("f0.consistency", f.consistency, 0.98, 1e-9);
        check_bool("f0.converged", f.converged, true);
        check_str("f0.reason", f.failure_reason, "");
    }
    // insufficient
    {
        std::vector<CalibrationObservation> obs;
        for (int i = 0; i < 3; ++i) {
            CalibrationObservation o;
            o.axis = CalibrationAxis::kX;
            o.injected_count = 100.0;
            o.measured_delta_px = 50.0;
            o.response_delay_ms = 51.0;
            o.target_id = "1:5";
            obs.push_back(o);
        }
        const auto f = fit_axis_measurements(obs, CalibrationAxis::kX);
        check_int("f1.sample", f.sample_count, 3);
        check_bool("f1.converged", f.converged, false);
        check_str("f1.reason", f.failure_reason, "x轴有效样本不足");
    }
    // multi target
    {
        std::vector<CalibrationObservation> obs;
        const double vals[] = {0.5, 0.52, 0.48, 0.51, 0.50, 0.53, 0.49};
        for (int i = 0; i < 7; ++i) {
            CalibrationObservation o;
            o.axis = CalibrationAxis::kX;
            o.injected_count = 100.0;
            o.measured_delta_px = vals[i] * 100.0;
            o.response_delay_ms = 51.0 + (i % 3);
            o.target_id = (i == 3) ? "2:9" : "1:5";
            obs.push_back(o);
        }
        const auto f = fit_axis_measurements(obs, CalibrationAxis::kX);
        check_bool("f2.converged", f.converged, false);
        check_str("f2.reason", f.failure_reason, "目标身份在标定过程中发生变化");
    }
    // gain out of range
    {
        std::vector<CalibrationObservation> obs;
        for (int i = 0; i < 6; ++i) {
            CalibrationObservation o;
            o.axis = CalibrationAxis::kY;
            o.injected_count = 10.0;
            o.measured_delta_px = 90.0;
            o.response_delay_ms = 50.0;
            o.target_id = "1:5";
            obs.push_back(o);
        }
        const auto f = fit_axis_measurements(obs, CalibrationAxis::kY);
        check("f3.gain", f.gain_px_per_count, 9.0, 1e-9);
        check("f3.delay", f.response_delay_ms, 50.0, 1e-9);
        check_int("f3.sample", f.sample_count, 6);
        check("f3.consistency", f.consistency, 1.0, 1e-9);
        check_bool("f3.converged", f.converged, false);
        check_str("f3.reason", f.failure_reason, "y轴增益超出范围");
    }
    // inconsistent (high MAD)
    {
        std::vector<CalibrationObservation> obs;
        const double vals[] = {0.5, 1.5, 0.4, 1.6, 0.3, 1.7};
        for (int i = 0; i < 6; ++i) {
            CalibrationObservation o;
            o.axis = CalibrationAxis::kX;
            o.injected_count = 100.0;
            o.measured_delta_px = vals[i] * 100.0;
            o.response_delay_ms = 50.0;
            o.target_id = "1:5";
            obs.push_back(o);
        }
        const auto f = fit_axis_measurements(obs, CalibrationAxis::kX);
        check("f4.gain", f.gain_px_per_count, 0.0, 1e-9);
        check("f4.delay", f.response_delay_ms, 0.0, 1e-9);
        check_int("f4.sample", f.sample_count, 6);
        check_int("f4.rejected", f.rejected_count, 6);
        check("f4.consistency", f.consistency, 0.0, 1e-9);
        check_bool("f4.converged", f.converged, false);
        check_str("f4.reason", f.failure_reason, "x轴测量一致性不足");
    }

    if (failures == 0) {
        std::printf("calibration_math_canary: ALL PASS\n");
        return 0;
    }
    std::printf("calibration_math_canary: %d FAILURES\n", failures);
    return 1;
}
