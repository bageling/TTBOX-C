# -*- coding: utf-8 -*-
"""gate —— 从 bin/ttbox-web.py 原样搬出（2026-10-02 web 换写法 S10 第一步）。

before_request 授权执法：未激活 ⇒ 302/activate。
★ 已在 S9 挂到 api/hooks.py 的 `bp.before_request`；
  本模块只提供执法逻辑本身。
★ **fail-loud 设计**：宁可拦下来，也不让未激活的设备露出面板。
  白名单里的路径（/api/license、/api/update…）在此放行。
搬出 1 个函数：_enforce_gate

★ 本模块在 lib/，**不能 import 入口** ⇒ 段外依赖一律经 hub 调用时取。
"""

from __future__ import annotations

import json

from flask import jsonify, redirect, request
from plugins.web.lib import hub

def _ACTIVATION_PAGES():
    """入口的 _ACTIVATION_PAGES —— 调用时取（常量 / 单例，必须与入口**同一个对象**）。"""
    return hub.get('_ACTIVATION_PAGES')


def _ACTIVATION_WHITELIST():
    """入口的 _ACTIVATION_WHITELIST —— 调用时取（常量 / 单例，必须与入口**同一个对象**）。"""
    return hub.get('_ACTIVATION_WHITELIST')


def _ACTIVATION_WHITELIST_PREFIXES():
    """入口的 _ACTIVATION_WHITELIST_PREFIXES —— 调用时取（常量 / 单例，必须与入口**同一个对象**）。"""
    return hub.get('_ACTIVATION_WHITELIST_PREFIXES')


def _LOG():
    """入口的 _LOG —— 调用时取（常量 / 单例，必须与入口**同一个对象**）。"""
    return hub.get('_LOG')


def _PAGE_ROUTES():
    """入口的 _PAGE_ROUTES —— 调用时取（常量 / 单例，必须与入口**同一个对象**）。"""
    return hub.get('_PAGE_ROUTES')


def app():
    """入口的 app —— 调用时取（常量 / 单例，必须与入口**同一个对象**）。"""
    return hub.get('app')


def _activation_ok(*args, **kwargs):
    """入口的 _activation_ok —— 调用时取（monkeypatch 锚点，转发须**原样透传**）。"""
    return hub.call('_activation_ok', *args, **kwargs)


def _is_static(*args, **kwargs):
    """入口的 _is_static —— 调用时取（monkeypatch 锚点，转发须**原样透传**）。"""
    return hub.call('_is_static', *args, **kwargs)

@app().before_request
def _enforce_gate():
    """★ M2.07 唯一入口执法点（D1/D9）：LAN 黑名单 403 + 激活 gate。

    判定顺序：
      静态/页面 → 页面未激活(302 /activate) → API 白名单 → API 未激活(403 activation_required)。
    鉴权语义已整体退役（D1）：所有 API 免密直通，安全边界由网络层承担。
    """
    path = request.path
    if _is_static(path):
        return None
    if path in _PAGE_ROUTES():
        # 页面路由：需激活的页面在未激活时 → 激活页（302，P0-2）；/activate 本体恒可渲染。
        if path in _ACTIVATION_PAGES() and not _activation_ok():
            return redirect('/activate')
        return None
    # API 面：白名单（激活前可达）或已激活直通
    if (request.method, path) in _ACTIVATION_WHITELIST():
        return None
    for prefix in _ACTIVATION_WHITELIST_PREFIXES():
        if path.startswith(prefix):
            return None
    if not _activation_ok():
        return jsonify({'ok': False, 'error': 'activation_required'}), 403
    return None
