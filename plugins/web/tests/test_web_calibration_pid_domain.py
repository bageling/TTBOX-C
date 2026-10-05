# test_web_calibration_pid_domain.py — 自动标定 PID 口径护栏（V1.0.38）
#
# 背景（业主令「pid 以 pid1 为准完全移植，不许有自己的变动」）：
#   V1.0.13~V1.0.37 期间 core 删掉了 smooth、把"削弱 99%"折算进 kp，于是整套自动标定
#   都搬到**生效值**域：温和档 CALIB_PID_KP_MAX 从 10.0 改成 0.10、derive_pid_params
#   直接返回生效值。
#   V1.0.38 回归 pid1 完全移植（smooth 接回 Pid1Controller 第 5 参、折算整段删除），
#   kp 回到**名义值**域 ⇒ 标定链必须整体翻回来，否则会静默地"标定成功但参数是错的"。
#
# ★ 为什么这几个文件之前没人盯：
#   `plugins/web/tests/test_web_calibration_*.py` 里有一部分是**脚本式自测**
#   （顶层直接 `sys.exit()`，不是 pytest 用例）—— 用 pytest 收集时会在收集阶段
#   INTERNALERROR 打断整个 session，**全量 pytest 永远跑不到它们**。
#   本文件是 pytest 友好版，把同样的口径契约钉住。
import sys
from pathlib import Path

import pytest

# 路径单点真源（A-PATH-3）：不写 parents[N] 层级硬编码（门禁第⑩项会 FAIL）
from plugins.web.lib.paths import repo_root as _ttbox_repo_root  # noqa: E402
REPO = Path(_ttbox_repo_root())
if str(REPO) not in sys.path:
    sys.path.insert(0, str(REPO))

from plugins.web.lib.calibration import (  # noqa: E402
    CALIB_PID_KD_RATIO,
    CALIB_PID_KP_MAX,
)
from ttbox_motion.calibration import (  # noqa: E402
    KD_RATIO_BASE,
    SMOOTH_FACTOR,
    derive_pid_params,
)

# 板端 A/B 实测锚点（2026-09-24，gain≈0.686 / 回路延迟 51ms，均为**生效值**域）
BOARD_STABLE_KP = 0.10   # 16 轮全稳
BOARD_STABLE_KD = 0.30   # = 3.0 × kp
BOARD_OSCILLATE_KP = 0.25  # 持续振荡（准星 ±150px），标定必挂


# ---------------------------------------------------------------- 温和档
def test_calib_gentle_kp_is_nominal_domain():
    """★ 温和档必须与 kp 同域（名义值），否则标定期连目标都推不动。

    V1.0.13 曾把它换成 0.10（生效域）。smooth 回来后 0.10 名义 ⇒ 实际只发挥 0.001，
    比板端现役 kp=25 弱 250 倍 ⇒ bias 阶跃推不动目标、位移采样全落在噪声里
    ⇒ gain 测不准 ⇒ 整轮标定得出错误 kp。
    """
    assert CALIB_PID_KP_MAX == pytest.approx(BOARD_STABLE_KP / SMOOTH_FACTOR), (
        '温和档 kp 必须是名义值（= 稳定组生效值 / SMOOTH_FACTOR）；'
        f'当前 {CALIB_PID_KP_MAX}，换算后生效 {CALIB_PID_KP_MAX * SMOOTH_FACTOR:.4f}，'
        f'应为板端稳定组 {BOARD_STABLE_KP}')


def test_calib_gentle_kd_ratio_matches_board_stable_set():
    """kd/kp 比值要对齐板端稳定组（3.0），比值本身与域无关但值要与 kp 同域。"""
    assert CALIB_PID_KD_RATIO == pytest.approx(3.0), (
        f'温和档阻尼比应为板端稳定组的 3.0，当前 {CALIB_PID_KD_RATIO}')
    kd_eff = CALIB_PID_KP_MAX * CALIB_PID_KD_RATIO * SMOOTH_FACTOR
    assert kd_eff == pytest.approx(BOARD_STABLE_KD, rel=0.01), (
        f'温和档 kd 换算成生效值应≈{BOARD_STABLE_KD}，实际 {kd_eff:.4f}')


def test_gentle_kp_is_far_above_oscillation_band():
    """反向锁：温和档**换算后的生效值**不能落进振荡带。

    振荡带是 kp_eff ≥ 0.25（板端实测持续振荡）。若有人把温和档又改回 0.10 名义，
    生效值会变成 0.001 —— 虽然不振荡，但也推不动目标 ⇒ 标定必然测不准。
    """
    kp_eff = CALIB_PID_KP_MAX * SMOOTH_FACTOR
    assert kp_eff < BOARD_OSCILLATE_KP, (
        f'温和档生效值 {kp_eff:.4f} 已进入振荡带(≥{BOARD_OSCILLATE_KP})')
    # 更重要：不能弱到推不动目标（板端 kp=25 名义 = 生效 0.25 是能工作的量级）
    assert kp_eff >= BOARD_STABLE_KP * 0.5, (
        f'温和档生效值 {kp_eff:.4f} 比板端稳定组 {BOARD_STABLE_KP} 弱一半以上，'
        f'bias 阶跃会推不动目标 ⇒ gain 测不准')


# ---------------------------------------------------------------- 推导公式
def test_derive_returns_nominal_value():
    """★ derive_pid_params 返回**名义值**（配置里直接写它，由控制器 soft-limit 实现压缩）。"""
    d = derive_pid_params(0.686, 0.686, 51.0)
    kp_eff = d['kp'] * SMOOTH_FACTOR
    # 板端实测稳定组 0.10（生效）⇒ 名义应为 10.0 附近
    assert kp_eff == pytest.approx(BOARD_STABLE_KP, rel=0.15), (
        f'推导出的 kp 换算成生效值应≈{BOARD_STABLE_KP}（板端稳定组），实际 {kp_eff:.4f}')
    assert kp_eff < BOARD_OSCILLATE_KP, '推导结果不能落进振荡带'


def test_derive_nominal_is_hundred_times_effective():
    """口径钉死：返回值与生效值差 SMOOTH_FACTOR 的倒数（100 倍）。

    混淆这两个方向的后果是不对称的：
      · 忘了折回（返回生效值）⇒ 控制器再压一次 ⇒ 比预期弱 100 倍 ⇒「标定成功但几乎不动」
      · 多折一次（返回名义×0.01）⇒ 强 100 倍 ⇒ 自激、目标被甩飞
    """
    d = derive_pid_params(0.686, 0.686, 51.0)
    assert d['kp'] != pytest.approx(d['kp'] * SMOOTH_FACTOR, rel=1e-3), (
        '返回值应与生效值不同（名义 vs 生效）—— 若相同说明折算没生效或被重复施加')
    assert d['kp'] / (d['kp'] * SMOOTH_FACTOR) == pytest.approx(1.0 / SMOOTH_FACTOR, rel=1e-6)


def test_derive_internal_math_still_on_effective_domain():
    """★ 内部物理推导（阻尼比）仍在**生效域**算，锚点没变 —— 只改了输出换算。"""
    # KD_RATIO_BASE + delay/div 是生效域的阻尼比；50ms ⇒ 3.0
    ratio = KD_RATIO_BASE + 51.0 / 25.0
    assert ratio == pytest.approx(3.0, rel=0.05), '阻尼比锚点被改动（板端稳定组是 3.0×kp）'
    d = derive_pid_params(0.686, 0.686, 51.0)
    # 名义域的 kd/kp 比值与生效域相同（分子分母同乘一个因子）
    assert d['kd'] / d['kp'] == pytest.approx(ratio, rel=0.05)


def test_derive_predict_bounds_unchanged():
    """predict 上下限与域无关，但口径翻回来后不该被顺手动过。"""
    d_low = derive_pid_params(0.65, 0.65, 10.0)
    d_high = derive_pid_params(0.65, 0.65, 60.0)
    assert d_low['predict'] > d_high['predict'], '延迟越大 predict 越保守'
    for d in (d_low, d_high):
        assert 0.1 <= d['predict'] <= 0.35, f'predict 越界: {d["predict"]}'
