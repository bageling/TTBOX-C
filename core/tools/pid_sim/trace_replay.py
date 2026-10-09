"""trace_replay.py — 自测脚本的**轨迹生成 + CSV 落盘**（V1.0.46 收口）

★ 定位（业主 2026-10-06 定案「代码全用 C++，只有脚本用 py」）：
  本文件**只做两件事** —— ① 生成测试轨迹；② 写成 DetTrace CSV。
  **不再实现任何控制逻辑**（那是 C++ 回放器 ttbox_replay 的事）。
  历史：V1.0.46 之前这里还有一整套 Python 版控制器（fitts.py/pid1.py）与闭环回放，
  那是"用替身验证真身"——V1.0.43 的 τ=MT 错误在两边犯一模一样的错，互验形同虚设。
  现已全部删除；控制逻辑只有一份，就是上板跑的那份 C++。

原文档（保留说明背景）：
--- 真实检测轨迹离线回放（V1.0.46）

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


# ────────────────────────────── 统计 ──────────────────────────────
# 控制器与闭环回放全部在 C++（ttbox_replay）。此处仅提供轨迹侧的描述性统计，
# 供报告展示场景规模用（控制质量指标由 selftest.py 从 C++ 回放器拿）。

def trace_summary(tr: "Trace") -> dict:
    """场景描述性统计（帧数/有目标帧/目标位移量级）—— 不含控制质量指标。"""
    if not tr.frames:
        return {"frames": 0}
    xs = [f.target_x for f in tr.frames]
    hs = [f.box_h for f in tr.frames if f.box_h > 0]
    return {
        "frames": len(tr.frames),
        "target_frames": sum(1 for f in tr.frames if f.has_target),
        "target_travel_px": round(max(xs) - min(xs), 1) if xs else 0.0,
        "box_h_min": round(min(hs), 1) if hs else 0.0,
        "box_h_max": round(max(hs), 1) if hs else 0.0,
    }
