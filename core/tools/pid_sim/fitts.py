"""fitts.py — FittsAimController.hpp 的 Python 精确移植（本地闭环仿真用，不进产品构建）

★ 与 core/src/aim/FittsAimController.hpp **逐条对应**，改一处必须改两处：
  1. 非有限输入拦截（isfinite 三参数）——abs(NaN)<min_zone 恒假会让 NaN 穿死区。
  2. 尺寸自适应死区：min_zone = max(deadzone_px, box_h × deadzone_box_ratio)。
  3. 难度指数 ID = log2(2·|err|/W + 1)，W = 2·min_zone。
  4. 移动耗时 MT = clamp(a + b·ID, 20, 320) ms。
  5. dt 实测传入 + 钳 [1,40]ms（写死 dt ⇒ 帧率抖动时输出 ±50% 失配）。
  6. 速度前馈 ff = min(ff_gain, 0.85) × vel × MT×0.001。
  7. 符号护栏：反向前馈**只钳幅度不动符号**（用 copysign 翻符号会让「减速」变「加速」）。
  8. 输出 = (err + ff) × (dt/MT) / gain，末道 isfinite 兜底。

对齐验证：core/tests/test_fitts_aim.cpp 的 10 条断言在本文件上应全部成立
（见 aim_replay.py 的 selftest，或直接 python fitts.py）。
"""
from __future__ import annotations

import math

K_MIN_MS = 20.0
K_MAX_MS = 320.0
K_DEFAULT_DT_MS = 6.9
K_MIN_DT_MS = 1.0
K_MAX_DT_MS = 40.0
K_FF_HARD_CAP = 0.85
# ★ 前馈时延 τ = 回路延迟（物理常量，板端实测 51ms）。**不是 MT**。
#   MT 随误差增长 ⇒ 用 MT 当 τ 会正反馈（越跟不上→前馈越猛→过冲→更跟不上）。
FF_TAU_MS = 51.0


class Fitts:
    def __init__(self, a_ms=20.0, b_ms=20.0, deadzone_px=3.0,
                 deadzone_box_ratio=0.02, ff_gain=0.85, ff_tau_ms=FF_TAU_MS):
        if a_ms > 0.0:
            self.a = a_ms
        if b_ms > 0.0:
            self.b = b_ms
        if deadzone_px > 0.0:
            self.dz = deadzone_px
        # ★ 守卫用 >= 0：ratio=0.0（只要绝对像素死区）是合法值，
        #   原来的 `> 0.0` 会让属性根本没被设置 ⇒ 第一次访问 self.ratio 就 AttributeError。
        if deadzone_box_ratio >= 0.0:
            self.ratio = deadzone_box_ratio
        if ff_gain >= 0.0:
            self.ff_gain = ff_gain
        if ff_tau_ms > 0.0:
            self.ff_tau_ms = ff_tau_ms

    def upd(self, error, box_h, gain_px_per_count, vel_px_s, dt_ms):
        # 0. 非有限输入拦截
        if not (math.isfinite(error) and math.isfinite(box_h) and math.isfinite(vel_px_s)):
            return 0.0
        # 1. 尺寸自适应死区
        min_zone = max(self.dz, box_h * self.ratio) if box_h > 0.0 else self.dz
        if abs(error) < min_zone:
            return 0.0
        # 2. 难度指数
        W = 2.0 * min_zone
        idd = math.log2(2.0 * abs(error) / W + 1.0)
        # 3. 移动耗时
        mt = max(K_MIN_MS, min(K_MAX_MS, self.a + self.b * idd))
        # 4/5. dt 实测 + 钳位
        dt = max(K_MIN_DT_MS, min(K_MAX_DT_MS, dt_ms)) if dt_ms > 0.0 else K_DEFAULT_DT_MS
        # 6. 速度前馈：补偿**固定回路延迟** τ（不是 MT——MT 随误差增长会形成正反馈）
        ff = min(self.ff_gain, K_FF_HARD_CAP) * vel_px_s * (self.ff_tau_ms * 0.001)
        # 7. 符号护栏：反向前馈只钳幅度，**保留原符号**
        if (error > 0.0 and ff < 0.0) or (error < 0.0 and ff > 0.0):
            capped = min(abs(ff), abs(error))
            ff = -capped if ff < 0.0 else capped
        # 8. px → count
        g = gain_px_per_count if gain_px_per_count > 1e-4 else 0.65
        out = ((error + ff) * (dt / mt)) / g
        return out if math.isfinite(out) else 0.0


if __name__ == "__main__":
    # 自检：复刻 core/tests/test_fitts_aim.cpp 的关键断言
    c = Fitts(20.0, 20.0, 3.0, 0.02, 0.0)
    # ratio=0.02 ⇒ box_h=100 时死区 = max(dz=3, 100×0.02=2) = 3px（由 dz 主导）
    assert c.upd(2.0, 100.0, 0.65, 0.0, 7.0) == 0.0, "死区内应不动"
    assert c.upd(4.0, 100.0, 0.65, 0.0, 7.0) > 0.0, "死区外应动"
    # ratio=0.02 ⇒ box_h=300 死区 = max(3, 6) = 6px（此时比例项主导）
    assert c.upd(4.0, 300.0, 0.65, 0.0, 7.0) == 0.0, "大框死区更大(6px)"
    assert c.upd(8.0, 300.0, 0.65, 0.0, 7.0) > 0.0, "大框死区外应动"
    a7 = c.upd(100.0, 100.0, 0.65, 0.0, 7.0)
    a14 = c.upd(100.0, 100.0, 0.65, 0.0, 14.0)
    assert abs(a14 / a7 - 2.0) < 0.01, "dt 线性"
    c2 = Fitts(20.0, 20.0, 3.0, 0.02, 0.85)
    # ★ 判据修正（V1.0.44）：前馈的物理作用是「在**小误差**工作点加速」——
    #   闭环稳态下 err 已经很小，此刻速度项决定准星能否维持住移动目标。
    #   在**大误差**点（如 100px）比较 ff 有无会误导：那时 |err| 远大于 |v·τ|，
    #   前馈只是锦上添花，看不出方向。必须在贴近稳态的误差上比。
    for err in (5.0, 10.0, 20.0, 40.0):
        no_ff_v = Fitts(20.0, 20.0, 3.0, 0.02, 0.0).upd(err, 100.0, 0.65, 200.0, 7.0)
        with_ff_v = c2.upd(err, 100.0, 0.65, 200.0, 7.0)
        assert with_ff_v > no_ff_v, f"err={err} 同向前馈应加速: {with_ff_v} <= {no_ff_v}"
    # 反向：符号护栏保证输出不反向（可能减速，但绝不为负）
    for vel in (-200.0, -2000.0):
        assert c2.upd(50.0, 100.0, 0.65, vel, 7.0) >= 0.0, "护栏：输出不得反向"
    nan = float("nan")
    assert c2.upd(nan, 100.0, 0.65, 0.0, 7.0) == 0.0, "NaN 必须被拦"
    assert math.isfinite(c2.upd(100.0, 100.0, float("inf"), 0.0, 7.0)), "Inf gain 不得产出 NaN"
    print("fitts.py selftest: PASS（与 core 单测对齐）")
