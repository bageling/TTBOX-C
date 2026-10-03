# test_web_calibration_amplitude_loop.py — 摆动幅度加大 + 闭环收敛（#39）
#
# 背景（业主 2026-09-30 指令三）：「摆动幅度太小测不出最快速度，想想逻辑如何闭环」。
# 落地两件事：
#   1) 幅度表上限从 ±32 提到 ±96（3 倍），等比 ×2 拉满动态范围；
#   2) 摆动形成闭环——追得上（位移 ≥ 幅度×0.6）就继续走大档；连续追不上 2 轮
#      ⇒ 认为到了系统速度上限，提前收敛停摆，不再硬跑更大幅、避免把目标甩飞。
#   闭环产物 max_tracked_amp = 温和档下「最快可追幅度」，落盘 + 面板可读，供后续 rate 推导。
import importlib.util
import os
import pathlib
import sys
import tempfile

import pytest

from plugins.web.lib.paths import repo_root as _ttbox_repo_root  # noqa: E402  路径单点真源（A-PATH-3）
REPO_ROOT = pathlib.Path(_ttbox_repo_root())
WEB_SRC = REPO_ROOT / 'plugins' / 'web' / 'bin' / 'ttbox-web.py'
# ★ 2026-10-02（web 换写法 S4）：标定实现搬到 lib/calibration.py。
CALIB_SRC = REPO_ROOT / 'plugins' / 'web' / 'lib' / 'calibration.py'

_ext = {'n': 0}


def _load():
    _ext['n'] += 1
    name = 'ttbox_web_ampload_%d_%d' % (os.getpid(), _ext['n'])
    spec = importlib.util.spec_from_file_location(name, WEB_SRC)
    mod = importlib.util.module_from_spec(spec)
    sys.modules[name] = mod
    spec.loader.exec_module(mod)
    return mod


@pytest.fixture()
def web_mod(monkeypatch):
    tmp = tempfile.mkdtemp(prefix='ttbox_ampload_%d_' % os.getpid())
    monkeypatch.setenv('TTBOX_PREFIX', tmp)
    monkeypatch.setenv('TTBOX_PRESETS_DIR', os.path.join(tmp, 'presets'))
    monkeypatch.setenv('TTBOX_CONFIG_DIR', os.path.join(tmp, 'config'))
    monkeypatch.setenv('TTBOX_MODELS_ROOT', os.path.join(tmp, 'models'))
    monkeypatch.setenv('TTBOX_MOTION_PROFILES_DIR', os.path.join(tmp, 'config', 'motion-profiles'))
    return _load()


def _src() -> str:
    """实现可能落在入口或 lib（S4 起标定搬到 lib/calibration.py）——
    这些断言只问「代码里有没有这一段」，故把两侧拼起来看。"""
    return (WEB_SRC.read_text(encoding='utf-8') +
            CALIB_SRC.read_text(encoding='utf-8'))


def _worker() -> str:
    s = CALIB_SRC.read_text(encoding='utf-8')
    return s[s.index('def _calib_worker('):s.index('def _calibration_payload(')]


# ===========================================================================
# 1. 幅度加大
# ===========================================================================

def test_amplitude_upper_bound_enlarged(web_mod):
    """上限至少 64px（原 32 的 2 倍起步），否则还是测不出最快速度。"""
    mags = [abs(a) for a in web_mod.CALIB_AMPLITUDES]
    assert max(mags) >= 64.0, f'上限只到 {max(mags)}px，未加大'


def test_amplitudes_still_alternate_and_increasing(web_mod):
    amps = list(web_mod.CALIB_AMPLITUDES)
    signs = [1 if a > 0 else -1 for a in amps]
    assert all(s != t for s, t in zip(signs, signs[1:])), f'幅度未正负交替: {amps}'
    mags = [abs(a) for a in amps]
    assert mags == sorted(mags), f'幅度应递增: {mags}'


# ===========================================================================
# 2. 闭环判据与收敛
# ===========================================================================

def test_closed_loop_track_judgement_exists():
    body = _worker()
    assert 'CALIB_AMP_TRACK_RATIO' in _src()
    assert 'tracked = False' in body, '采样窗缺"追上"判据初始化'
    assert 'tracked = True' in body, '追到 60% 幅度没有置位'


def test_closed_loop_converges_on_miss(web_mod):
    """追得上→记最大幅度；连续追不上→提前收敛，不再硬跑更大幅。"""
    body = _worker()
    assert 'max_tracked_amp = max(max_tracked_amp, abs(amp))' in body
    assert 'miss_streak >= CALIB_AMP_MISS_LIMIT' in body
    assert 'break' in body
    assert web_mod.CALIB_AMP_MISS_LIMIT >= 1


def test_miss_only_counts_when_injection_landed():
    """只有"注入真生效却没追到"才算追不上；注入没落设备（no_write）不该触发收敛。"""
    body = _worker()
    # 收敛分支挂在 elif wrote 上：wrote=False 时不该进 miss 计数
    seg = body[body.index('if tracked:'):body.index('if not axis_observations[axis]:')]
    assert 'elif wrote:' in seg, '收敛分支没挂在"注入落地"判据上'


# ===========================================================================
# 3. 产物落盘 + 面板可见
# ===========================================================================

def test_max_tracked_amp_persisted():
    s = _src()
    assert "'max_tracked_amp_x'" in s, '留档缺 X 轴最快可追幅度'
    assert "'max_tracked_amp_y'" in s, '留档缺 Y 轴最快可追幅度'
    assert "'max_tracked_amp'" in s, '留档缺整体瓶颈'


def test_max_tracked_amp_exposed_in_payload():
    s = _src()
    assert "'max_tracked_amp': _cal['max_tracked_amp']" in s, 'payload 没暴露 max_tracked_amp'
