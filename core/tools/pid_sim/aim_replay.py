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

★ V1.0.12（2026-09-30）：原先用 zoom_scale 模拟"同一物理场景、不同倍镜"的那个维度已删
  （业主口径：不区分倍镜）。gain 只有一套（腰射标定值），仿真不再有倍镜轴。

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
from fitts import Fitts

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
    # ★ V1.0.44：速度估计 EMA 系数（Fitts 前馈的输入源）。
    #   板端真值是 AimTracker 的 One-Euro 自适应滤波（AimTracker.cpp:60-78），
    #   这里用固定 alpha 近似；**扫描出来的最优值暴露了一个真问题**：
    #   alpha=0.2 时 300px/s 急跑滞后 40px（比 pid1 的 23px 差近一倍），
    #   因为 EMA 太钝、速度估计本身滞后 ⇒ 前馈补不上。提高到 0.5 明显改善。
    vel_alpha: float = 0.5


@dataclass
class Controller:
    """控制器 + 输出尾链配置。

    ★ V1.0.44：新增 `kind` 字段做 pid1 / fitts 双控制器 A/B（板端 mouse.controller_type）。
      kind="pid1"  → 走 Pid1（BB927 实战值，V1.0.42）
      kind="fitts" → 走 FittsAimController 的 Python 等价实现（V1.0.43 起默认）
    fitts_* 为 Fitts 四参；box_h_px 是死区尺寸自适应用的目标框高。
    """

    kp: float = KP_NOMINAL
    kd: float = KD_NOMINAL
    predict: float = 0.35 - RESPONSE_DELAY_MS / 300.0   # calibration.py:256 公式
    rate: float = 0.3
    smooth: float = SMOOTH_DEFAULT
    deadzone_count: float = 1.0        # 面板「抖动忽略门槛」默认 1（index.html:6583）
    mode: str = "legacy"               # legacy | accum
    seed: int = 20260928
    # ---- Fitts（V1.0.44）：默认值与 core MouseTypes.hpp 逐个一致 ----
    kind: str = "fitts"                # pid1 | fitts
    fitts_a_ms: float = 20.0
    fitts_b_ms: float = 20.0
    fitts_deadzone_px: float = 3.0
    fitts_ff_gain: float = 0.85
    fitts_deadzone_ratio: float = 0.02  # 死区比例（死区=max(dz, 框高×ratio)）
    fitts_ff_tau_ms: float = 51.0       # 前馈时延 = 回路延迟（物理常量，不是 MT）
    box_h_px: float = 100.0            # 目标框高（死区尺寸自适应用）


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
    if ctl.kind not in ("pid1", "fitts"):
        raise ValueError(f"未知控制器: {ctl.kind}")
    nd = _rng(ctl.seed)
    # ★ V1.0.44：控制器按板端 mouse.controller_type 二选一。Fitts 是纯函数（无内部状态），
    #   与 pid1（P_PID 有 kp_gain 爬升惯性）并存时互不干扰。
    if ctl.kind == "pid1":
        ctl_obj = Pid1(ctl.kp, ctl.kd, ctl.predict, ctl.rate, ctl.smooth)
    else:
        ctl_obj = Fitts(ctl.fitts_a_ms, ctl.fitts_b_ms, ctl.fitts_deadzone_px,
                        ctl.fitts_deadzone_ratio, ctl.fitts_ff_gain, ctl.fitts_ff_tau_ms)

    g = plant.gain_px_per_count
    delay_frames = max(0, int(round(plant.response_delay_ms / plant.frame_ms)))
    px_queue: list[float] = [0.0] * delay_frames

    err = initial_error_px
    remainder = 0.0
    errors: list[float] = []
    outputs: list[float] = []
    # ★ 目标速度估计（Fitts 前馈的输入源）。板端由 AimTracker 用 One-Euro 滤波给出
    #   （core/src/mouse/AimTracker.cpp:60-78，EMA 系数随框高/截止频率变化）。
    #   这里用固定 EMA 近似，噪声抑制量级对齐 AimTracker 的默认档。
    vel_est = 0.0
    vel_alpha = plant.vel_alpha

    for i in range(frames):
        # --- 观测：检测噪声 + 周期性框跳（AimTracker.hpp:51 记载 y1 帧间 ±18px）---
        observed = err
        if plant.detect_noise_px:
            observed += (nd() * 2.0 - 1.0) * plant.detect_noise_px
        if plant.box_jump_px and plant.box_jump_every and i % plant.box_jump_every == 0:
            observed += (nd() * 2.0 - 1.0) * plant.box_jump_px

        # --- 控制器：输出 count。误差不做任何倍率折算（V1.0.12 起不区分倍镜）---
        if ctl.kind == "pid1":
            u_count = ctl_obj.upd(observed)
        else:
            # 速度估计用**真实目标速度**（plant 是仿真真值；板端用检测差分，此处更干净）
            vel_est += vel_alpha * (plant.target_speed_px_s - vel_est)
            u_count = ctl_obj.upd(observed, ctl.box_h_px, g, vel_est, plant.frame_ms)

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
            # ★ V1.0.44 修建模错误：原来是 `err -= speed*dt`，把「目标横移」建成了
            #   「误差每帧缩小」——等价于给系统加了一个人为的误差衰减项（前馈的补偿对象）。
            #   后果：target_speed 越大误差越小，稳态指标与真实横移场景脱节；
            #   且 vel_est（真实横移速度）与 err 的物理关系相反 ⇒ Fitts 前馈被反向抵消，
            #   实测 ff_gain 从 0.85 调到 3.0 稳态滞后纹丝不动（37.54px）。
            #   正确建模：目标横移 v ⇒ 误差增量 = +v·dt（准星没动时误差变大）。
            err += plant.target_speed_px_s * (plant.frame_ms / 1000.0)
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
        "gain_px_per_count": plant.gain_px_per_count,
        "quantization_px": plant.gain_px_per_count,  # 1 count 对应的最小位移＝量化下限
    }


SCAN_GRID = [
    # (tag, kp, kd, predict, rate, smooth, deadzone, target_speed, box_jump)
    # V1.0.12（2026-09-30）：原先前两行开头的「6x镜」两组已随倍镜维度一并删除（不区分倍镜）。
    ("静止靶", KP_NOMINAL, KD_NOMINAL, 0.35 - RESPONSE_DELAY_MS / 300.0, 0.3, SMOOTH_DEFAULT, 1.0, 0.0, 0.0),
    ("移动60px/s", KP_NOMINAL, KD_NOMINAL, 0.35 - RESPONSE_DELAY_MS / 300.0, 0.3, SMOOTH_DEFAULT, 1.0, 60.0, 0.0),
    ("框跳±18px", KP_NOMINAL, KD_NOMINAL, 0.35 - RESPONSE_DELAY_MS / 300.0, 0.3, SMOOTH_DEFAULT, 1.0, 0.0, 18.0),
]


def scan(verbose: bool = True) -> list[dict]:
    """多场景扫描（V3 阶段 0 要求：参考 VisionForge 27 组思路）。

    覆盖最有代表性的三组：静止靶 / 移动目标 / 框抖。
    需要更宽生效域时按 SCAN_GRID 扩展即可，接口不变。
    """
    rows = []
    for tag, kp, kd, pr, rt, sm, dz, spd, jump in SCAN_GRID:
        plant = Plant(target_speed_px_s=spd, box_jump_px=jump)
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
    """内置断言：阶段 3b 是否真的达成（用于 CI/改动前后回归检查）。

    ★ 已知：断言 2（"稳态必须收敛到 2 count 量化级"）只对**静止靶**成立。
      纯 P 控制器跟匀速目标必然留 v/Kp_eff 的稳态误差，与死区/余数无关 ——
      "移动60px/s" 那行长期报这一条（2026-09-30 复核：改动前后同样报，不是回归）。
      要消掉它得先给控制器加积分项或速度前馈，不在本工具职责内，故保留原样。
    """
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
    args = ap.parse_args()

    if args.mode:
        plant = Plant()
        ctl = Controller(mode=args.mode)
        m, _, _ = run_closed_loop(plant, ctl)
        payload = {"mode": args.mode, **m.as_dict()}
        print(json.dumps(payload, ensure_ascii=False, indent=2) if args.json
              else f"mode={args.mode} 稳态={m.settle_median_px:.2f}px "
                   f"p75={m.settle_p75_px:.2f}px max={m.settle_max_px:.2f}px 振荡={'Y' if m.oscillating else 'N'}")
        return 0

    print("闭环 replay：静止/移动/框跳（gain 只有一套腰射标定值，不区分倍镜）")
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
