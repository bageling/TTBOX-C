# -*- coding: utf-8 -*-
"""power —— 从 bin/ttbox-web.py 原样搬出（2026-10-02 web 换写法 S10 第一步）。

reboot / poweroff 的实际执行 + 读-改-写时的配置深合并。
★ `_power_action_allowed` 是补丁锚点（2 处 monkeypatch，
  替换 logind 查询结果来测「无权限时不能声称成功」）⇒ hub 转发。
★ **深合并必须持配置写锁**：不持锁时 waitress 64 线程并发下
  A 读→B 读→A 写→B 写，A 的修改会被 B 的整份快照静默抹掉
  （现象是「设置偶发不生效 / 被改回去」）。
搬出 2 个函数：_power_action / _deep_merge_profile

★ 本模块在 lib/，**不能 import 入口** ⇒ 段外依赖一律经 hub 调用时取。
"""

from __future__ import annotations

import json
import os
import subprocess
import threading
import time

from flask import jsonify, request
from plugins.web.lib import hub

def _LOG():
    """入口的 _LOG —— 调用时取（常量 / 单例，必须与入口**同一个对象**）。"""
    return hub.get('_LOG')


def _get_runtime_profile():
    """入口的 _get_runtime_profile —— 调用时取（常量 / 单例，必须与入口**同一个对象**）。"""
    return hub.get('_get_runtime_profile')


def ipc_request():
    """入口的 ipc_request —— 调用时取（常量 / 单例，必须与入口**同一个对象**）。"""
    return hub.get('ipc_request')


def _power_action_allowed(*args, **kwargs):
    """入口的 _power_action_allowed —— 调用时取（monkeypatch 锚点，转发须**原样透传**）。"""
    return hub.call('_power_action_allowed', *args, **kwargs)

def _power_action(verb: str):
    body = request.get_json(silent=True) or {}
    allowed, why = _power_action_allowed(verb)
    data = {'action': verb, 'scheduled': False, 'allowed': allowed, 'reason': why}
    if body.get('dry_run'):
        # dry_run 是验收脚本用的连通性自检（按 200 判定鉴权门是否放行），沿用 200。
        # 真实权限结论放在 allowed/reason，调用方可自行判断。
        return jsonify({'ok': True, 'data': data})
    if not allowed:
        return jsonify({'ok': False, 'error': why, 'data': data}), 403
    threading.Thread(target=lambda: (time.sleep(1.5), os.system('systemctl ' + verb)),
                     daemon=True).start()
    data['scheduled'] = True
    return jsonify({'ok': True, 'data': data})


def _deep_merge_profile(base: dict, patch: dict) -> dict:
    """RuntimeProfile 深合并：子对象（capture/fov/mouse/...）按键级合并而非整体替换。

    Web 前端每次 PUT 都是全量 collectConfig，但翻译层只产出非空子集；
    若浅合并，未提交的子对象（如 geometry_filter）会被 partial dict 整体顶掉，
    导致"保存一个字段 → 其它字段全丢"的参数失效问题。
    """
    merged = dict(base)
    for k, v in patch.items():
        if isinstance(v, dict) and isinstance(merged.get(k), dict):
            merged[k] = _deep_merge_profile(merged[k], v)
        else:
            merged[k] = v
    return merged
