# -*- coding: utf-8 -*-
"""model_patch —— 从 bin/ttbox-web.py 原样搬出（2026-10-02 web 换写法 S10 第一步）。

模型字段更新后的统一响应投影（改完模型就回一份最新列表给面板）。
★ 面板依赖这个响应形状（data.models + version），改字段名要连带改前端。
搬出 1 个函数：_models_patch_response

★ 本模块在 lib/，**不能 import 入口** ⇒ 段外依赖一律经 hub 调用时取。
"""

from __future__ import annotations

import json

from plugins.web.lib import hub

def _LOG():
    """入口的 _LOG —— 调用时取（常量 / 单例，必须与入口**同一个对象**）。"""
    return hub.get('_LOG')


def _get_runtime_profile():
    """入口的 _get_runtime_profile —— 调用时取（常量 / 单例，必须与入口**同一个对象**）。"""
    return hub.get('_get_runtime_profile')


def _models_view():
    """入口的 _models_view —— 调用时取（常量 / 单例，必须与入口**同一个对象**）。"""
    return hub.get('_models_view')


def _write_model_ui_meta(*args, **kwargs):
    """入口的 _write_model_ui_meta —— 调用时取（monkeypatch 锚点，转发须**原样透传**）。"""
    return hub.call('_write_model_ui_meta', *args, **kwargs)


def ipc_request(*args, **kwargs):
    """入口的 ipc_request —— 调用时取（monkeypatch 锚点，转发须**原样透传**）。"""
    return hub.call('ipc_request', *args, **kwargs)


def list_models(*args, **kwargs):
    """入口的 list_models —— 调用时取（monkeypatch 锚点，转发须**原样透传**）。"""
    return hub.call('list_models', *args, **kwargs)

def _models_patch_response(model_id: str, patch: dict):
    """统一返回：校验模型存在 → 写 ui_meta → 附带最新模型列表。模型不可用时返回 None。"""
    check = ipc_request('MODEL_LIST')
    if check.get('status') != 0:
        return None
    known = [m.get('model_id') for m in check.get('data', {}).get('models', [])]
    if model_id not in known:
        return None
    _write_model_ui_meta(model_id, patch)
    models_resp = list_models()
    models_data = {}
    if models_resp is not None:
        try:
            models_data = models_resp.get_json() or {}
        except Exception:
            models_data = {}
    return {'ok': True, 'data': {
        'message': '已保存',
        'model_id': model_id,
        **(models_data.get('data') or {}),
    }}
