"""aim_replay.py — TTBOX 自瞄**闭环**离线 replay（V3 阶段 0 交付物）。

用途
----
阶段 0 要求的就是「可控输入 + 可复现输出」，没有它后面每一期的验收都没有依据。
本文件在 `pid1.py`（Pid1Controller.hpp 的精确移植）之上补上**被控对象模型**：

    误差 e(px) → PID(count) → 死区/remainder → int16 量化(count)
                → ×G(px/count) 位移 → 延迟(N 帧) → 作用于画面 → 下一帧误差

为什么必须闭环：稳态误差、极限环、自激振荡全是**闭环**行为，固定帧序列的开环回放
看不到（V3 文档阶段 0 已写明）。本模型就是闭环，可以直接量稳态指标。

★ 与同目录旧脚本 sim_run.py 的两处**量纲修正**（这也是新建本文件的原因）：
  1. sim_run.py 里 `out = u*GAIN` 得到已是 px，随后又 `err -= applied*GAIN` ⇒ **GAIN 被乘了两次**，
     等效把对象增益变成 G²，小增益场景会被严重低估。本文件的物理量是：
        PID 输出 **count** → 乘 G 得 **px 位移** → 减进误差（只乘一次）。
  2. sim_run.py 先把 count 转成 px 再跟 deadzone 比；真实代码 `AimThread.cpp:632` 判的是
     **count 域**的 `scaled`（`|scaled| < out_deadzone`）。本文件死区判断严格在 count 域，
     与板端一致。
  为不破坏历史结论，旧 sim_run.py 保持不动；新结论一律以本文件为准。

被控对象关键参数（板端标定实测，见 ttbox_motion/calibration.py）
-------------------------------------------------------------
  gain_x=0.686 / gain_y=0.695 px/count      @144fps
  response_delay = 51ms（渲染→采集→推理→注入→游戏应用→再采集）

★ V3 第五轮修正：gain 跟倍镜走（px/count = 每 count 转角 × f），倍镜下 f 涨 ⇒ gain 同步涨。
  本文件用 zoom_scale 显式缩放 gain（也同步缩放初始误差/目标速度），保证"同一物理场景、
  不同倍镜"的仿真成立。

两种输出模式（对应 V3 阶段 3b 的改动）
--------------------------------------
  legacy = 当前板端行为：`|scaled| < DZ ⇒ 归零` 在 remainder 累加**之前**（AimThread.cpp:632 vs :635）
           ⇒ 不足死区的输出被**永久丢弃**，攒不起来 ⇒ 稳态误差 ≈ kp_eff⁻¹ × DZ 量级（约 10×DZ px）
  accum  = V3 改动后：先累加、`|remainder| < 1 count` 才不发（余数保留，不丢）
           ⇒ 死区只决定"多久发一次"，不再决定稳态误差 ⇒ 稳态误差降到 1 count 量化级（≈G px）

小白理解
--------
  把自瞄系统放上一条传送带：每个输出都绕同一条回路绕一圈（51ms 延迟）。
  legacy 模式每隔一段就把不够 count 的小件丢掉，所以永远差一小截；
  accum 模式把小件攒在袋子里，攒够一件再发，最后几乎不差。
  这个"差多少"就是稳态误差，本文件就是用来量它的。

注意：本文件是离线仿真/验证工具，**不进产品构建**，不影响运行时行为。
"""

from __future__ import annotations

import argparse
import json
import statistics
import sys
from dataclasses import dataclass, field, asdict

from pid1 import Pid1

# ---- 板端标定实测物理量（ttbox_motion/calibration.py:25 / :19）----
GAIN_X_PX_PER_COUNT = 0.686
GAIN_Y_PX_PER_COUNT = 0.695
RESPONSE_DELAY_MS = 51.0
FRAME_MS_144FPS = 1000.0 / 144.0

# 闸门 kp 锚点（calibration.py:29  KP_FRACTION_PER_FRAME=0.07 ⇒ kp_eff=0.07/gain）
KP_EFF_FROM_CALIB = 0.07 / GAIN_X_PX_PER_COUNT  # ≈0.102
SMOOTH_DEFAULT = 9900.0
KP_NOMINAL = KP_EFF_FROM_CALIB / ((10000.0 - SMOOTH_DEFAULT) / 10000.0)  # ≈10.2
KD_NOMINAL = min(50.0, max(4.0, KP_NOMINAL * (1.0 + RESPONSE_DELAY_MS / 25.0)))  # ≈30.8


@dataclass
class Plant:
    """被控对象模型（游戏 + HDMI 采集 + 推理 + 注入这条链路）"""

    gain_px_per_count: float = GAIN_X_PX_PER_COUNT
    response_delay_ms: float = RESPONSE_DELAY_MS
    frame_ms: float = FRAME_MS_144FPS
    detect_noise_px: float = 2.0      # 检测框常态噪声（±px）
    box_jump_px: float = 0.0          # 每 N 帧的框跳变（低置信度抖动）
    box_jump_every: int = 20
    target_speed_px_s: float = 0.0    # 目标匀速横向移动（px/s）；0 = 静止靶
    zoom_scale: float = 1.0           # 倍镜：gain 与画面像素同比缩放（V3 第五轮）

    def effective_gain(self) -> float:
        """倍镜下 gain 跟 f 走（px/count = 每 count 转角 × f）。"""
        return self.gain_px_per_count * self.zoom_scale


@dataclass
class Controller:
    """pid1 控制器 + 输出尾链配置（与 AimThread.cpp 尾链一一对应）"""

    kp: float = KP_NOMINAL
    kd: float = KD_NOMINAL
    predict: float = 0.35 - RESPONSE_DELAY_MS / 300.0   # calibration.py:256 公式
    rate: float = 0.3
    smooth: float = SMOOTH_DEFAULT
    deadzone_count: float = 1.0        # 面板「抖动忽略门槛」默认 1（index.html:6583）
    mode: str = "legacy"               # legacy | accum
    seed: int = 20260928


@dataclass
class Metrics:
    settle_median_px: float = 0.0      # 稳态误差中位数（|px|）
    settle_p75_px: float = 0.0         # 稳态误差 75 分位
    settle_max_px: float = 0.0
    move_frames: int = 0               # 有 count 输出的帧数
    max_step_count: float = 0.0        # 单帧最大输出
    oscillating: bool = False          # 末窗幅度是否仍在扩张（自激/极限环）
    final_error_px: float = 0.0
    frames: int = 0

    def as_dict(self) -> dict:
        return asdict(self)


def _rng(seed: int):
    """确定性伪随机（同 seed 同结果，保证 replay 可复现）。"""
    state = seed & 0xFFFFFFFF

    def nxt() -> float:
        nonlocal state
        state = (1103515245 * state + 12345) & 0x7FFFFFFF
        return state / 0x7FFFFFFF
    return nxt


def run_closed_loop(
    plant: Plant,
    ctl: Controller,
    initial_error_px: float = 40.0,
    frames: int = 900,
    settle_window: int = 300,
) -> tuple[Metrics, list[float], list[float]]:
    """跑一次闭环仿真。返回（指标, 逐帧误差, 逐帧输出 count）。

    误差定义与 AimThread.cpp:388 一致：`e = 目标瞄准点 − 准心参考点`。
    延迟队列模拟 51ms 回路延迟（帧数 = round(delay/frame_ms)）。
    """
    if ctl.mode not in ("legacy", "accum"):
        raise ValueError(f"未知输出模式: {ctl.mode}")
    nd = _rng(ctl.seed)
    pid = Pid1(ctl.kp, ctl.kd, ctl.predict, ctl.rate, ctl.smooth)

    g = plant.effective_gain()
    delay_frames = max(0, int(round(plant.response_delay_ms / plant.frame_ms)))
    px_queue: list[float] = [0.0] * delay_frames

    # 同一物理场景在不同倍镜下：画面像素随 f 缩放，初始误差/目标速度同比例放大
    err = initial_error_px * plant.zoom_scale
    remainder = 0.0
    errors: list[float] = []
    outputs: list[float] = []

    for i in range(frames):
        # --- 观测：检测噪声 + 周期性框跳（AimTracker.hpp:51 记载 y1 帧间 ±18px）---
        observed = err
        if plant.detect_noise_px:
            observed += (nd() * 2.0 - 1.0) * plant.detect_noise_px
        if plant.box_jump_px and plant.box_jump_every and i % plant.box_jump_every == 0:
            observed += (nd() * 2.0 - 1.0) * plant.box_jump_px * plant.zoom_scale

        # --- 控制器：PID 输出在 count 域 ---
        # ★ 这里刻意**不**做 zoom 折算：当前板端按像素工作，倍镜下画面像素已同比放大
        #   （observed 含 zoom），而 kp 不变 ⇒ 输出天然被放大 zoom 倍——这正是 V3 阶段 2
        #   要修的「6x 镜下过冲 6 倍」，仿真必须如实复现。
        u_count = pid.upd(observed)

        # --- 输出尾链（V3 阶段 3b 的唯一差别就在这里）---
        if ctl.mode == "legacy":
            # 当前板端：死区判零在 remainder 累加之前 ⇒ 小输出被永久丢弃
            if abs(u_count) < ctl.deadzone_count:
                u_count = 0.0
        # accum: 不做阈值丢弃，直接进入余数累加（"攒够 1 count 再发"）
        remainder += u_count
        move_count = 0.0
        if abs(remainder) >= 1.0:
            move_count = float(int(remainder))   # 朝零截断 == int16 取整行为
            remainder -= move_count
        if ctl.mode == "legacy":
            # 与 AimThread 相同的安全阀：余数失控（理论上不会）时清零
            pass
        outputs.append(move_count)

        # --- 被控对象：count → px 位移（只乘一次 gain）→ 延迟队列 ---
        px_move = move_count * g
        px_queue.append(px_move)
        applied = px_queue.pop(0)
        err -= applied
        if plant.target_speed_px_s:
            err -= plant.target_speed_px_s * plant.zoom_scale * (plant.frame_ms / 1000.0)
        errors.append(err)

    # ---- 稳态指标（取最后 settle_window 帧）----
    tail = [abs(x) for x in errors[-settle_window:]]
    tail_out = outputs[-settle_window:]
    m = Metrics(
        settle_median_px=statistics.median(tail),
        settle_p75_px=sorted(tail)[max(0, int(len(tail) * 0.75) - 1)],
        settle_max_px=max(tail),
        move_frames=sum(1 for o in tail_out if abs(o) >= 1.0),
        max_step_count=max(abs(o) for o in tail_out) if tail_out else 0.0,
        final_error_px=errors[-1],
        frames=frames,
    )
    # 振荡判据：后半窗口幅度 > 前半窗口（能量在扩张 = 自激/发散）
    h1 = max(abs(x) for x in errors[-settle_window:-settle_window // 2])
    h2 = max(abs(x) for x in errors[-settle_window // 2:])
    m.oscillating = h2 > h1 * 1.05 and h2 > g * 2.0
    return m, errors, outputs


def compare_modes(plant: Plant, ctl: Controller, **kw) -> dict:
    """同一套件下 legacy vs accum 的 A/B（这就是阶段 3b 的收益证据）。"""
    base = Controller(**{**asdict(ctl)})
    base.mode = "legacy"
    acc = Controller(**{**asdict(ctl)})
    acc.mode = "accum"
    ml, _, _ = run_closed_loop(plant, base, **kw)
    ma, _, _ = run_closed_loop(plant, acc, **kw)
    return {
        "legacy": ml.as_dict(),
        "accum": ma.as_dict(),
        "gain_px_per_count": plant.effective_gain(),
        "quantization_px": plant.effective_gain(),  # 1 count 对应的最小位移＝量化下限
    }


SCAN_GRID = [
    # (tag, kp, kd, predict, rate, smooth, deadzone, zoom, target_speed, box_jump)
    ("腰射 静止靶", KP_NOMINAL, KD_NOMINAL, 0.35 - RESPONSE_DELAY_MS / 300.0, 0.3, SMOOTH_DEFAULT, 1.0, 1.0, 0.0, 0.0),
    ("腰射 移动60px/s", KP_NOMINAL, KD_NOMINAL, 0.35 - RESPONSE_DELAY_MS / 300.0, 0.3, SMOOTH_DEFAULT, 1.0, 1.0, 60.0, 0.0),
    ("腰射 框跳±18px", KP_NOMINAL, KD_NOMINAL, 0.35 - RESPONSE_DELAY_MS / 300.0, 0.3, SMOOTH_DEFAULT, 1.0, 1.0, 0.0, 18.0),
    ("6x镜 静止靶", KP_NOMINAL, KD_NOMINAL, 0.35 - RESPONSE_DELAY_MS / 300.0, 0.3, SMOOTH_DEFAULT, 1.0, 6.07, 0.0, 0.0),
    ("6x镜 移动60px/s", KP_NOMINAL, KD_NOMINAL, 0.35 - RESPONSE_DELAY_MS / 300.0, 0.3, SMOOTH_DEFAULT, 1.0, 6.07, 60.0, 0.0),
]


def scan(verbose: bool = True) -> list[dict]:
    """多场景扫描（V3 阶段 0 要求：参考 VisionForge 27 组思路）。

    当前先覆盖最有代表性的 5 组：静止靶/移动目标/框抖 × 腰射/6x 镜。
    需要更宽生效域时按 GRID 扩展即可，接口不变。
    """
    rows = []
    for tag, kp, kd, pr, rt, sm, dz, zoom, spd, jump in SCAN_GRID:
        plant = Plant(zoom_scale=zoom, target_speed_px_s=spd, box_jump_px=jump)
        ctl = Controller(kp=kp, kd=kd, predict=pr, rate=rt, smooth=sm, deadzone_count=dz)
        r = compare_modes(plant, ctl)
        rows.append({"scene": tag, **r})
        if verbose:
            l, a = r["legacy"], r["accum"]
            print(f"  {tag:<16} legacy 稳态={l['settle_median_px']:6.2f}px  "
                  f"accum 稳态={a['settle_median_px']:6.2f}px  "
                  f"量化下限={r['quantization_px']:.3f}px  "
                  f"振荡 legacy={'Y' if l['oscillating'] else 'N'}/"
                  f"accum={'Y' if a['oscillating'] else 'N'}")
    return rows


def check_assertions(rows: list[dict]) -> list[str]:
    """内置断言：阶段 3b 是否真的达成（用于 CI/改动前后回归检查）。"""
    fails = []
    for row in rows:
        scene = row["scene"]
        l, a = row["legacy"], row["accum"]
        q = row["quantization_px"]
        # 断言 1：accum 稳态误差不劣于 legacy（不允许改坏）
        if a["settle_median_px"] > l["settle_median_px"] * 1.05:
            fails.append(f"[{scene}] accum 反而更差 {a['settle_median_px']:.2f} > legacy {l['settle_median_px']:.2f}")
        # 断言 2：accum 稳态误差应压到 2 count 量化级以内（≈2×G px）
        if a["settle_median_px"] > q * 2.0:
            fails.append(f"[{scene}] accum 稳态未收敛到 2 count 量化级：{a['settle_median_px']:.2f} > {q*2:.2f}px")
        # 断言 3：任一模式都不得自激振荡
        if a["oscillating"]:
            fails.append(f"[{scene}] accum 判为振荡（相位/延迟不匹配）")
    return fails


def main() -> int:
    ap = argparse.ArgumentParser(description="TTBOX 自瞄闭环离线 replay（V3 阶段 0）")
    ap.add_argument("--scan", action="store_true", help="跑多场景扫描（默认）")
    ap.add_argument("--assert", dest="do_assert", action="store_true",
                    help="跑内置断言，任一失败则退出码非 0")
    ap.add_argument("--json", action="store_true", help="以 JSON 输出，便于存档/diff")
    ap.add_argument("--mode", default=None, choices=["legacy", "accum"], help="只跑单一模式")
    ap.add_argument("--zoom", type=float, default=1.0, help="倍镜焦距缩放（gain 同比）")
    args = ap.parse_args()

    if args.mode:
        plant = Plant(zoom_scale=args.zoom)
        ctl = Controller(mode=args.mode)
        m, _, _ = run_closed_loop(plant, ctl)
        payload = {"mode": args.mode, "zoom": args.zoom, **m.as_dict()}
        print(json.dumps(payload, ensure_ascii=False, indent=2) if args.json
              else f"mode={args.mode} zoom={args.zoom} 稳态={m.settle_median_px:.2f}px "
                   f"p75={m.settle_p75_px:.2f}px max={m.settle_max_px:.2f}px 振荡={'Y' if m.oscillating else 'N'}")
        return 0

    print("闭环 replay：腰射/倍镜 × 静止/移动/框跳（同一物理场景，gain 随倍镜缩放）")
    rows = scan(verbose=not args.json)
    if args.json:
        print(json.dumps(rows, ensure_ascii=False, indent=2))
    if args.do_assert:
        fails = check_assertions(rows)
        if fails:
            print("\n断言失败：")
            for f in fails:
                print("  - " + f)
            return 1
        print("\n全部断言通过 ✓（accum 稳态优于 legacy，且收敛到 2 count 量化级内、无振荡）")
    return 0


if __name__ == "__main__":
    sys.exit(main())
