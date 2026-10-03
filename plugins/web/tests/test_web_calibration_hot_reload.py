# test_web_calibration_hot_reload.py — 标定写回参数后面板热更新（不用刷新页面）
#
# 背景（业主 2026-09-30 指令一）：自动标定跑完，_calib_apply_pid 已经把
# kp/kd/predict_x 写进 core，但面板输入框只在 applyFullState / applyConfigNow
# 时回填 ⇒ 用户看到的还是旧值，只能刷新页面。两处必须同时成立：
#   ① 后端 /api/control/calibration 要把「当前生效的 PID」报出来（effective）；
#   ② 前端到达 completed 终态时要拉一次 /api/config 并 populateForm。
import os
import re
import threading


def _read(rel):
    here = os.path.dirname(os.path.abspath(__file__))
    for base in (os.getcwd(), os.path.dirname(os.path.dirname(here))):
        p = os.path.join(base, rel)
        if os.path.exists(p):
            return open(p, encoding='utf-8').read()
    raise AssertionError('找不到文件: ' + rel)


WEB_SRC = _read('plugins/web/bin/ttbox-web.py')
# ★ 2026-10-02（web 换写法 S4）：标定实现搬到 lib/calibration.py，
#   _calibration_payload 整体在那边；命名空间注入的东西不变。
CALIB_SRC = _read('plugins/web/lib/calibration.py')
HTML_SRC = (_read('plugins/web/templates/index.html')
     + _read('plugins/web/static/panel.css')

     + _read('plugins/web/static/panel/00-const.js')
     + _read('plugins/web/static/panel/10-flow.js')
     + _read('plugins/web/static/panel/calib-bind.js')
     + _read('plugins/web/static/panel/01-home.js')
     + _read('plugins/web/static/panel/02-hotkey.js')
     + _read('plugins/web/static/panel/03-pointer.js')
     + _read('plugins/web/static/panel/04-assist.js')
     + _read('plugins/web/static/panel/05-model.js')
     + _read('plugins/web/static/panel/06-hardware.js')
     + _read('plugins/web/static/panel/07-preset.js')
     + _read('plugins/web/static/panel/08-license.js')
     + _read('plugins/web/static/panel/09-fan.js'))


def _func_src(text, name):
    """取顶层函数源码：起点是 `def <name>(`，终点是下一个顶层 def（没有就到文件尾）。
    S4 前用的是入口里 `@app.get('/api/control/calibration')` 当终点，搬到 lib 后没这个锚点了。"""
    start = text.index('def %s(' % name)
    nxt = text.find('\n\ndef ', start)
    return text[start:] if nxt < 0 else text[start:nxt + 1]

CAL_KEYS = ('thread', 'phase', 'state', 'status', 'ready', 'reason', 'total_rounds',
            'round', 'progress', 'current_axis', 'valid_sample_count', 'axis_fits',
            'candidate_count', 'candidate_track_id', 'candidate_class_id',
            'candidate_width', 'candidate_height', 'stable_frames', 'stable_ms',
            'center_jitter_px', 'size_variation', 'elapsed_ms', 'amplitude_counts',
            'amplitude_px', 'settle_ms', 'settled', 'dropped_sample_count',
            'max_tracked_amp')


def _payload(profile, record):
    ns = {
        '_cal_lock': threading.Lock(),
        '_cal': {k: (0 if 'width' in k or 'height' in k else None) for k in CAL_KEYS},
        '_read_calibration': lambda: record,
        '_get_runtime_profile': lambda: profile,
    }
    exec(_func_src(CALIB_SRC, '_calibration_payload'), ns)
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
    ns = {
        '_cal_lock': threading.Lock(),
        '_cal': {k: (0 if 'width' in k or 'height' in k else None) for k in CAL_KEYS},
        '_read_calibration': lambda: {'valid': True, 'pid_params': {'kp': 0.1}},
        '_get_runtime_profile': boom,
    }
    exec(_func_src(CALIB_SRC, '_calibration_payload'), ns)
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
    # ★ 2026-10-03：renderCalibration 拆成 5 段后，这段终态处理搬进了
    #   calibHandleTerminalState()，块尾不再是 scheduleCalibrationPolling(running)。
    #   改用「下一个顶层函数定义」作为块尾——意图（这段逻辑必须在 completed 时
    #   拉一次配置）不变，只是不再依赖某个具体调用恰好在后面。
    tail = HTML_SRC.find('\nfunction ', HTML_SRC.index('if (["completed", "failed", "cancelled"].includes(state2)'))
    assert tail > 0, '终态处理块之后应有下一个顶层函数'
    block = block[:tail]
    assert 'state2 === "completed"' in block, block
    assert 'syncConfigAfterCalibration()' in block, block
    # 只能触发一次：不设闸门会被 800ms 轮询反复拉配置，把用户正在改的输入冲掉
    assert 'state.calibConfigSynced' in block, block
    # ★ 2026-10-03 补强：原断言只查「calibConfigSynced 字样存在」，
    #   故障注入证明把 `state.calibConfigSynced = true;` 整行删掉它照样绿
    #   （字样还在别处被读，但闸门永远不置位 ⇒ 每次 completed 都重复拉配置）。
    #   ⇒ 必须同时锁住「读」与「写」两侧，且写的那行在同一个 if 块内。
    assert re.search(r'!\s*state\.calibConfigSynced', block), \
        '闸门读侧缺失：应看到 !state.calibConfigSynced，实际\n%s' % block
    assert re.search(r'state\.calibConfigSynced\s*=\s*true', block), \
        '闸门写侧缺失：应看到 state.calibConfigSynced = true，实际\n%s' % block


def test_frontend_sync_pulls_config_and_populates():
    assert 'const cfg = await api("/api/config");' in HTML_SRC
    assert 'populateForm(cfg);' in HTML_SRC


def test_start_resets_sync_flag():
    start = HTML_SRC.index('on("calibStartButton"')
    block = HTML_SRC[start:start + 900]
    assert 'state.calibConfigSynced = false;' in block, block
