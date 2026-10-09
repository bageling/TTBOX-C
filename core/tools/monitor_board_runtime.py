#!/usr/bin/env python3
"""板端长时间监控：轮询 /api/state + 热区温度，用于验证改动稳定性。"""


★ 分工（业主 2026-10-06 定案「代码全部用 C++，只有脚本用 py」）：
  本脚本是**板端运维/诊断**工具，只读不写、不含任何控制逻辑。
  控制器与仿真逻辑全在 C++（core/src/aim/*.hpp、core/tools/replay/replay_main.cpp）。

import argparse
import json
import subprocess
import sys
import time
import urllib.request


SUMMARY_KEYS = (
    "capture_fps",
    "fps",
    "infer_ms",
    "infer_set_input_ms",
    "infer_run_ms",
    "infer_output_ms",
    "decode_ms",
    "resize_ms",
    "queue_wait_ms",
    "e2e_ms",
    "preview_fps",
    "preview_encode_ms",
    "preview_dropped",
    "frames_superseded",
    "model_errors",
    "state",
    "runtime_status",
)


def flatten_metrics(payload: dict) -> dict:
    data = payload.get("data", payload)
    metrics = data.get("metrics")
    if metrics is None and isinstance(data.get("state"), dict):
        metrics = data["state"]
    out: dict = {}
    if not isinstance(metrics, dict):
        return out
    for value in metrics.values():
        if isinstance(value, dict):
            out.update(value)
    out.update(metrics)
    return out


def read_temps() -> list[int]:
    try:
        zones = subprocess.run(
            ["sh", "-c", "cat /sys/class/thermal/thermal_zone*/temp 2>/dev/null"],
            capture_output=True,
            text=True,
            timeout=3,
        )
        return [int(v) // 1000 for v in zones.stdout.split() if v.strip()]
    except Exception:
        return []


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--url", default="http://127.0.0.1:8000/api/state")
    parser.add_argument("--seconds", type=float, default=60.0)
    parser.add_argument("--interval", type=float, default=5.0)
    args = parser.parse_args()

    samples: list[dict] = []
    deadline = time.monotonic() + args.seconds
    while time.monotonic() < deadline:
        row: dict = {"ts": time.strftime("%H:%M:%S")}
        try:
            with urllib.request.urlopen(args.url, timeout=5) as resp:
                raw = json.loads(resp.read().decode("utf-8"))
            metrics = flatten_metrics(raw)
            for key in SUMMARY_KEYS:
                if key in metrics:
                    row[key] = metrics[key]
        except Exception as exc:
            row["error"] = str(exc)
        temps = read_temps()
        row["temp_max_c"] = max(temps) if temps else -1
        row["temp_min_c"] = min(temps) if temps else -1
        samples.append(row)
        fields = " ".join(f"{k}={v}" for k, v in row.items())
        print(fields, flush=True)
        time.sleep(args.interval)

    print("\n=== summary ranges ===", flush=True)
    num_keys = [
        "capture_fps",
        "fps",
        "infer_ms",
        "infer_set_input_ms",
        "decode_ms",
        "e2e_ms",
        "preview_fps",
        "preview_encode_ms",
        "preview_dropped",
        "frames_superseded",
        "model_errors",
        "temp_max_c",
    ]
    for key in num_keys:
        values = [s.get(key) for s in samples if isinstance(s.get(key), (int, float))]
        if not values:
            continue
        print(
            f"{key}: min={min(values)} max={max(values)} "
            f"avg={sum(values) / len(values):.3f} n={len(values)}",
            flush=True,
        )
    errors = [s for s in samples if "error" in s]
    if errors:
        print(f"poll_errors={len(errors)}", flush=True)
        for err in errors[:5]:
            print(json.dumps(err, ensure_ascii=False), flush=True)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
