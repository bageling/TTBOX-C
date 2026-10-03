# -*- coding: utf-8 -*-
"""remote_guard —— 从 bin/ttbox-web.py 原样搬出（2026-10-02 web 换写法 S10 第一步）。

远端管理（从另一台设备拉模型过来）的「未就绪」占位响应。
★ 目前恒返回「未启用」，是留着的扩展点。
搬出 1 个函数：_remote_not_ready

★ 本模块在 lib/，**不能 import 入口** ⇒ 段外依赖一律经 hub 调用时取。
"""

from __future__ import annotations

import json

from flask import jsonify
from plugins.web.lib import hub

def _LOG():
    """入口的 _LOG —— 调用时取（常量 / 单例，必须与入口**同一个对象**）。"""
    return hub.get('_LOG')

def _remote_not_ready():
    # 保持 Web 契约：未连接 Windows 电脑时提示输入局域网 IP
    return jsonify({
        'ok': False,
        'error': '请输入 Windows 电脑局域网 IP',
    })
