"""trace_replay.py — 真实检测轨迹离线回放（V1.0.46）

读板端 DetTrace 录的 CSV，用**真实目标运动**驱动控制器闭环，产出可比较的指标。
这是"有目标才能测出 PID 自瞄各种问题"的落地：合成轨迹（匀速/随机游走）只能测
稳态滞后，真实轨迹里才有切靶、遮挡、抖动、变向、急停这些真正让人难受的片段。

用法
----
    # 合成轨迹（无需录制文件，CI/门禁用）
    python trace_replay.py --synthetic

    # 真实轨迹（板端开 det_trace_enabled 录一段后拉下来）
    python trace_replay.py --trace /path/det_trace.csv

    # 控制器 A/B + 参数扫描
    python trace_replay.py --trace x.csv --ab
    python trace_replay.py --trace x.csv --sweep

口径（必须与板端一致，否则回放结论不可信）
--------------------------------------------
  · 误差定义  e = 瞄准落点 − 准星参考点（AimThread.cpp:469）
  · 控制量单位 count；px 位移 = count × gain_px_per_count（只乘一次）
  · 回路延迟  response_delay_ms：板端实测 51ms（= 6.94ms × 7.3 帧 @144fps）
  · 死区判定在 **count 域**（AimThread 用 scaled，不是 px）
  · int16 量化：朝零截断，小数余量跨帧携带（remainder）
  · 检测噪声直接来自录制数据（**不再叠加**合成噪声，否则双重噪声）
"""
from __future__ import annotations

import argparse
import csv
import json
import math
import statistics
from dataclasses import dataclass, field, asdict

from fitts import Fitts
from pid1 import Pid1

# 板端标定实测（ttbox_motion/calibration.py）
GAIN_X_PX_PER_COUNT = 0.686
GAIN_Y_PX_PER_COUNT = 0.695
RESPONSE_DELAY_MS = 51.0
FRAME_MS_144FPS = 1000.0 / 144.0

# 输出链安全阀（AimThread.cpp:775-776）
HID_MAX = 32767
HID_MIN = -32768


# ────────────────────────────── 轨迹（真实 / 合成）──────────────────────────────

@dataclass
class Frame:
    """一帧观测。

    ★ target_x 是**目标绝对位置**（px，相对镜头中心），不是误差。
      回放里准星位置由本回放闭环驱动，误差 = target_x - cross_x 是**算出来的**。
      （第一版把板端录好的 err_x 直接当输入、却又让回放自己推准星，
        等于把准星位移算两遍 ⇒ 误差发散到 300~900px。语义必须先定死。）
    """
    t_ms: float
    box_w: float
    box_h: float
    target_x: float
    target_y: float = 0.0
    has_target: int = 1
    target_id: int = 0


@dataclass
class Trace:
    name: str
    frames: list[Frame] = field(default_factory=list)
    source: str = "synthetic"

    def stats(self) -> dict:
        if not self.frames:
            return {"frames": 0}
        errs = [abs(f.target_x - self.frames[0].target_x) for f in self.frames if f.has_target]
        tgt = sum(1 for f in self.frames if f.has_target)
        speeds = []
        for i in range(1, len(self.frames)):
            a, b = self.frames[i - 1], self.frames[i]
            dt = (b.t_ms - a.t_ms) / 1000.0
            if dt > 1e-6:
                speeds.append(abs(b.target_x - a.target_x) / dt)
        return {
            "frames": len(self.frames),
            "target_frames": tgt,
            "lost_frames": len(self.frames) - tgt,
            "mean_abs_err": round(statistics.mean(errs), 2) if errs else 0.0,
            "p95_abs_err": round(sorted(errs)[int(len(errs) * 0.95)], 2) if errs else 0.0,
            "max_abs_err": round(max(errs), 2) if errs else 0.0,
            "mean_speed_px_s": round(statistics.mean(speeds), 1) if speeds else 0.0,
            "max_speed_px_s": round(max(speeds), 1) if speeds else 0.0,
        }


def load_trace_csv(path: str) -> Trace:
    """读 DetTrace CSV。回放用**控制误差**列（ctrl_x/ctrl_y）——

    为什么不用框重算落点：DetTrace 已记了板端**实际用的**控制误差（含 tracker 平滑、
    预测、冻结框、box_source 分支），直接用它才能 1:1 复现板端行为；
    用框重算反而要重写一遍 AimThread 的落点算法（且容易漏掉某条分支）。
    检测框序列另存（供选靶类实验用），本文件默认不参与闭环。
    """
    tr = Trace(name=path.rsplit("/", 1)[-1], source="det_trace_csv")
    with open(path, newline="", encoding="utf-8") as f:
        rd = csv.DictReader(f)
        required = {"timestamp_us", "dt_ms", "ctrl_x", "ctrl_y"}
        missing = required - set(rd.fieldnames or [])
        if missing:
            raise SystemExit(
                f"CSV 缺少必需列 {sorted(missing)}。\n"
                f"这不是 DetTrace 录的文件（现有列: {(rd.fieldnames or [])[:8]}...）"
            )
        # 板端录的 ctrl_x 是**误差**（目标−板端准星）。要换成目标绝对位置，
        # 需减掉板端准星已走的距离：ctrl_x[i] = tgt[i] - cross[i]，
        # 而 cross[i] = Σ(板端注入量 × gain)。用 move_x 列还原：
        #     tgt[i] = ctrl_x[i] + gain × Σ_{k<i} move_x[k]
        g = GAIN_X_PX_PER_COUNT
        cross = 0.0
        for row in rd:
            tr.frames.append(Frame(
                t_ms=float(row["timestamp_us"]) / 1000.0,
                box_w=float(row.get("aim_x2") or 0) - float(row.get("aim_x1") or 0),
                box_h=float(row.get("aim_y2") or 0) - float(row.get("aim_y1") or 0),
                target_x=float(row["ctrl_x"]) + cross,
                target_y=float(row["ctrl_y"]),
                has_target=1,
                target_id=int(float(row.get("target_id") or 0)),
            ))
            cross += g * float(row.get("move_x") or 0.0)
    return tr


# DetTrace 的列定义（与 core/src/aim/DetTrace.hpp 的 open() 逐字对齐）——
# **唯一真源**。写 CSV 必须用它，别手搓：V1.0.46 手搓一次错一位（ctrl_x 落到空位）
# ⇒ 回放读到全 0，白排查半小时。
DET_TRACE_HEADER = (
    "timestamp_us,frame_number,frame_w,frame_h,dt_ms,target_id,box_source,"
    "sel_x1,sel_y1,sel_x2,sel_y2,sel_cls,sel_score,"
    "aim_x1,aim_y1,aim_x2,aim_y2,"
    "tx,ty,ref_x,ref_y,smooth_x,smooth_y,vel_x,vel_y,ctrl_x,ctrl_y,"
    "move_x,move_y,nbox"
    + "".join(f",cls{i},x1_{i},y1_{i},x2_{i},y2_{i},sc{i}" for i in range(8))
)


def write_det_trace_csv(path: str, tr: Trace, gain: float = GAIN_X_PX_PER_COUNT) -> str:
    """把 Trace 写成 DetTrace 格式 CSV（供 C++ 回放器吃）。

    move_x 一律写 0：回放器会 target_x = ctrl_x + gain×Σmove_x，
    我们这里 ctrl_x 直接填**目标绝对位置**、move_x=0 ⇒ 还原出的 target_x 就是它。
    """
    with open(path, "w", encoding="utf-8", newline="") as f:
        f.write(DET_TRACE_HEADER + "\n")
        for i, fr in enumerate(tr.frames):
            x1, y1 = 0.0, 0.0
            x2, y2 = fr.box_w, fr.box_h
            f.write(
                f"{int(fr.t_ms * 1000)},{i},640,640,{FRAME_MS_144FPS:.3f},0,0,"
                f"{x1:.2f},{y1:.2f},{x2:.2f},{y2:.2f},0,0.90,"
                f"{x1:.2f},{y1:.2f},{x2:.2f},{y2:.2f},"
                f"{fr.target_x:.2f},{fr.target_y:.2f},0,0,0,0,0,0,"
                f"{fr.target_x:.2f},{fr.target_y:.2f},0,0,1"
                + (",0,0,0,80,160,0.90" if i == 0 else ",0,0,0,0,0,0" * 8)
                + "\n"
            )
    return path


def make_synthetic(name: str, speed_px_s: float, noise_px: float = 2.0,
                   frames: int = 1400, seed: int = 7) -> Trace:
    """合成轨迹：速度随机游走（人手不规则运动）+ 检测噪声。

    为什么要有合成模式：板端录一段真轨迹要 10 分钟+；CI/门禁每���改控制器都要跑，
    必须有不依赖外部文件的基准轨迹。
    """
    st = seed
    def nxt() -> float:
        nonlocal st
        st = (1103515245 * st + 12345) & 0x7FFFFFFF
        return st / 0x7FFFFFFF

    tr = Trace(name=name, source="synthetic")
    pos = -40.0          # 目标绝对位置（准星从 0 出发 ⇒ 初始误差 -40px）
    vel = speed_px_s if speed_px_s else 0.0
    for i in range(frames):
        if speed_px_s:
            vel += (nxt() * 2.0 - 1.0) * speed_px_s * 0.12
            vel = max(-speed_px_s * 1.6, min(speed_px_s * 1.6, vel))
        pos += vel * (FRAME_MS_144FPS / 1000.0)
        tr.frames.append(Frame(
            t_ms=i * FRAME_MS_144FPS,
            box_w=80.0, box_h=160.0,
            target_x=pos + (nxt() * 2.0 - 1.0) * noise_px,
            target_y=(nxt() * 2.0 - 1.0) * noise_px,
        ))
    return tr


# ────────────────────────────── 控制器 ──────────────────────────────

@dataclass
class CtlCfg:
    kind: str = "fitts"                 # fitts | pid1
    # Fitts
    fitts_a_ms: float = 20.0
    fitts_b_ms: float = 20.0
    fitts_deadzone_px: float = 3.0
    fitts_deadzone_ratio: float = 0.02
    fitts_ff_gain: float = 0.85
    fitts_ff_tau_ms: float = RESPONSE_DELAY_MS
    # pid1
    kp: float = 25.0
    kd: float = 25.0
    predict: float = 0.5
    rate: float = 0.3
    smooth: float = 9900.0
    # 公共
    deadzone_count: float = 1.0
    gain: float = GAIN_X_PX_PER_COUNT
    response_delay_ms: float = RESPONSE_DELAY_MS

    def make(self):
        if self.kind == "pid1":
            return Pid1(self.kp, self.kd, self.predict, self.rate, self.smooth)
        return Fitts(self.fitts_a_ms, self.fitts_b_ms, self.fitts_deadzone_px,
                     self.fitts_deadzone_ratio, self.fitts_ff_gain, self.fitts_ff_tau_ms)


@dataclass
class Result:
    label: str
    kind: str
    settle_med: float = 0.0
    settle_p95: float = 0.0
    overshoot_px: float = 0.0     # 首次穿越目标后的最大过冲
    oscillate: bool = False
    sign_flips: int = 0          # 输出方向翻转次数（>阈值 = 抖）
    move_frames: int = 0
    max_step: float = 0.0
    stuck_frames: int = 0         # 误差大却没输出（追不上）
    settle_frames: int = 0        # 收敛耗时：首次进入死区且此后不再出去（0=未收敛）
    drift_px: float = 0.0         # 末段误差线性漂移（px/s，正=越追越远；识别"缓慢失控"）
    jerk: float = 0.0             # ★ 误差二阶差分能量（px）：控制器自身抖动量。
                                   #   比 oscillate 判据可靠：后者用「末两段 max|h| 比值」，
                                   #   在**随机游走目标**下 h1/h2 天然抖动（实测 1.47）⇒ 误报。
    total: float = 0.0
    errs: list = field(default_factory=list)
    outs: list = field(default_factory=list)

    def brief(self) -> dict:
        d = asdict(self)
        d.pop("errs", None)
        d.pop("outs", None)
        return d


def replay(tr: Trace, cfg: CtlCfg, label: str = "") -> Result:
    """闭环回放。

    ★ 闭环必须自己积分「准星位置」（V1.0.46 踩过的坑）：
      录制数据里的 err_x 是**板端当时**的误差（准星已经跟着移动过了）。
      回放要回答「换成另一个控制器会怎样」，就得把准星位置当成真实状态驱动：
          目标位置 target_pos（由录制序列推进）
          准星位置 cross_pos（从 0 起，只由本回放的输出驱动）
          观测误差   obs_err = target_pos - cross_pos
      第一版写成 `live_err = fr.err_x - applied`，等于每帧把准星位置丢掉重置
      ⇒ 准星永远不动 ⇒ 稳态误差发散到 250~850px（还被误判成「振荡」）。
    """
    ctl = cfg.make()
    delay_n = max(0, int(round(cfg.response_delay_ms / FRAME_MS_144FPS)))
    queue: list[float] = [0.0] * delay_n
    rem = 0.0
    cross_pos = 0.0        # 准星绝对位置（本回放闭环驱动）
    target_pos = 0.0       # 目标绝对位置（由录制序列推进）
    vel_est = 0.0
    errs: list[float] = []
    outs: list[float] = []
    sign_prev = 0
    flips = 0
    over = 0.0
    stuck = 0
    crossed = False

    for i, fr in enumerate(tr.frames):
        obs_err = target_pos - cross_pos
        # ★ 速度估计必须用**目标位置**的速度（板端 AimTracker 就是跟目标框，不是跟误差）。
        #   用「含自身输出的观测误差」差分 = 把自己的动作算进速度 ⇒ 正反馈。
        if i > 0:
            dt = (fr.t_ms - tr.frames[i - 1].t_ms) / 1000.0
            if dt > 1e-6:
                v = (fr.target_x - tr.frames[i - 1].target_x) / dt
                vel_est += 0.5 * (v - vel_est)

        if cfg.kind == "pid1":
            u = ctl.upd(obs_err)
        else:
            u = ctl.upd(obs_err, fr.box_h or 0.0, cfg.gain, vel_est, FRAME_MS_144FPS)
        if not math.isfinite(u):
            u = 0.0
        if abs(u) < cfg.deadzone_count:
            u = 0.0
        rem += u
        move = 0.0
        if abs(rem) >= 1.0:
            move = float(int(rem))
            rem -= move
        move = max(HID_MIN, min(HID_MAX, move))
        # 回路：位移经延迟队列才生效 ⇒ 延迟内准星不动（滞后的物理来源）
        queue.append(move * cfg.gain)
        applied = queue.pop(0) if queue else 0.0
        cross_pos += applied
        errs.append(obs_err)
        outs.append(move)

        # 目标推进：直接用录制/合成的**目标绝对位置**（语义已在 Frame 里定死）
        target_pos = fr.target_x

        # 过冲：误差穿零后的最大反向距离
        if not crossed and i > 3 and abs(obs_err) < 4.0:
            crossed = True
        if crossed and i > 3 and (obs_err < 0) != (errs[-2] < 0):
            over = max(over, abs(obs_err))
        s = 1 if move > 0 else (-1 if move < 0 else 0)
        if s and sign_prev and s != sign_prev:
            flips += 1
        if s:
            sign_prev = s
        if abs(obs_err) > 15.0 and move == 0.0:
            stuck += 1

    # 收敛帧数：从头扫，找最后一个 |err| > 死区基准的帧，其后即"已收敛"
    # （死区基准取控制器死区：Fitts 用 min(绝对, 框高×ratio) 的近似 3px，pid1 用 0.5px）
    dz_base = cfg.deadzone_count * cfg.gain if cfg.kind == "pid1" else 3.0
    settle_frames = 0
    for k in range(len(errs)):
        if abs(errs[k]) > max(dz_base, 2.0):
            settle_frames = k + 1
    # 末段漂移：线性回归斜率 ×1000（px/s）——识别"慢慢追不上"这种静态指标看不出来的问题
    drift_px = 0.0
    m = len(errs) // 4
    if m > 4:
        seg = errs[-m:]
        xm = (m - 1) / 2.0
        ym = sum(seg) / m
        num = sum((i - xm) * (seg[i] - ym) for i in range(m))
        den = sum((i - xm) ** 2 for i in range(m))
        if den > 1e-9:
            drift_px = (num / den) * 1000.0

    # 误差二阶差分能量：|e[i] - 2e[i-1] + e[i-2]| 的均值
    # （去掉目标自身运动的贡献后，剩下的就是控制器造成的抖动）
    jerk = 0.0
    if len(errs) > 8:
        m = len(errs) // 2
        seg = errs[-m:]
        jerk = statistics.mean(abs(seg[i] - 2 * seg[i - 1] + seg[i - 2])
                              for i in range(2, len(seg)))

    n3 = max(30, len(errs) // 3)
    tail = [abs(x) for x in errs[-n3:]]
    tailo = outs[-n3:]
    q = max(1, len(errs) // 4)
    h1 = max((abs(x) for x in errs[-2 * q:-q]), default=0.0)
    h2 = max((abs(x) for x in errs[-q:]), default=0.0)
    return Result(
        label=label or cfg.kind,
        kind=cfg.kind,
        settle_med=round(statistics.median(tail), 2) if tail else 0.0,
        settle_p95=round(sorted(tail)[int(len(tail) * 0.95)], 2) if tail else 0.0,
        overshoot_px=round(over, 2),
        oscillate=bool(h2 > h1 * 1.05 and h2 > cfg.gain * 2.0),
        sign_flips=flips,
        move_frames=sum(1 for o in tailo if abs(o) >= 1.0),
        max_step=round(max((abs(o) for o in tailo), default=0.0), 2),
        stuck_frames=stuck,
        settle_frames=settle_frames,
        drift_px=round(drift_px, 1),
        jerk=round(jerk, 3),
        total=round(statistics.mean([abs(x) for x in errs[-200:]]), 2) if errs else 0.0,
        errs=[round(x, 2) for x in errs],
        outs=[round(x, 2) for x in outs],
    )


def sweep(tr: Trace, key: str, values: list[float], base: CtlCfg) -> list[dict]:
    """单参数扫描：每个取值跑一次闭环，产出可直接画图/画表的行。"""
    rows = []
    for v in values:
        cfg = CtlCfg(**{**asdict(base)})
        setattr(cfg, key, v)
        r = replay(tr, cfg, label=f"{key}={v:g}")
        row = r.brief()
        row["value"] = v
        rows.append(row)
    return rows


# ────────────────────────────── 报告 ──────────────────────────────

def build_report(traces: list[Trace]) -> dict:
    """跑「合成三档速度 + A/B + 参数扫描」，产出 HTML 用的数据。"""
    scenes = []
    for name, spd in (("静止", 0.0), ("慢跑 50", 50.0), ("快跑 150", 150.0)):
        tr = make_synthetic(name, spd)
        pid1 = replay(tr, CtlCfg(kind="pid1"), "pid1")
        fitts = replay(tr, CtlCfg(kind="fitts"), "fitts")
        scenes.append({
            "name": name,
            "stats": tr.stats(),
            "results": [pid1.brief(), fitts.brief()],
            "series": {
                "errs": {"pid1": pid1.errs[:600], "fitts": fitts.errs[:600]},
            },
        })
    # 参数扫描（用快跑场景）
    tr = make_synthetic("快跑 150", 150.0)
    scans = {
        "fitts_ff_gain": sweep(tr, "fitts_ff_gain", [0.0, 0.3, 0.6, 0.85], CtlCfg()),
        "fitts_deadzone_px": sweep(tr, "fitts_deadzone_px", [1.0, 2.0, 3.0, 5.0], CtlCfg()),
        "fitts_a_ms": sweep(tr, "fitts_a_ms", [8.0, 12.0, 20.0, 30.0], CtlCfg()),
        "fitts_b_ms": sweep(tr, "fitts_b_ms", [10.0, 20.0, 30.0, 40.0], CtlCfg()),
    }
    return {
        "generated_by": "trace_replay.py",
        "physics": {
            "gain_px_per_count": GAIN_X_PX_PER_COUNT,
            "response_delay_ms": RESPONSE_DELAY_MS,
            "frame_ms": round(FRAME_MS_144FPS, 3),
        },
        "scenes": scenes,
        "scans": scans,
    }


def main() -> int:
    ap = argparse.ArgumentParser(description="真实轨迹闭环回放")
    ap.add_argument("--trace", help="板端 DetTrace CSV（不给则用合成轨迹）")
    ap.add_argument("--out", default="", help="导出 JSON 报告路径")
    ap.add_argument("--ab", action="store_true", help="控制器 A/B")
    ap.add_argument("--sweep", action="store_true", help="参数扫描")
    ap.add_argument("--html", default="", help="导出 HTML 报告路径")
    args = ap.parse_args()

    traces = []
    if args.trace:
        tr = load_trace_csv(args.trace)
        print(f"读入真实轨迹: {tr.name}  帧数={len(tr.frames)}")
        print(f"  统计: {tr.stats()}")
        traces.append(tr)
    else:
        for name, spd in (("静止", 0.0), ("慢跑 50", 50.0), ("快跑 150", 150.0)):
            traces.append(make_synthetic(name, spd))

    for tr in traces:
        print(f"\n=== {tr.name} ===")
        p1 = replay(tr, CtlCfg(kind="pid1"), "pid1")
        fi = replay(tr, CtlCfg(kind="fitts"), "fitts")
        for r in (p1, fi):
            d = r.brief()
            print(f"  {d['label']:<6} 稳态{d['settle_med']:>7.2f}px "
                  f"P95{d['settle_p95']:>7.2f} 过冲{d['overshoot_px']:>6.2f} "
                  f"翻转{d['sign_flips']:>4d} 卡死{d['stuck_frames']:>4d} "
                  f"{'振荡!' if d['oscillate'] else '稳'}")
        if p1.settle_med > 0:
            print(f"  ⇒ fitts 稳态误差 {p1.settle_med:.2f} → {fi.settle_med:.2f} px "
                  f"（{(p1.settle_med - fi.settle_med) / p1.settle_med * 100:+.0f}%）")

    if args.sweep:
        tr = traces[-1]
        print(f"\n=== 参数扫描（场景 {tr.name}）===")
        for key, vals in (("fitts_ff_gain", [0.0, 0.3, 0.6, 0.85]),
                          ("fitts_deadzone_px", [1.0, 2.0, 3.0, 5.0])):
            print(f"  {key}:")
            for row in sweep(tr, key, vals, CtlCfg()):
                print(f"    {row['value']:>6g}  稳态{row['settle_med']:>7.2f} "
                      f"翻转{row['sign_flips']:>4d} 卡死{row['stuck_frames']:>4d}")

    if args.out:
        with open(args.out, "w", encoding="utf-8") as f:
            json.dump({"physics": {"gain": GAIN_X_PX_PER_COUNT, "delay": RESPONSE_DELAY_MS},
                       "scenes": [{"name": t.name, "stats": t.stats(),
                                   "results": [replay(t, CtlCfg(kind="pid1"), "pid1").brief(),
                                               replay(t, CtlCfg(kind="fitts"), "fitts").brief()]}
                                  for t in traces]}, f, ensure_ascii=False, indent=1)
        print(f"\nJSON 报告: {args.out}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
