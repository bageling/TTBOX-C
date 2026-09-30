# test_web_calibration_hot_reload.py — 标定写回参数后面板热更新（不用刷新页面）
#
# 背景（业主 2026-09-30 指令一）：自动标定跑完，_calib_apply_pid 已经把
# kp/kd/predict_x 写进 core，但面板输入框只在 applyFullState / applyConfigNow
# 时回填 ⇒ 用户看到的还是旧值，只能刷新页面。两处必须同时成立：
#   ① 后端 /api/control/calibration 要把「当前生效的 PID」报出来（effective）；
#   ② 前端到达 completed 终态时要拉一次 /api/config 并 populateForm。
import os
import threading


def _read(rel):
    here = os.path.dirname(os.path.abspath(__file__))
    for base in (os.getcwd(), os.path.dirname(os.path.dirname(here))):
        p = os.path.join(base, rel)
        if os.path.exists(p):
            return open(p, encoding='utf-8').read()
    raise AssertionError('找不到文件: ' + rel)


WEB_SRC = _read('plugins/web/bin/ttbox-web.py')
HTML_SRC = _read('plugins/web/templates/index.html')

CAL_KEYS = ('thread', 'phase', 'state', 'status', 'ready', 'reason', 'total_rounds',
            'round', 'progress', 'current_axis', 'valid_sample_count', 'axis_fits',
            'candidate_count', 'candidate_track_id', 'candidate_class_id',
            'candidate_width', 'candidate_height', 'stable_frames', 'stable_ms',
            'center_jitter_px', 'size_variation', 'elapsed_ms', 'amplitude_counts',
            'amplitude_px', 'settle_ms', 'settled', 'dropped_sample_count',
            'max_tracked_amp')


def _payload(profile, record):
    start = WEB_SRC.index('def _calibration_payload(')
    end = WEB_SRC.index("@app.get('/api/control/calibration')")
    ns = {
        '_cal_lock': threading.Lock(),
        '_cal': {k: (0 if 'width' in k or 'height' in k else None) for k in CAL_KEYS},
        '_read_calibration': lambda: record,
        '_get_runtime_profile': lambda: profile,
    }
    exec(WEB_SRC[start:end], ns)
    return ns['_calibration_payload']()


def test_effective_exposes_live_pid():
    """标定写回的 kp/kd/predict_x 必须从接口里读得到（面板热更新的数据源）。"""
    profile = {'mouse': {
        'gain_x_px_per_count': 0.62, 'gain_y_px_per_count': 0.48,
        'kp_x': 0.1129, 'kd_x': 0.2, 'predict_x': 0.3,
    }}
    eff = _payload(profile, None)['calibration']['effective']
    assert eff.get('kp_x') == 0.1129, eff
    assert eff.get('kd_x') == 0.2, eff
    assert eff.get('predict_x') == 0.3, eff
    # 增益照旧（原有契约不能破）
    assert eff.get('gain_x_px_per_count') == 0.62, eff


def test_effective_empty_when_core_offline():
    """core 离线时生效值留空，绝不拿留档/默认值冒充。"""
    def boom():
        raise RuntimeError('core down')
    start = WEB_SRC.index('def _calibration_payload(')
    end = WEB_SRC.index("@app.get('/api/control/calibration')")
    ns = {
        '_cal_lock': threading.Lock(),
        '_cal': {k: (0 if 'width' in k or 'height' in k else None) for k in CAL_KEYS},
        '_read_calibration': lambda: {'valid': True, 'pid_params': {'kp': 0.1}},
        '_get_runtime_profile': boom,
    }
    exec(WEB_SRC[start:end], ns)
    calib = ns['_calibration_payload']()['calibration']
    assert calib['effective'] == {}, calib


def test_saved_record_exposes_derived_pid():
    """留档里的本轮推导 PID 要能回传给面板展示（不是生效值，只作留痕）。"""
    record = {'valid': True, 'pid_params': {'kp': 0.113, 'kd': 0.2, 'predict': 0.3}}
    calib = _payload({'mouse': {}}, record)['calibration']
    assert calib['pid_params']['kp'] == 0.113, calib


def test_frontend_refreshes_form_on_completed():
    """completed 终态 → 拉 /api/config → populateForm（用户不用刷新页面）。"""
    assert 'syncConfigAfterCalibration' in HTML_SRC
    block = HTML_SRC[HTML_SRC.index('if (["completed", "failed", "cancelled"].includes(state2)'):]
    block = block[:block.index('scheduleCalibrationPolling(running);')]
    assert 'state2 === "completed"' in block, block
    assert 'syncConfigAfterCalibration()' in block, block
    # 只能触发一次：不设闸门会被 800ms 轮询反复拉配置，把用户正在改的输入冲掉
    assert 'state.calibConfigSynced' in block, block


def test_frontend_sync_pulls_config_and_populates():
    assert 'const cfg = await api("/api/config");' in HTML_SRC
    assert 'populateForm(cfg);' in HTML_SRC


def test_start_resets_sync_flag():
    start = HTML_SRC.index('on("calibStartButton"')
    block = HTML_SRC[start:start + 900]
    assert 'state.calibConfigSynced = false;' in block, block
