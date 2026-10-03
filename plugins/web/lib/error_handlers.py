# -*- coding: utf-8 -*-
"""error_handlers —— 从 bin/ttbox-web.py 原样搬出（2026-10-02 web 换写法 S10 第一步）。

413（请求体过大）与 Core 不可达两个错误处理器的响应体构造。
★ 这两个已在 S9 挂到 api/hooks.py 的 `bp.app_errorhandler` 上；
  本模块只提供**响应体构造**，注册仍在 api/hooks.py。
★ 千万别改成 `bp.errorhandler` —— 那是 Blueprint 级的，
  只处理本 Blueprint 内路由抛出的异常，而这两个与路由无关。
搬出 2 个函数：_handle_payload_too_large / _handle_core_unavailable

★ 本模块在 lib/，**不能 import 入口** ⇒ 段外依赖一律经 hub 调用时取。
"""

from __future__ import annotations

import json

from flask import jsonify
from plugins.web.lib import hub

def CoreUnavailableError():
    """入口的 CoreUnavailableError —— 调用时取（常量 / 单例，必须与入口**同一个对象**）。"""
    return hub.get('CoreUnavailableError')


def MAX_UPLOAD_BYTES():
    """入口的 MAX_UPLOAD_BYTES —— 调用时取（常量 / 单例，必须与入口**同一个对象**）。"""
    return hub.get('MAX_UPLOAD_BYTES')


def _LOG():
    """入口的 _LOG —— 调用时取（常量 / 单例，必须与入口**同一个对象**）。"""
    return hub.get('_LOG')


def app():
    """入口的 app —— 调用时取（常量 / 单例，必须与入口**同一个对象**）。"""
    return hub.get('app')

@app().errorhandler(413)
def _handle_payload_too_large(_exc):
    """请求体超限：给前端一个能看懂的 JSON，而不是默认的 HTML 413 页。"""
    return jsonify({'ok': False, 'error': f'上传内容过大（上限 {MAX_UPLOAD_BYTES() // (1024 * 1024)} MB）'}), 413


@app().errorhandler(CoreUnavailableError())
def _handle_core_unavailable(exc: CoreUnavailableError):
    """Core IPC 不可达 ⇒ 503 + core_offline 标志（前端据此明示\"Core 离线\"）。

    V-01/V-02 修复的一部分：Web 配置/状态唯一来源 = Core IPC；Core 不可达时
    **如实报错**，绝不返回 {} 或磁盘内容（禁止静默回落）。
    """
    return jsonify({'ok': False, 'error': str(exc), 'core_offline': True}), 503
