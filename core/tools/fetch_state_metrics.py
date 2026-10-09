#!/usr/bin/env python3
"""板端调试：从 ttbox-web /api/state 拉取并打印核心指标子集。"""


★ 分工（业主 2026-10-06 定案「代码全部用 C++，只有脚本用 py」）：
  本脚本是**板端运维/诊断**工具，只读不写、不含任何控制逻辑。
  控制器与仿真逻辑全在 C++（core/src/aim/*.hpp、core/tools/replay/replay_main.cpp）。
import json
import sys
import urllib.request


def main() -> int:
    url = sys.argv[1] if len(sys.argv) > 1 else "http://127.0.0.1:8000/api/state"
    with urllib.request.urlopen(url, timeout=5) as resp:
        payload = json.loads(resp.read().decode("utf-8"))
    data = payload.get("data", {})
    metrics = data.get("metrics")
    if metrics is None and isinstance(data.get("state"), dict):
        metrics = data["state"]
    if isinstance(metrics, dict) and isinstance(metrics.get("capture"), dict):
        metrics = dict(metrics)
        metrics.update(metrics["capture"])
    if isinstance(metrics, dict) and isinstance(metrics.get("latency"), dict):
        metrics = dict(metrics)
        metrics.update(metrics["latency"])
    if isinstance(metrics, dict) and isinstance(metrics.get("preview"), dict):
        metrics = dict(metrics)
        metrics.update(metrics["preview"])
    if metrics is None:
        print(json.dumps(data, ensure_ascii=False, indent=2)[:4000])
        return 0
    keys = (
        "capture_fps",
        "fps",
        "infer_ms",
        "infer_set_input_ms",
        "infer_run_ms",
        "infer_output_ms",
        "infer_p50_ms",
        "infer_p95_ms",
        "decode_ms",
        "decode_p50_ms",
        "decode_p95_ms",
        "resize_ms",
        "queue_wait_ms",
        "e2e_ms",
        "e2e_p50_ms",
        "e2e_p95_ms",
        "e2e_max_ms",
        "preview_fps",
        "preview_encode_ms",
        "preview_width",
        "preview_height",
        "preview_bytes",
        "preview_frames",
        "preview_dropped",
        "frames_superseded",
        "model_errors",
        "state",
        "runtime_status",
    )
    for key in keys:
        if key in metrics:
            print(f"{key}={metrics[key]}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
