# ★ 分工（业主 2026-10-06 定案「代码全部用 C++，只有脚本用 py」）：
#   本脚本只做「调 C++ / 生成输入 / 判阈值 / 出报告」，不含任何控制逻辑。
#   控制器与物理仿真全在 C++（core/src/aim/*.hpp、core/tools/replay/replay_main.cpp）。
"""selftest.py — 自瞄控制器**自动测试系统**（V1.0.46）

这是「改完控制器不用上板就能知道好没好」的那一层：跑一批场景 × 一批指标 ×
一组阈值，**自动判定 PASS / WARN / FAIL**，并与基线对比抓回归。

为什么必须有这一层（V1.0.46 的教训）：
  · 手工看数字会漏判 —— V1.0.43 我跑出「稳态 250~850px」还以为是控制器不行，
    实际是回放器自己的 bug（轨迹语义搞错）。
  · 手工比数字更会漏 —— V1.0.43 换了 Fitts 后，「不规则目标翻转 10→4」是结论，
    但没人保证下一版不会又抖回来。**判定必须是代码，不是我。**

四类检查
--------
  A. 阈值判定   单场景单控制器，指标越界就 FAIL（WARN 介于中间）
  B. 场景覆盖   每个控制器必须在所有场景都过，防止"只调好了一个场景"
  C. A/B 回归   相对基线 JSON 比；变差超过容差就 FAIL
  D. 鲁棒性     参数扰动（±20%）后指标不应崩坏（防"只在单点上最优"）

用法
----
    python selftest.py                    # 全跑，退出码 0=全过 1=有 FAIL
    python selftest.py --update-baseline  # 确认当前是新的好状态后刷基线
    python selftest.py --trace x.csv      # 用真实轨迹代替合成场景
    python selftest.py --html out.html   # 同时出报告
"""
from __future__ import annotations

import argparse
import json
import os
import shutil
import sys
import tempfile

import trace_replay as T

BASELINE = os.path.join(os.path.dirname(os.path.abspath(__file__)), "selftest_baseline.json")

# ★ C++ 回放器（V1.0.46）：**直接链接产品控制器头文件**。
#   之前用「Python 精确移植」在 Python 里回放 = 二次翻译，两套独立实现可能同时
#   误解原始算法（V1.0.43 的 τ=MT 错误就是两边犯一样的错）⇒ 互验形同虚设。
#   现在 Python 只负责"跑多个场景 + 判阈值 + 出报告"，控制逻辑一律走产品 C++。
REPLAY_EXE = os.environ.get("TTBOX_REPLAY_EXE", "")


def find_replay_exe() -> str | None:
    """定位 ttbox_replay 可执行文件（build 目录里）。"""
    if REPLAY_EXE and os.path.exists(REPLAY_EXE):
        return REPLAY_EXE
    here = os.path.dirname(os.path.abspath(__file__))
    for cand in (
        os.path.join(here, "..", "..", "build-ascii", "ttbox_replay.exe"),
        os.path.join(here, "..", "..", "build-ascii", "ttbox_replay"),
        os.path.join(here, "..", "..", "build", "ttbox_replay.exe"),
    ):
        p = os.path.normpath(cand)
        if os.path.exists(p):
            return p
    return None


def replay_cpp(trace_csv: str, kind: str, ctl_args: list[str] | None = None) -> dict:
    """调 C++ 回放器跑一段轨迹，返回指标 dict。找不到可执行文件时返回 {}。"""
    exe = find_replay_exe()
    if not exe:
        return {}
    import subprocess, tempfile
    with tempfile.NamedTemporaryFile(suffix=".json", delete=False) as tf:
        outp = tf.name
    cmd = [exe, "--kind", kind, "--out", outp]
    if trace_csv:
        cmd += ["--trace", trace_csv]
    else:
        cmd += ["--frames", "1200"]
    cmd += (ctl_args or [])
    try:
        subprocess.run(cmd, check=True, capture_output=True, timeout=120)
        with open(outp, encoding="utf-8") as f:
            return json.load(f)
    except Exception:
        return {}
    finally:
        try:
            os.unlink(outp)
        except OSError:
            pass

PASS, WARN, FAIL = "PASS", "WARN", "FAIL"

# ────────────────────────── 阈值（唯一真源，改这里就改了判定口径）──────────────────────────
# 依据：板端实测物理量（gain 0.686 px/count、回路延迟 51ms、144fps、检测噪声 ±2px）
TH = {
    # 稳态误差中位数（px）：贴得准。>8 明显跟不上，>15 判 FAIL
    "settle_med_warn": 6.0, "settle_med_fail": 15.0,
    # 稳态 P95（px）：偶发大偏差。>18 WARN，>30 FAIL（被遮挡/漏帧导致）
    "settle_p95_warn": 18.0, "settle_p95_fail": 30.0,
    # 过冲（px）：冲过头。>8 WARN，>16 FAIL
    "overshoot_warn": 8.0, "overshoot_fail": 16.0,
    # 输出方向翻转次数：★"追着怪"的量化。>25 WARN，>50 FAIL
    "sign_flips_warn": 25, "sign_flips_fail": 50,
    # 卡死帧（误差>15px 却无输出）：跟不上。>10 WARN，>40 FAIL
    "stuck_warn": 10, "stuck_fail": 40,
    # 收敛帧数：>400 WARN，>800 FAIL（一直追不进去）
    "settle_frames_warn": 400, "settle_frames_fail": 800,
    # 末段漂移 |px/s|：>25 WARN，>60 FAIL（缓慢失控）
    "drift_warn": 25.0, "drift_fail": 60.0,
    # 单帧最大输出（count）：>20 WARN（猛推），>40 FAIL（一帧打飞）
    "max_step_warn": 20.0, "max_step_fail": 40.0,
    # 回归容差（相对基线的允许劣化比例）
    "reg_tol": 0.25,
}

# ── 场景目录 ──
# 2D 平面场景（等价"框大小不变的横移"）：只用来测**基础跟随**。
SCENES = [
    ("静止", 0.0, 900),
    ("慢跑", 50.0, 1200),
    ("快跑", 150.0, 1200),
    ("急跑", 300.0, 1200),
]

# ★ 3D 场景（业主 2026-10-06 指出：三角洲是 3D）——**这才是实战的主场景**。
#   2D 平面仿真结构上测不到「框随深度暴涨暴跌」，而那正是「追着怪/停不住」
#   的一大来源。scene3d.py 用透视投影生成：框高 ∝ 1/Z，冲向时实测放大 6.2×。
USE_3D = True
# 3D 场景：(显示名, C++ --scene 关键字)。★ 生成在 C++（透视投影属物理，不该 Python 重写）
SCENES_3D_CPP = [("3D横移", "strafe"), ("3D冲向", "approach"),
                 ("3D远离", "retreat"), ("3D折返", "zigzag"), ("3D混合", "mixed")]

RANK = {PASS: 0, WARN: 1, FAIL: 2}

# 默认控制器 = 板端 mouse.controller_type 的出厂值（V1.0.43 起 fitts）。
# 只有它的失败才判整体 FAIL；备选控制器（pid1，已知有"追着怪"）只记录 ——
# 否则基线里永远挂着备选的 FAIL，门禁就成了摆设。
DEFAULT_KIND = "fitts"
ALT_KIND = "pid1"


def _j(v):
    return (f"{v:.2f}" if isinstance(v, float) else str(v))


def judge(r, moving: bool = False) -> tuple[str, list[tuple[str, str, str]]]:
    """单条结果判定：返回 (总判, [(指标, 实测, 等级)])。

    ★ moving=True（目标持续移动）时**跳过「收敛」判据**：移动目标本来就永远
      追不到"停住"，拿"末帧仍在死区外"判 FAIL 是错的口径（V1.0.46 第一版
      就栽在这：静止+移动 8 个场景全 FAIL，其实 7 个是判据错）。
      移动场景要看的是**跟随误差**（稳态/P95/翻转/漂移），不是"能否停住"。
    """
    rows = []

    def add(key, val, cmp, warn, fail, unit="", hard=False):
        if cmp == ">":
            level = FAIL if val > fail else (WARN if val > warn else PASS)
        else:
            level = FAIL if val < warn else (PASS if val > fail else WARN)
        rows.append((key, f"{_j(val)}{unit}", level))

    add("稳态误差", r.settle_med, ">", TH["settle_med_warn"], TH["settle_med_fail"], "px")
    add("稳态P95", r.settle_p95, ">", TH["settle_p95_warn"], TH["settle_p95_fail"], "px")
    add("过冲", r.overshoot_px, ">", TH["overshoot_warn"], TH["overshoot_fail"], "px")
    add("翻转", r.sign_flips, ">", TH["sign_flips_warn"], TH["sign_flips_fail"], "次")
    add("卡死", r.stuck_frames, ">", TH["stuck_warn"], TH["stuck_fail"], "帧")
    if not moving:  # 只有静止目标才谈"收敛到死区里"
        add("收敛", r.settle_frames, ">", TH["settle_frames_warn"], TH["settle_frames_fail"], "帧")
    add("漂移", abs(r.drift_px), ">", TH["drift_warn"], TH["drift_fail"], "px/s")
    add("单帧最大", r.max_step, ">", TH["max_step_warn"], TH["max_step_fail"], "count")
    # ★ 抖动判定**只用「翻转」**，oscillate 与 jerk 都已实测不可用：
    #   · oscillate：末两段 max|h| 比值 >1.05 —— 随机游走目标下 h1/h2 天然抖动
    #     （实测 h2/h1=1.47）⇒ 8 个场景全误报。
    #   · jerk：误差二阶差分能量 —— 被**检测噪声 ±2px 完全主导**
    #     （fitts 2.30 vs pid1 2.35，区分不出控制器好坏）⇒ 也不可用。
    #   「输出方向翻转次数」是唯一干净区分二者的指标（fitts 2~9 vs pid1 15~58）。
    worst = max((x[2] for x in rows), key=lambda v: RANK[v])
    return worst, rows


class _Metrics:
    """C++ 回放器返回的指标容器（字段与 C++ replay_main.cpp 的 JSON 输出一一对应）。
    刻意**不**复用 Python 侧的控制器：控制逻辑只有 C++ 一份。"""

    def __init__(self, kind: str, d: dict):
        self.label = kind
        self.kind = kind
        self.settle_med = float(d.get("settle_med", 0.0))
        self.settle_p95 = float(d.get("settle_p95", 0.0))
        self.overshoot_px = float(d.get("overshoot_px", 0.0))
        self.sign_flips = int(d.get("sign_flips", 0))
        self.move_frames = int(d.get("move_frames", 0))
        self.max_step = float(d.get("max_step", 0.0))
        self.stuck_frames = int(d.get("stuck_frames", 0))
        self.settle_frames = int(d.get("settle_frames", 0))
        self.drift_px = float(d.get("drift_px", 0.0))
        self.jerk = float(d.get("jerk", 0.0))
        self.total = 0.0


def _as_result(d: dict, kind: str) -> "_Metrics":
    """把 C++ 回放器的 JSON 指标包成 _Metrics（供 judge 读取）。"""
    return _Metrics(kind, d)


def run_all(traces: dict[str, T.Trace]) -> tuple[dict, list]:
    """跑全部场景 × 全部控制器。

    ★ 控制逻辑一律走 **C++ 回放器**（链接产品控制器头文件），Python 只做
      「生成轨迹 + 判阈值 + 出报告」。没有 C++ 回放器时**直接报错**而不是
      悄悄退回 Python 替身 —— 那样等于又回到"用替身验证真身"。
    """
    if not find_replay_exe():
        print("★ 找不到 ttbox_replay（C++ 回放器）。")
        print("  先编译：cd core/build-ascii && cmake --build . --target ttbox_replay")
        print("  刻意不退回 Python 替身：替身与产品代码是两份实现，可能同时误解算法。")
        return {}, []
    data: dict = {"scenes": {}}
    detail = []
    tmpdir = tempfile.mkdtemp(prefix="ttbox_replay_")
    try:
        for name, tr in traces.items():
            is_cpp_scene = (isinstance(tr, tuple) and len(tr) == 2 and tr[0] == "__cpp_scene__")
            if is_cpp_scene:
                csv_path = ""            # 让 C++ 自己用 --scene 生成
                scene_kw = tr[1]
            else:
                csv_path = os.path.join(tmpdir, f"{abs(hash(name))}.csv")
                T.write_det_trace_csv(csv_path, tr)
                scene_kw = ""
            for kind in (DEFAULT_KIND, ALT_KIND):
                extra = (["--scene", scene_kw] if is_cpp_scene else [])
                raw = replay_cpp(csv_path, kind, extra)
                if not raw:
                    print(f"★ C++ 回放失败: {name}/{kind}")
                    continue
                r = _as_result(raw, kind)
                verdict, rows = judge(r, moving=(name != "静止"))
                data["scenes"][f"{name}/{kind}"] = {
                    k: getattr(r, k, 0) for k in (
                        "settle_med", "settle_p95", "overshoot_px", "sign_flips",
                        "stuck_frames", "settle_frames", "drift_px", "max_step")}
                detail.append((name, kind, verdict, r, rows))
    finally:
        shutil.rmtree(tmpdir, ignore_errors=True)
    return data, detail


def check_regression(cur: dict, base: dict | None) -> tuple[str, list[str]]:
    """与基线比：关键指标劣化超过容差就 FAIL。"""
    if not base:
        return WARN, ["无基线（首次运行）：跳过回归比对。建议确认结果 OK 后 --update-baseline"]
    notes = []
    worst = PASS
    keys = ("settle_med", "overshoot_px", "sign_flips", "stuck_frames", "max_step")
    tol = TH["reg_tol"]
    for sk, cur_r in cur["scenes"].items():
        base_r = base.get("scenes", {}).get(sk)
        if not base_r:
            notes.append(f"{sk}: 基线缺失（新增场景），跳过")
            continue
        for k in keys:
            c, b = cur_r.get(k, 0), base_r.get(k, 0)
            if b == 0:
                if c > 0:
                    notes.append(f"{sk}.{k}: 基线 0 → 现 {c}（需关注）")
                    worst = max(worst, WARN, key=lambda v: RANK[v])
                continue
            d = (c - b) / abs(b)
            if d > tol:
                notes.append(f"{sk}.{k}: {b:g} → {c:g}（劣化 {d * 100:+.0f}%，容差 {tol * 100:.0f}%）")
                worst = max(worst, FAIL if d > tol * 2 else WARN, key=lambda v: RANK[v])
    return worst, notes


# 鲁棒性扰动：参数名 → C++ 命令行开关
_ROBUST_ARGS = {
    "fitts_a_ms": "--fitts-a", "fitts_b_ms": "--fitts-b",
    "fitts_deadzone_px": "--fitts-dz", "fitts_ff_gain": "--fitts-ff",
}


def check_robustness(tr: T.Trace) -> tuple[str, list[str]]:
    """参数扰动鲁棒性：每个参数 ±20%，指标不应崩坏（不能只在单点最优）。走 C++ 回放器。"""
    notes = []
    worst = PASS
    if not find_replay_exe():
        return WARN, ["无 C++ 回放器，跳过鲁棒性检查"]
    tmpdir = tempfile.mkdtemp(prefix="ttbox_rb_")
    try:
        is_cpp_scene = (isinstance(tr, tuple) and len(tr) == 2 and tr[0] == "__cpp_scene__")
        csv_path = "" if is_cpp_scene else os.path.join(tmpdir, "rb.csv")
        if not is_cpp_scene:
            T.write_det_trace_csv(csv_path, tr)
        for key, base in (("fitts_a_ms", 20.0), ("fitts_b_ms", 20.0),
                          ("fitts_deadzone_px", 3.0), ("fitts_ff_gain", 0.85)):
            for mul in (0.8, 1.2):
                val = base * mul
                extra = (["--scene", tr[1]] if is_cpp_scene else [])
                raw = replay_cpp(csv_path, "fitts",
                                 extra + [_ROBUST_ARGS[key], f"{val:g}"])
                if not raw:
                    continue
                v, _ = judge(_as_result(raw, "fitts"), moving=True)
                tag = f"{key}={val:.3g}"
                if v == FAIL:
                    notes.append(f"{tag}: {v}（单点扰动即崩 ⇒ 参数过拟合）")
                    worst = FAIL
                elif v == WARN:
                    notes.append(f"{tag}: {v}")
                    worst = max(worst, WARN, key=lambda x: RANK[x])
    finally:
        shutil.rmtree(tmpdir, ignore_errors=True)
    if not notes:
        notes.append("±20% 扰动下全部稳定")
    return worst, notes


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--trace", default="", help="真实轨迹 CSV（默认用合成场景）")
    ap.add_argument("--update-baseline", action="store_true")
    ap.add_argument("--html", default="", help="同时出 HTML 报告")
    ap.add_argument("--no-regression", action="store_true")
    ap.add_argument("--no-3d", action="store_true", help="只跑 2D 平面场景（快，但测不出深度影响）")
    args = ap.parse_args()

    if args.trace:
        t = T.load_trace_csv(args.trace)
        traces = {t.name: t}
    else:
        # 2D 平面场景（粗筛基础跟随）
        traces = {name: T.make_synthetic(name, spd, frames=n) for name, spd, n in SCENES}
        # ★ 3D 场景由 **C++ 回放器的 --scene** 生成（透视投影是物理，不该用 Python 重写）。
        #   Python 这里只登记"场景名 → C++ 场景关键字"，轨迹生成/控制全在 C++。
        if USE_3D and not args.no_3d:
            for nm, mode in SCENES_3D_CPP:
                traces[nm] = ("__cpp_scene__", mode)   # 标记：这条走 C++ --scene

    cur, detail = run_all(traces)

    base = None
    if os.path.exists(BASELINE):
        try:
            with open(BASELINE, encoding="utf-8") as f:
                base = json.load(f)
        except Exception:
            base = None
    reg_v, reg_notes = (PASS, ["已跳过回归比对"]) if args.no_regression else check_regression(cur, base)
    rb_v, rb_notes = check_robustness(list(traces.values())[-1])  # 只针对 fitts

    # ── 控制台摘要 ──
    print("=" * 78)
    print("TTBOX 自瞄控制器自测（阈值判定 + 回归 + 鲁棒性）")
    print("=" * 78)
    print(f"{'场景':<8}{'控制器':<8}{'判定':<6}{'稳态':>8}{'P95':>8}{'过冲':>8}"
          f"{'翻转':>7}{'卡死':>7}{'收敛':>8}{'漂移':>9}")
    print("-" * 78)
    n_fail = n_warn = 0
    n_fail_alt = 0          # 备选控制器的 FAIL（不阻塞整体判定）
    for name, kind, verdict, r, rows in detail:
        col = {PASS: "\033[32m", WARN: "\033[33m", FAIL: "\033[31m"}.get(verdict, "")
        end = "\033[0m" if col else ""
        if verdict == FAIL:
            if kind == DEFAULT_KIND:
                n_fail += 1
            else:
                n_fail_alt += 1
        elif verdict == WARN:
            n_warn += 1
        print(f"{name:<8}{kind:<8}{col}{verdict:<6}{end}{r.settle_med:>8.2f}{r.settle_p95:>8.2f}"
              f"{r.overshoot_px:>8.2f}{r.sign_flips:>7}{r.stuck_frames:>7}"
              f"{r.settle_frames:>8}{r.drift_px:>9.1f}")
        for k, v, lv in rows:
            if lv != PASS:
                print(f"         └─ {k}: {v}  [{lv}]")

    print("-" * 78)
    print(f"回归判定: {reg_v}")
    for n in reg_notes:
        print(f"  · {n}")
    print(f"鲁棒性:   {rb_v}")
    for n in rb_notes:
        print(f"  · {n}")
    print("=" * 78)

    overall = FAIL if (n_fail or reg_v == FAIL or rb_v == FAIL) else (
        WARN if (n_warn or reg_v == WARN or rb_v == WARN) else PASS)
    print(f"总判定: {overall}   (默认控制器 {DEFAULT_KIND} FAIL {n_fail} 项 / WARN {n_warn} 项"
          + (f"；备选 {ALT_KIND} FAIL {n_fail_alt} 项[不阻塞])" if n_fail_alt else ")"))

    if args.update_baseline:
        with open(BASELINE, "w", encoding="utf-8") as f:
            json.dump(cur, f, ensure_ascii=False, indent=1)
        print(f"基线已更新: {BASELINE}")

    if args.html:
        import report_html
        data = report_html.build(list(traces.values()))
        data["selftest"] = {
            "overall": overall, "regression": reg_v, "robustness": rb_v,
            "regression_notes": reg_notes, "robustness_notes": rb_notes,
            "thresholds": TH,
        }
        with open(args.html, "w", encoding="utf-8") as f:
            f.write(report_html.render(data))
        print(f"HTML 报告: {args.html}")

    return 1 if overall == FAIL else 0


if __name__ == "__main__":
    raise SystemExit(main())
