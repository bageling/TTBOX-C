"""TTBOX 自动标定行为模型。

只负责状态、观测和稳健拟合；设备读写与 HTTP 编排由上层负责。
"""
from __future__ import annotations

from dataclasses import dataclass, field
from enum import Enum
from statistics import median
from typing import Iterable


class CalibrationAxis(str, Enum):
    X = "x"
    Y = "y"


# 响应延迟验证门（ms）。2026-09-24 板上实测：144fps 采集回路的真实延迟
# ≈51ms（渲染→采集→推理→注入→游戏应用→再采集），旧上限 50ms 刚好把
# 完全正常的链路判死。120ms 与 derive_pid_params 里 kd 的延迟满量程一致。
RESPONSE_DELAY_MAX_MS = 120.0

# ---- 自动调参的锚点常量（2026-09-25 由板端 A/B 实测反推，见 derive_pid_params）----
#
# 板端 A/B（gain 实测 x=0.686 / y=0.695，回路延迟 51ms）：
#   kp=0.25 / kd=0.25 → 单帧吃 17% 误差 → bias 阶跃打进持续振荡（准星 ±150px），标定必挂
#   kp=0.10 / kd=0.30 → 单帧吃 6.9% 误差 → 16 轮全稳
# ⇒ 单帧误差比例取稳定组的 7%，阻尼比取稳定组在 50ms 的 3.0×kp。
# ★ V1.0.13（2026-09-30）：上面两组数从"名义值"换成了**生效值** —— core 侧删掉 smooth 之后，
#   RuntimeProfile.mouse.kp_x 就是接进环路的那个数（旧配置里的 25 实际只等于 0.25）。
# ★★★ V1.0.38（2026-10-05）：**口径再次翻转，回归 pid1 完全移植**（业主令）。
#   推导内部仍用**生效值**做物理计算（单帧吃多少误差 / 阻尼比多少，与 smooth 无关），
#   但**返回前折回名义值**，因为 core 侧把 smooth 交还给 Pid1Controller 第 5 参、
#   并删掉了 kp 折算（见 core/src/model/RuntimeProfile.cpp、AimThread.cpp:274-277）。
#   不折回去的后果（实测）：标定算出 kp=0.102 写进配置 ⇒ 控制器再经 smoothTerm 压一次
#   ⇒ 实际只发挥 0.0408，**比标定预期弱 2.5 倍** ⇒ "标定成功但自瞄几乎不动"。
KP_FRACTION_PER_FRAME = 0.07
KD_RATIO_BASE = 1.0          # 零延迟时的基础阻尼比
KD_RATIO_DELAY_DIV = 25.0    # 延迟每 +25ms，阻尼比 +1.0（50ms ⇒ 3.0×kp，对齐实测稳定组）
# KP 下限：它管的是"这点输出能不能真挤出 count"（死区/整数量化）。
# 锚点 0.04（= 旧名义 4.0 @ smooth=9900；仿真扫描证明 g1.5+d60 需要 kp≈0.04~0.07 才动得起来）。
KP_EFF_MIN = 0.04
KP_MAX = 1.0                 # 安全帽：仿真扫描显示生效 kp > 0.3 在 51ms 回路下必自激
# KD 的上下限同样换到生效域（旧名义 4.0 / 50.0 × 0.01）。
KD_MIN = 0.04
KD_MAX = 0.5
# ★★★ V1.0.38 新增：名义↔生效 的换算因子。
#   pid1 的 soft-limit 对小信号是纯乘法 (v/10000)*(10000-smooth)，即
#   "名义 kp 实际只发挥 kp×(10000-smooth)/10000"。core 恢复这条通路后，
#   标定要把生效值折回名义值必须**除以**这个因子。
BOARD_SMOOTH = 9900.0
SMOOTH_FACTOR = (10000.0 - BOARD_SMOOTH) / 10000.0   # = 0.01


class CalibrationState(str, Enum):
    IDLE = "idle"
    PREPARING = "preparing"
    STABILIZE_X = "stabilize_x"
    SAMPLING_X = "sampling_x"
    ANALYZING_X = "analyzing_x"
    STABILIZE_Y = "stabilize_y"
    SAMPLING_Y = "sampling_y"
    ANALYZING_Y = "analyzing_y"
    VALIDATING = "validating"
    APPLYING = "applying"
    COMPLETED = "completed"
    CANCELLED = "cancelled"
    FAILED = "failed"


@dataclass
class CalibrationObservation:
    axis: CalibrationAxis
    injected_count: float
    measured_delta_px: float
    response_delay_ms: float
    target_id: str
    valid: bool = True


@dataclass
class AxisFit:
    axis: CalibrationAxis
    gain_px_per_count: float = 0.0
    response_delay_ms: float = 0.0
    sample_count: int = 0
    rejected_count: int = 0
    consistency: float = 0.0
    converged: bool = False
    failure_reason: str = ""


@dataclass
class CalibrationSession:
    state: CalibrationState = CalibrationState.IDLE
    failure_reason: str = ""
    current_axis: CalibrationAxis | None = None
    observations: list[CalibrationObservation] = field(default_factory=list)

    def start(self) -> None:
        if self.state is not CalibrationState.IDLE:
            raise ValueError("标定会话已启动")
        self.state = CalibrationState.PREPARING

    def begin_axis(self, axis: CalibrationAxis) -> None:
        if axis is CalibrationAxis.X:
            allowed = {CalibrationState.PREPARING, CalibrationState.ANALYZING_X}
            next_state = CalibrationState.STABILIZE_X
        else:
            allowed = {CalibrationState.ANALYZING_X, CalibrationState.STABILIZE_Y}
            next_state = CalibrationState.STABILIZE_Y
        if self.state not in allowed:
            raise ValueError(f"当前状态不能开始{axis.value}轴")
        self.current_axis = axis
        self.state = next_state

    def begin_sampling(self) -> None:
        if self.state is CalibrationState.STABILIZE_X:
            self.state = CalibrationState.SAMPLING_X
        elif self.state is CalibrationState.STABILIZE_Y:
            self.state = CalibrationState.SAMPLING_Y
        else:
            raise ValueError("当前状态不能进入采样")

    def begin_analysis(self) -> None:
        if self.state is CalibrationState.SAMPLING_X:
            self.state = CalibrationState.ANALYZING_X
        elif self.state is CalibrationState.SAMPLING_Y:
            self.state = CalibrationState.ANALYZING_Y
        else:
            raise ValueError("当前状态不能进入分析")

    def complete_axis(self) -> None:
        if self.state is CalibrationState.ANALYZING_X:
            self.current_axis = CalibrationAxis.Y
            self.state = CalibrationState.STABILIZE_Y
        elif self.state is CalibrationState.ANALYZING_Y:
            self.state = CalibrationState.VALIDATING
        else:
            raise ValueError("当前状态不能完成轴标定")

    def cancel(self) -> None:
        self.state = CalibrationState.CANCELLED

    def fail(self, reason: str) -> None:
        self.failure_reason = str(reason)
        self.state = CalibrationState.FAILED


def fit_axis_measurements(
    axis: CalibrationAxis,
    observations: Iterable[CalibrationObservation],
    *,
    max_relative_mad: float = 0.35,
    min_samples: int = 5,
) -> AxisFit:
    """按轴估计 px/count 和延迟，使用中位数/MAD 排除离群动作。

    目标身份必须一致；每个观测的增益为 measured_delta/injected_count。
    """
    items = [o for o in observations if o.axis is axis and o.valid]
    result = AxisFit(axis=axis)
    result.sample_count = len(items)
    if len(items) < min_samples:
        result.failure_reason = f"{axis.value}轴有效样本不足"
        return result
    target_ids = {o.target_id for o in items if o.target_id}
    if len(target_ids) > 1:
        result.failure_reason = "目标身份在标定过程中发生变化"
        return result
    ratios = []
    for item in items:
        if item.injected_count <= 0:
            continue
        ratios.append(item.measured_delta_px / item.injected_count)
    if len(ratios) < min_samples:
        result.failure_reason = f"{axis.value}轴有效输入不足"
        return result
    center = median(ratios)
    deviations = [abs(value - center) for value in ratios]
    mad = median(deviations)
    threshold = max(abs(center) * max_relative_mad, 1e-6)
    kept = [value for value in ratios if abs(value - center) <= threshold]
    result.rejected_count = len(ratios) - len(kept)
    if len(kept) < min_samples - 1:
        result.failure_reason = f"{axis.value}轴测量一致性不足"
        return result
    result.gain_px_per_count = median(kept)
    result.consistency = max(0.0, 1.0 - mad / max(abs(center), 1e-6))
    if mad / max(abs(center), 1e-6) > max_relative_mad:
        result.failure_reason = f"{axis.value}轴测量一致性不足"
        return result
    result.response_delay_ms = median([o.response_delay_ms for o in items])
    if not 0.03 <= result.gain_px_per_count <= 8.0:
        result.failure_reason = f"{axis.value}轴增益超出范围"
        return result
    if not 0.0 <= result.response_delay_ms <= RESPONSE_DELAY_MAX_MS:
        result.failure_reason = f"{axis.value}轴响应延迟超出范围"
        return result
    result.converged = True
    return result


def derive_pid_params(
    gain_x_px_per_count: float,
    gain_y_px_per_count: float,
    response_delay_ms: float,
) -> dict:
    """由标定实测物理量推导 SmoothAimController 的比例增益 aim_gain（自动调参核心）。

    ★★★ V1.0.41（2026-10-05）：pid1 彻底删除，换 SmoothAimController
      （EMA+比例+限幅+框高死区，见 core/src/aim/SmoothAimController.hpp）。
      新控制器只推导 aim_gain（= 误差 px → 移动 count 的比例），
      alpha / max_move / deadzone_ratio 是手感参数、不进物理推导 ⇒ 调参简单。

    物理（单帧）：
      move(count) = error(px) × aim_gain(count/px)
      准星移动 px  = move × gain = error × aim_gain × gain
      单帧吃误差比例 = aim_gain × gain
    ⇒ aim_gain = KP_FRACTION_PER_FRAME / gain（单帧吃 7% 误差）

    锚点（板端 A/B 实测 2026-09-24）：单帧吃 6.9% ⇒ 16 轮全稳；吃 17% ⇒ 振荡。
    """
    if gain_x_px_per_count <= 0 or gain_y_px_per_count <= 0:
        raise ValueError("增益必须 > 0")
    gain = min(gain_x_px_per_count, gain_y_px_per_count)
    delay = max(0.0, float(response_delay_ms))
    # aim_gain：目标「单帧吃掉 7% 误差」。
    aim_gain = KP_FRACTION_PER_FRAME / gain
    # 极端场景（超高增益 + 高延迟）：命令在延迟窗口内过冲是极限环主因。
    if gain >= 1.2 and delay >= 50.0:
        aim_gain *= 0.4
    aim_gain = max(KP_EFF_MIN, min(KP_MAX, aim_gain))
    return {'aim_gain': round(aim_gain, 4)}
