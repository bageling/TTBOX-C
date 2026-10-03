# -*- coding: utf-8 -*-
"""api/aim.py —— 瞄准轨迹域 Blueprint。

从 bin/ttbox-web.py 搬出（2026-10-02 web 换写法 S9-b）。
搬出 1 条路由，**URL 一字未改**。

瞄准轨迹记录开关。板端调试与压枪自整定取证用。
段外依赖处理：
    · json / os / threading / time —— 标准库，直接 import。
    · jsonify / request —— flask，直接 import。

★ **禁止在 except 子句里调 hub**（求值时机是函数定义时，此时 hub.bind 还没执行 ⇒ 永久绑成 None ⇒ 该 except 永不匹配）。
  需要按异常类型分流时，抽 `_is_xxx(exc)` 函数在调用时才取类。
"""

from __future__ import annotations

import json
import os
import threading
import time

from flask import Blueprint, jsonify, request
from plugins.web.lib import paths as ttbox_paths

from plugins.web.lib import hub

bp = Blueprint('aim', __name__)

def _aim_trace(*args, **kwargs):
    """入口的 _aim_trace —— 调用时取。"""
    return hub.call('_aim_trace', *args, **kwargs)


def _get_status(*args, **kwargs):
    """入口的 _get_status —— 调用时取。"""
    return hub.call('_get_status', *args, **kwargs)


@bp.post('/api/diagnostics/aim-trace')
def start_aim_trace():
    body = request.get_json(silent=True) or {}
    duration_sec = int(body.get('duration_sec', 10))
    if duration_sec <= 0 or duration_sec > 120:
        return jsonify({'ok': False, 'error': 'duration_sec 必须在 1~120 之间'})
    if _aim_trace['running']:
        return jsonify({'ok': False, 'error': '已有轨迹记录进行中'})
    # Core 未运行时拒绝开始（避免记录全 0 假轨迹）
    st = _get_status()
    if not st.get('runtime_running'):
        return jsonify({'ok': False, 'error': '推理服务未运行，无法记录瞄准轨迹'}), 400
    _aim_trace['running'] = True
    _aim_trace['samples'] = []
    _aim_trace['started_at'] = time.time()
    _aim_trace['stop_at'] = time.time() + duration_sec

    def _collect():
        while _aim_trace['running'] and time.time() < _aim_trace['stop_at']:
            try:
                st = _get_status()
                m = st.get('metrics', {})
                _aim_trace['samples'].append({
                    't': round(time.time() - _aim_trace['started_at'], 3),
                    'err_x': round(m.get('aim_error_x', 0.0), 3),
                    'err_y': round(m.get('aim_error_y', 0.0), 3),
                    'move_x': m.get('mouse_dx', 0),
                    'move_y': m.get('mouse_dy', 0),
                    'target': m.get('target_frames', 0) > 0 or m.get('aim_active', False),
                })
            except Exception:
                pass
            time.sleep(0.02)
        try:
            _run_dir = ttbox_paths.prefix_path('run')
            os.makedirs(_run_dir, exist_ok=True)
            with open(ttbox_paths.join_path(_run_dir, 'aim_trace.json'), 'w') as f:
                json.dump({'samples': _aim_trace['samples'], 'duration_sec': duration_sec}, f)
        except Exception:
            pass
        _aim_trace['running'] = False

    _aim_trace['thread'] = threading.Thread(target=_collect, daemon=True)
    _aim_trace['thread'].start()
    # 保持 Web 契约：返回 duration_sec/filename/path/recording 结构
    fname = f'aim_trace_{time.strftime("%Y%m%d_%H%M%S")}.jsonl'
    return jsonify({'ok': True, 'data': {
        'duration_sec': float(duration_sec),
        'filename': fname,
        'path': ttbox_paths.join_path(ttbox_paths.prefix_path('run'), fname),
        'recording': True,
    }})
