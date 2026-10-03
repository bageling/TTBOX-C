# -*- coding: utf-8 -*-
"""api/calib.py —— 自动标定域 Blueprint。

从 bin/ttbox-web.py 搬出（2026-10-02 web 换写法 S9-b）。
搬出 5 条路由，**URL 一字未改**。

自动标定生命周期（读状态 / 改参数 / 开始 / 取消 / 清除）。

标定算法本体在 `lib/calibration.py`（794 行）—— 本文件只做 HTTP 编排。
★ `_cal` / `_cal_lock` 是**运行期共享状态**（标定线程与本域并发读写），
  经 hub.get 取同一份对象，各自 new 等于没锁。
★ `_calib_target` 是补丁锚点（3 处 monkeypatch），必须经 hub。
段外依赖处理：
    · _cal / _cal_lock / _calib_target —— 经 hub.get 调用时取。
    · _calib_apply_gain / _calib_apply_pid / _calib_derive_pid / _calib_set / _calib_thread_entry / _clear_calibration / _get_runtime_profile / _get_status / _read_active_model / _write_calibration / ipc_request —— 经 hub.call 调用时取。
    · jsonify / request —— flask，直接 import。
    · _calibration_payload(from calibration) —— lib里已有，直接 import。

★ **禁止在 except 子句里调 hub**（求值时机是函数定义时，此时 hub.bind 还没执行 ⇒ 永久绑成 None ⇒ 该 except 永不匹配）。
  需要按异常类型分流时，抽 `_is_xxx(exc)` 函数在调用时才取类。
"""

from __future__ import annotations

from flask import Blueprint, jsonify, request

from plugins.web.lib import hub
from plugins.web.lib.calibration import _calibration_payload
import threading
import time

from plugins.web.lib.calibration import _cal
from plugins.web.lib.calibration import _cal_lock

bp = Blueprint('calib', __name__)

def _calib_target(*args, **kwargs):
    """入口的 _calib_target —— 调用时取（monkeypatch 锚点，转发须**原样透传**：
    测试替身常是少参数的 lambda，补默认值会多塞实参而 TypeError）。"""
    return hub.call('_calib_target', *args, **kwargs)


def _calib_apply_gain(*args, **kwargs):
    """入口的 _calib_apply_gain —— 调用时取（monkeypatch 锚点）。"""
    return hub.call('_calib_apply_gain', *args, **kwargs)


def _calib_apply_pid(*args, **kwargs):
    """入口的 _calib_apply_pid —— 调用时取（monkeypatch 锚点）。"""
    return hub.call('_calib_apply_pid', *args, **kwargs)


def _calib_derive_pid(*args, **kwargs):
    """入口的 _calib_derive_pid —— 调用时取（monkeypatch 锚点）。"""
    return hub.call('_calib_derive_pid', *args, **kwargs)


def _calib_set(*args, **kwargs):
    """入口的 _calib_set —— 调用时取（monkeypatch 锚点）。"""
    return hub.call('_calib_set', *args, **kwargs)


def _calib_thread_entry(*args, **kwargs):
    """入口的 _calib_thread_entry —— 调用时取（monkeypatch 锚点）。"""
    return hub.call('_calib_thread_entry', *args, **kwargs)


def _clear_calibration(*args, **kwargs):
    """入口的 _clear_calibration —— 调用时取（monkeypatch 锚点）。"""
    return hub.call('_clear_calibration', *args, **kwargs)


def _get_runtime_profile(*args, **kwargs):
    """入口的 _get_runtime_profile —— 调用时取（monkeypatch 锚点）。"""
    return hub.call('_get_runtime_profile', *args, **kwargs)


def _get_status(*args, **kwargs):
    """入口的 _get_status —— 调用时取（monkeypatch 锚点）。"""
    return hub.call('_get_status', *args, **kwargs)


def _read_active_model(*args, **kwargs):
    """入口的 _read_active_model —— 调用时取（monkeypatch 锚点）。"""
    return hub.call('_read_active_model', *args, **kwargs)


def _write_calibration(*args, **kwargs):
    """入口的 _write_calibration —— 调用时取（monkeypatch 锚点）。"""
    return hub.call('_write_calibration', *args, **kwargs)


def ipc_request(*args, **kwargs):
    """入口的 ipc_request —— 调用时取（monkeypatch 锚点）。"""
    return hub.call('ipc_request', *args, **kwargs)

@bp.get('/api/control/calibration')
def get_auto_calibration():
    return jsonify({'ok': True, 'data': _calibration_payload()})


@bp.put('/api/control/calibration')
def update_auto_calibration():
    body = request.get_json(silent=True) or {}
    try:
        gain_x = float(body.get('gain_x_px_per_count') or body.get('mouse_gain_x_px_per_count') or 0)
        gain_y = float(body.get('gain_y_px_per_count') or body.get('mouse_gain_y_px_per_count') or 0)
        delay = float(body.get('response_delay_ms') or body.get('mouse_response_delay_ms') or 0)
    except (TypeError, ValueError):
        return jsonify({'ok': False, 'error': '参数格式错误'}), 400
    if gain_x <= 0 or gain_y <= 0:
        return jsonify({'ok': False, 'error': '增益必须 > 0'}), 400
    calib = {
        'mouse_gain_x_px_per_count': round(gain_x, 4),
        'mouse_gain_y_px_per_count': round(gain_y, 4),
        'mouse_response_delay_ms': round(delay, 3),
        'mouse_calibration_applied': True,
        'valid': True,
        'confidence': 0.0,
        'calibrated_at': time.strftime('%Y%m%d_%H%M%S'),
        'model_id': _read_active_model(),
    }
    ok, detail = _write_calibration(calib)
    if ok:
        ok2, detail2 = _calib_apply_gain(calib)
        detail = detail + '；' + detail2
        # 手动填增益同样联动自动调参（同一推导函数，保证行为一致）
        try:
            calib['pid_params'] = _calib_derive_pid(gain_x, gain_y, delay)
        except Exception:
            calib['pid_params'] = {}
        if ok2 and calib.get('pid_params'):
            ok3, detail3 = _calib_apply_pid(calib)
            detail = detail + '；' + detail3
            ok = ok and ok3
            _write_calibration(calib)
        if ok:
            _calib_set(status='manual', phase='done', ready=True, reason='completed')
        else:
            # Core 未运行时文件已保存但参数未生效：不标记完成（诚实反映）
            _calib_set(status='manual', phase='saved', ready=False, reason=detail)
    resp = jsonify({'ok': ok, 'data': _calibration_payload(), 'detail': detail})
    resp.status_code = 200 if ok else 500
    return resp


@bp.post('/api/control/calibration/start')
def start_auto_calibration():
    with _cal_lock:
        if _cal['thread'] and _cal['thread'].is_alive():
            return jsonify({'ok': False, 'error': '标定已在运行中'}), 409
    st = _get_status()
    if not st.get('runtime_running'):
        return jsonify({'ok': False, 'error': '推理服务未运行或目标反馈未就绪（请先启动推理）'}), 400
    if _calib_target() is None:
        return jsonify({'ok': False, 'error': '未识别到目标，无法开始标定（请将准星对准画面中的目标，等待检测框稳定出现）'}), 400
    # V1.0.12（2026-09-30）：不再接受 scope_index —— 不区分倍镜，标定只写全局 gain。
    # 老面板若仍带这个字段，直接忽略（不报错，保持向后兼容）。
    th = threading.Thread(target=_calib_thread_entry, daemon=True)
    with _cal_lock:
        _cal['thread'] = th
    th.start()
    return jsonify({'ok': True, 'data': _calibration_payload(), 'detail': '标定已启动'})


@bp.post('/api/control/calibration/cancel')
def cancel_auto_calibration():
    # 无条件落终态 `cancelled`：worker 还活着时它会在 1~2 秒内自己退出并补一次同样的
    # state（幂等）；worker 已因异常穿透而死、state 停在非终态时（见 _calib_worker 的
    # except 注释），**只有这里能复位** —— 否则面板会永远以为在标定、且取消按钮被禁用。
    _calib_set(state='cancelled', status='idle', phase='cancelled', ready=False, reason='cancelled')
    try:
        prof = _get_runtime_profile()
        prof.setdefault('mouse', {})['calibrating'] = False
        ipc_request('SET_CONFIG', {'profile': prof})
    except Exception:
        pass
    return jsonify({'ok': True, 'data': _calibration_payload(), 'detail': '标定已取消'})


@bp.delete('/api/control/calibration')
def clear_auto_calibration():
    _clear_calibration()
    return jsonify({'ok': True, 'data': _calibration_payload(), 'detail': '标定已清除'})

