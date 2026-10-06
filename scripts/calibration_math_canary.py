#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""标定拟合数学 Python/C++ 对拍金丝雀（Python 侧）。

对同一组输入求值并打印黄金值；C++ 侧为 core/tools/calibration_math_canary.cpp
（用容差断言同一组黄金值）。本脚本可独立重跑，供回归时重新产黄金值对照。
用法：
    python3 scripts/calibration_math_canary.py
"""
import json
import sys

sys.path.insert(0, ".")
from ttbox_motion.calibration import (
    CalibrationAxis,
    CalibrationObservation,
    derive_pid_params,
    fit_axis_measurements,
)


def main() -> int:
    print("=== derive_pid_params ===")
    pid_cases = [
        (0.686, 0.695, 51.0),
        (0.686, 0.695, 0.0),
        (1.5, 1.5, 60.0),
        (0.04, 0.04, 0.0),
        (8.0, 8.0, 120.0),
        (0.686, 0.695, 200.0),
    ]
    for c in pid_cases:
        print(c, "->", json.dumps(derive_pid_params(*c), sort_keys=True))

    def dump(fit):
        return {
            "gain_px_per_count": fit.gain_px_per_count,
            "response_delay_ms": fit.response_delay_ms,
            "sample_count": fit.sample_count,
            "rejected_count": fit.rejected_count,
            "consistency": fit.consistency,
            "converged": fit.converged,
            "failure_reason": fit.failure_reason,
        }

    print("=== fit_axis_measurements ===")
    # converged
    vals = [0.5, 0.52, 0.48, 0.51, 0.50, 0.53, 0.49]
    obs = [
        CalibrationObservation(
            axis=CalibrationAxis.X,
            injected_count=100.0,
            measured_delta_px=v * 100.0,
            response_delay_ms=51.0 + (i % 3),
            target_id="1:5",
        )
        for i, v in enumerate(vals)
    ]
    print("converged:", json.dumps(dump(fit_axis_measurements(CalibrationAxis.X, obs)), sort_keys=True))

    print("insufficient:",
          json.dumps(dump(fit_axis_measurements(CalibrationAxis.X, obs[:3])), sort_keys=True))

    obs2 = list(obs)
    obs2[3] = CalibrationObservation(
        axis=CalibrationAxis.X,
        injected_count=100.0,
        measured_delta_px=51.0,
        response_delay_ms=51.0,
        target_id="2:9",
    )
    print("multi_target:",
          json.dumps(dump(fit_axis_measurements(CalibrationAxis.X, obs2)), sort_keys=True))

    obs3 = [
        CalibrationObservation(
            axis=CalibrationAxis.Y,
            injected_count=10.0,
            measured_delta_px=90.0,
            response_delay_ms=50.0,
            target_id="1:5",
        )
        for _ in range(6)
    ]
    print("gain_out_of_range:",
          json.dumps(dump(fit_axis_measurements(CalibrationAxis.Y, obs3)), sort_keys=True))

    vals4 = [0.5, 1.5, 0.4, 1.6, 0.3, 1.7]
    obs4 = [
        CalibrationObservation(
            axis=CalibrationAxis.X,
            injected_count=100.0,
            measured_delta_px=v * 100.0,
            response_delay_ms=50.0,
            target_id="1:5",
        )
        for v in vals4
    ]
    print("inconsistent:",
          json.dumps(dump(fit_axis_measurements(CalibrationAxis.X, obs4)), sort_keys=True))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
