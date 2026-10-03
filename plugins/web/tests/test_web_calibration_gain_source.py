# test_web_calibration_gain_source.py — 自动标定「分母真源」与「采样方法」回归锁
#
# 背景（2026-09-24 上板三组对照定死的事）：
#   1) 旧实现把「参考点偏置的 px」当成注入量当分母 ⇒ 量纲是 px/px ⇒ 拟合出的 gain 恒 ≈1.0，
#      再经 derive_pid_params 恒推出 kp=0.07 —— **与游戏灵敏度无关**，标定成功也改不了手感。
#   2) 幅度表 [8,16,24,32,40] 全为正值、轮间只清 bias 不等瞄点归位 ⇒ 慢环下位移逐轮累加
#      ⇒ 比值散开 ⇒ 必挂一致性门（MAD/|中位|>0.35）⇒ 表现为"永远标定不到"。
#   3) 中途取消/失败时未清 calibration_bias_* ⇒ 参考点被永久顶偏（只能重启恢复）。
#
# 修法（本文件逐条钉住）：
#   · 分母换成 core 的真实累计注入 count（aim_out_counts_*）；旧 core 缺字段时必须**明确失败**，
#     绝不退回 px 分母。
#   · 幅度表正负交替 + 分量程；每轮开始前回零并等瞄点静止。
#   · 每样本要求：同目标 + 位移门槛 + count 门槛 + usbproxy 确实写出过包。
#   · finally 里把两个 bias 一起归零。
#
# 运行：python -m pytest plugins/web/tests/test_web_calibration_gain_source.py -v
import importlib.util
import os
import pathlib
import re
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
    name = 'ttbox_web_calibsrc_%d_%d' % (os.getpid(), _ext['n'])
    spec = importlib.util.spec_from_file_location(name, WEB_SRC)
    mod = importlib.util.module_from_spec(spec)
    sys.modules[name] = mod
    spec.loader.exec_module(mod)
    return mod


@pytest.fixture()
def web_mod(monkeypatch):
    tmp = tempfile.mkdtemp(prefix='ttbox_calibsrc_%d_' % os.getpid())
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


def _worker_src() -> str:
    s = CALIB_SRC.read_text(encoding='utf-8')
    return s[s.index('def _calib_worker('):s.index('def _calibration_payload(')]


def _status(metrics):
    return {'runtime_running': True, 'metrics': metrics}


# ===========================================================================
# 1. 分母真源：读 core 真实 count，缺字段必须返回 None（不退回 px）
# ===========================================================================

def test_out_counts_reads_core_metric(web_mod, monkeypatch):
    monkeypatch.setattr(web_mod, '_get_status', lambda: _status(
        {'aim_out_counts_x': 42, 'aim_out_counts_y': -17}))
    assert web_mod._calib_out_counts() == (42, -17)


def test_out_counts_distinguishes_zero_from_missing(web_mod, monkeypatch):
    """0 是合法读数（这一段没动），None 才是"读不到"——两者不能混。"""
    monkeypatch.setattr(web_mod, '_get_status', lambda: _status(
        {'aim_out_counts_x': 0, 'aim_out_counts_y': 0}))
    assert web_mod._calib_out_counts() == (0, 0)


@pytest.mark.parametrize('metrics', [
    {},                              # 旧 core：整个字段不存在
    {'aim_pos_x': 1.0},              # 旧 core：只有老字段
    {'aim_out_counts_x': 1},         # 只有 x（半套）也算读不到
])
def test_out_counts_returns_none_for_old_core(web_mod, monkeypatch, metrics):
    monkeypatch.setattr(web_mod, '_get_status', lambda: _status(metrics))
    assert web_mod._calib_out_counts() is None


@pytest.mark.parametrize('bad', [
    None, {}, [], 'nope', {'data': {}},
])
def test_out_counts_tolerates_shapeless_status(web_mod, monkeypatch, bad):
    monkeypatch.setattr(web_mod, '_get_status', lambda: bad)
    assert web_mod._calib_out_counts() is None


def test_worker_fails_loudly_when_counts_metric_missing(web_mod):
    """★ 缺字段时必须**失败**，绝不能悄悄退回"用 px 当分母"那套。

    退回的代价是"永远成功、永远是错的"：gain 恒 1.0 ⇒ kp 恒 0.07 ⇒ 用户以为标定好了，
    实际手感一点没变。宁可明确报错。
    """
    body = _worker_src()
    assert '_calib_out_counts() is None' in body
    m = re.search(r"if _calib_out_counts\(\) is None:\s*\n(.{0,400})", body, re.S)
    assert m, '找不到缺字段的分支'
    branch = m.group(1)
    assert "status='failed'" in branch or 'status="failed"' in branch
    assert 'aim_out_counts_' in branch
    # 失败分支必须 return，不能继续往下跑
    assert re.search(r'\breturn\b', branch)


# ===========================================================================
# 2. 幅度表：正负交替 + 分量程
# ===========================================================================

def test_amplitudes_alternate_sign(web_mod):
    amps = list(web_mod.CALIB_AMPLITUDES)
    assert len(amps) >= 5, '样本数必须够撑满 fit 的 min_samples=5'
    signs = [1 if a > 0 else -1 for a in amps]
    assert all(s != t for s, t in zip(signs, signs[1:])), f'幅度未正负交替: {amps}'


def test_amplitudes_cover_a_range_of_magnitudes(web_mod):
    mags = [abs(a) for a in web_mod.CALIB_AMPLITUDES]
    assert mags == sorted(mags), f'幅度应递增: {mags}'
    assert len(set(mags)) >= 4, f'幅度要分量程: {mags}'


def test_amplitude_table_is_no_longer_all_positive(web_mod):
    """反向锁：旧表 [8,16,24,32,40] 全正 ⇒ 慢环下位移逐轮累加 ⇒ 必挂一致性门。"""
    amps = list(web_mod.CALIB_AMPLITUDES)
    assert any(a < 0 for a in amps), '又退回全正幅度表了'


def test_sampling_thresholds_reject_noise_level_signals(web_mod):
    assert web_mod.CALIB_MIN_COUNTS >= 2
    # 静止时实测抖动 ±0.05px，位移门槛必须远高于它，否则噪声就能伪造样本
    assert web_mod.CALIB_MIN_DELTA_PX >= 0.3


# ===========================================================================
# 3. 偏置写入：只写目标轴、恒开标定模式、Core 拒绝要如实返回
# ===========================================================================

def _bias_env(web_mod, monkeypatch, status=0):
    rec = {'sent': []}
    profile = {'mouse': {'kp_x': 25.0, 'calibrating': False}}

    def fake_ipc(req_type, params=None, timeout=5):
        rec['sent'].append((req_type, params))
        return {'status': status, 'data': {}}

    monkeypatch.setattr(web_mod, 'ipc_request', fake_ipc)
    monkeypatch.setattr(web_mod, '_get_runtime_profile', lambda: profile)
    return rec, profile


def test_apply_bias_writes_only_the_target_axis(web_mod, monkeypatch):
    rec, profile = _bias_env(web_mod, monkeypatch)
    assert web_mod._calib_apply_bias(web_mod.CalibrationAxis.X, 16.0) is True
    mo = profile['mouse']
    assert mo['calibration_bias_x'] == pytest.approx(16.0)
    assert mo['calibration_bias_y'] == pytest.approx(0.0), '另一轴必须显式归零，否则会串轴'
    assert mo['calibrating'] is True
    assert rec['sent'] and rec['sent'][0][0] == 'SET_CONFIG'


def test_apply_bias_negative_value_keeps_sign(web_mod, monkeypatch):
    rec, profile = _bias_env(web_mod, monkeypatch)
    web_mod._calib_apply_bias(web_mod.CalibrationAxis.Y, -24.0)
    assert profile['mouse']['calibration_bias_y'] == pytest.approx(-24.0)
    assert profile['mouse']['calibration_bias_x'] == pytest.approx(0.0)


def test_apply_bias_zero_clears_both_axes(web_mod, monkeypatch):
    """轮间回零用的就是这个入口：清偏置时两轴都要归零。"""
    rec, profile = _bias_env(web_mod, monkeypatch)
    profile['mouse']['calibration_bias_y'] = 32.0
    web_mod._calib_apply_bias(web_mod.CalibrationAxis.X, 0.0)
    assert profile['mouse']['calibration_bias_x'] == pytest.approx(0.0)
    assert profile['mouse']['calibration_bias_y'] == pytest.approx(0.0)


def test_apply_bias_reports_core_rejection(web_mod, monkeypatch):
    rec, profile = _bias_env(web_mod, monkeypatch, status=1)
    assert web_mod._calib_apply_bias(web_mod.CalibrationAxis.X, 8.0) is False


# ===========================================================================
# 4. 采样配对：无目标 ⇒ None（不能拿半条数据算 gain）
# ===========================================================================

def test_sample_pair_returns_none_without_target(web_mod, monkeypatch):
    monkeypatch.setattr(web_mod, '_calib_target', lambda: None)
    monkeypatch.setattr(web_mod, '_calib_out_counts', lambda: (10, 0))
    assert web_mod._calib_sample_pair(web_mod.CalibrationAxis.X, 2) is None


def test_sample_pair_returns_none_without_counts(web_mod, monkeypatch):
    monkeypatch.setattr(web_mod, '_calib_target', lambda: {
        'x': 100.0, 'y': 50.0, 'target_id': 1, 'class_id': 0})
    monkeypatch.setattr(web_mod, '_calib_out_counts', lambda: None)
    assert web_mod._calib_sample_pair(web_mod.CalibrationAxis.X, 2) is None


def test_sample_pair_reads_the_axis_under_test(web_mod, monkeypatch):
    monkeypatch.setattr(web_mod, '_calib_target', lambda: {
        'x': 100.0, 'y': 50.0, 'target_id': 1, 'class_id': 0})
    monkeypatch.setattr(web_mod, '_calib_out_counts', lambda: (7, -9))
    px = web_mod._calib_sample_pair(web_mod.CalibrationAxis.X, 2)
    py = web_mod._calib_sample_pair(web_mod.CalibrationAxis.Y, 2)
    assert px['px'] == pytest.approx(100.0) and px['counts'] == 7
    assert py['px'] == pytest.approx(50.0) and py['counts'] == -9


# ===========================================================================
# 5. 轮间回零：等瞄点静止
# ===========================================================================

def test_wait_settled_true_when_target_is_still(web_mod, monkeypatch):
    monkeypatch.setattr(web_mod, '_calib_sample_pair',
                        lambda axis, n=1: {'px': 100.0, 'counts': 0, 'target': {}})
    settled, ms = web_mod._calib_wait_settled(web_mod.CalibrationAxis.X,
                                              deadline_s=0.3, quiet_n=3)
    assert settled is True
    assert ms >= 0


def test_wait_settled_false_when_target_keeps_drifting(web_mod, monkeypatch):
    """目标一直动（上一轮余速/用户手抖）⇒ 等不到静止，如实返回 False。

    不因此判失败：identity 对任意窗口成立，只是样本脏，交给 MAD 门过滤。
    """
    state = {'px': 0.0}
    def drifting(axis, n=1):
        state['px'] += 5.0
        return {'px': state['px'], 'counts': 0, 'target': {}}
    monkeypatch.setattr(web_mod, '_calib_sample_pair', drifting)
    settled, ms = web_mod._calib_wait_settled(web_mod.CalibrationAxis.X,
                                              deadline_s=0.1, quiet_n=3)
    assert settled is False


def test_worker_clears_bias_before_every_round():
    """★ 反向锁：每轮采样前必须先回零再测（旧实现只清 bias 不等归位 ⇒ 位移逐轮累加）。"""
    body = _worker_src()
    pre = body.index('for index, amp in enumerate(amplitudes):')
    post = body.index('start = _calib_sample_pair(axis)')
    window = body[pre:post]
    assert '_calib_apply_bias(axis, 0.0)' in window, '采样前没有回零'
    assert '_calib_wait_settled(axis)' in window, '回零后没有等瞄点静止'
    assert window.index('_calib_apply_bias(axis, 0.0)') < window.index('_calib_wait_settled(axis)')


# ===========================================================================
# 6. 失败清理：bias 必须归零
# ===========================================================================

def test_worker_clears_bias_on_entry():
    """★ 入场也要清零：上一轮若异常退出（进程被杀/重启），板上可能留着非零偏置，
    那会让紧接着的「稳定检测」先把目标拉偏 ⇒ 直接判目标不稳、标定还没开始就死。
    """
    body = _worker_src()
    head = body[:body.index('try:')]
    assert re.search(r"mo0\['calibration_bias_x'\]\s*=\s*0\.0", head), '入场没清 x 偏置'
    assert re.search(r"mo0\['calibration_bias_y'\]\s*=\s*0\.0", head), '入场没清 y 偏置'


def test_worker_finally_clears_bias():
    """★ 中途取消/失败若留着 calibration_bias_*，参考点会被永久顶偏只能重启。

    这是源码级反向锁：删掉那两行就会红。
    """
    body = _worker_src()
    fin = body[body.index('finally:'):]
    assert re.search(r"mo\['calibration_bias_x'\]\s*=\s*0\.0", fin), 'finally 没清 x 偏置'
    assert re.search(r"mo\['calibration_bias_y'\]\s*=\s*0\.0", fin), 'finally 没清 y 偏置'
    assert re.search(r"mo\['calibrating'\]\s*=\s*False", fin)


# ===========================================================================
# 7. 分母必须来自 count 增量，不能是幅度
# ===========================================================================

def test_observation_uses_count_delta_as_injected_count():
    body = _worker_src()
    m = re.search(r'axis_observations\[axis\]\.append\(CalibrationObservation\((.*?)\)\)',
                  body, re.S)
    assert m, '找不到构造观测的地方'
    block = m.group(1)
    assert 'injected_count=float(d_counts)' in block, '分母不是真实 count 增量'
    assert 'injected_count=float(amp)' not in block, '分母又退回幅度 px 了'
    assert 'measured_delta_px=d_px' in block


def test_worker_rejects_samples_that_never_reached_the_device():
    body = _worker_src()
    assert 'mouse_control_socket_write_ok' in _src(), '没读写入计数就判不了"注入是否落地"'
    assert 'if no_write' in body or 'no_write:' in body


def test_payload_exposes_new_diagnostics():
    """面板/排障需要看到：本轮幅度(px)、等静止耗时、被丢掉的样本数。"""
    s = _src()
    for key in ('amplitude_px', 'settle_ms', 'settled', 'dropped_sample_count'):
        assert "'%s'" % key in s, f'payload 缺 {key}'


# ===========================================================================
# 8. 标定期"温和档" PID（2026-09-24 板上 A/B 定死）
#
#   实战参数（kp=0.25/kd=0.25，V1.0.13 起都是**生效值**）在 ~50ms 采集回路延迟下，
#   bias 阶跃（±8..32px 换向）会把环打进持续振荡（实测准星 ±150px），目标被甩出画面
#   ⇒ 整轮 no_target。kp=0.10/kd=0.30 同一链路 16 轮全稳、gain 一致性 0.94/0.90。
#   gain=Δpx/ΔΣcounts 是闭环恒等式 ⇒ 压 PID 不影响测量结果。
# ===========================================================================

def test_calib_gentle_pid_constants_are_sane(web_mod):
    # ★ V1.0.13：10.0 是**旧名义值**（实际生效 0.10）。kp 换成生效值之后这里必须同步，
    #   否则标定期会把 10 直接写进 mouse.kp_x ⇒ 环路自激 100 倍 ⇒ 整轮标定白跑。
    assert 0.04 <= web_mod.CALIB_PID_KP_MAX <= 0.15
    assert 2.0 <= web_mod.CALIB_PID_KD_RATIO <= 4.0


def test_worker_enters_with_gentle_pid_and_saves_original():
    """入场必须：先保存用户原 kp/kd，再把温和档写进**第一次** SET_CONFIG。"""
    body = _worker_src()
    assert "saved_kp = mo0.get('kp_x')" in body, '没保存用户原 KP'
    assert "saved_kd = mo0.get('kd_x')" in body, '没保存用户原 KD'
    first_set = body.index("ipc_request('SET_CONFIG'")
    head = body[:first_set]
    assert "mo0['kp_x'] = calib_kp" in head
    assert "mo0['kp_y'] = calib_kp" in head
    assert 'CALIB_PID_KP_MAX' in head, 'KP 没被压到温和档'
    assert "mo0['kd_x'] = calib_kp * CALIB_PID_KD_RATIO" in head


def test_worker_restores_pid_on_failure_but_never_on_success():
    """失败/取消要恢复用户原 kp/kd；成功路径若恢复会把刚推导的参数覆盖掉。"""
    body = _worker_src()
    fin = body[body.index('    finally:'):]
    assert "in ('failed', 'cancelled')" in fin, '恢复必须只发生在失败/取消'
    assert "mo['kp_x'] = mo['kp_y'] = saved_kp" in fin
    assert "mo['kd_x'] = mo['kd_y'] = saved_kd" in fin


def test_worker_raises_kp_when_low_gain_samples_are_dropped():
    """注入生效但位移不够（低 gain 系统温和档太慢）⇒ 轮间抬 KP，上限是用户原值。"""
    body = _worker_src()
    assert 'calib_kp * 1.7' in body
    assert 'd_px < CALIB_MIN_DELTA_PX' in body
