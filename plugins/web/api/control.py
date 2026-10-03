# -*- coding: utf-8 -*-
"""api/control.py —— 控制流启停域 Blueprint。

从 bin/ttbox-web.py 搬出（2026-10-02 web 换写法 S9-b）。
搬出 2 条路由，**URL 一字未改**。

瞄准控制的启动/停止。停止后立即回读 collect_web_state 让面板刷新。
段外依赖处理：
    · collect_web_state —— 经 hub.call 调用时取。
    · jsonify —— flask，直接 import。

★ **禁止在 except 子句里调 hub**（求值时机是函数定义时，此时 hub.bind 还没执行 ⇒ 永久绑成 None ⇒ 该 except 永不匹配）。
  需要按异常类型分流时，抽 `_is_xxx(exc)` 函数在调用时才取类。
"""

from __future__ import annotations

from flask import Blueprint, jsonify

from plugins.web.lib import hub

bp = Blueprint('control', __name__)

def _get_runtime_profile(*args, **kwargs):
    """入口的 _get_runtime_profile —— 调用时取。"""
    return hub.call('_get_runtime_profile', *args, **kwargs)


def collect_web_state(*args, **kwargs):
    """入口的 collect_web_state —— 调用时取。"""
    return hub.call('collect_web_state', *args, **kwargs)


def ipc_request(*args, **kwargs):
    """入口的 ipc_request —— 调用时取。"""
    return hub.call('ipc_request', *args, **kwargs)


def _collect_web_state(*args, **kwargs):
    """入口的 collect_web_state —— 调用时取（monkeypatch 锚点）。"""
    return hub.call('collect_web_state', *args, **kwargs)

@bp.post('/api/control/start')
def start_control():
    # 保持 Web 契约：无模型时返回 error=未导入模型
    prof = _get_runtime_profile()
    if not prof.get('model_id'):
        return jsonify({'ok': False, 'error': '未导入模型'})
    r = ipc_request('RUNTIME_CONTROL', {'action': 'start'})
    if r.get('status') != 0:
        err = r.get('error') or '启动失败'
        if '模型' not in err and 'model' not in err.lower():
            err = '未导入模型'
        return jsonify({'ok': False, 'error': err})
    state = collect_web_state()
    data = state.get('data', state) if isinstance(state, dict) else state
    return jsonify({'ok': True, 'data': data})


@bp.post('/api/control/stop')
def stop_control():
    r = ipc_request('RUNTIME_CONTROL', {'action': 'stop'})
    state = collect_web_state()
    data = state.get('data', state) if isinstance(state, dict) else state
    return jsonify({'ok': r.get('status') == 0, 'data': data})
