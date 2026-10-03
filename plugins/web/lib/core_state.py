# -*- coding: utf-8 -*-
"""core_state —— 从 bin/ttbox-web.py 原样搬出（2026-10-02 web 换写法 S10 第一步）。

把 Core 运行态 + 云端授权 + 自启状态聚成 /api/state 的一部分。
★ 这个模块是 `state_snapshot.py` 的 17 个 hub 依赖里的一部分 ——
  聚合器与被聚合者都在 lib，方向一致（lib → lib 单向，无环）。
★ `_cloud_license_subblock` 既在本模块实现（供别人调），
  入口也re-export 它（外部测试用）⇒ 两边是同一个函数对象。
搬出 2 个函数：_core_state_payload / _cloud_license_subblock

★ 本模块在 lib/，**不能 import 入口** ⇒ 段外依赖一律经 hub 调用时取。
"""

from __future__ import annotations

import json
import subprocess

from plugins.web.lib import hub

def _CLOUD_SESSION():
    """入口的 _CLOUD_SESSION —— 调用时取（常量 / 单例，必须与入口**同一个对象**）。"""
    return hub.get('_CLOUD_SESSION')


def _HEARTBEAT():
    """入口的 _HEARTBEAT —— 调用时取（常量 / 单例，必须与入口**同一个对象**）。"""
    return hub.get('_HEARTBEAT')


def _LOG():
    """入口的 _LOG —— 调用时取（常量 / 单例，必须与入口**同一个对象**）。"""
    return hub.get('_LOG')


def _auto_start_payload():
    """入口的 _auto_start_payload —— 调用时取（常量 / 单例，必须与入口**同一个对象**）。"""
    return hub.get('_auto_start_payload')


def _get_status():
    """入口的 _get_status —— 调用时取（常量 / 单例，必须与入口**同一个对象**）。"""
    return hub.get('_get_status')


def _license_block():
    """入口的 _license_block —— 调用时取（常量 / 单例，必须与入口**同一个对象**）。"""
    return hub.get('_license_block')


def _ota_current_version():
    """入口的 _ota_current_version —— 调用时取（常量 / 单例，必须与入口**同一个对象**）。"""
    return hub.get('_ota_current_version')


def _ui_block():
    """入口的 _ui_block —— 调用时取（常量 / 单例，必须与入口**同一个对象**）。"""
    return hub.get('_ui_block')


def kAppVersion():
    """入口的 kAppVersion —— 调用时取（常量 / 单例，必须与入口**同一个对象**）。"""
    return hub.get('kAppVersion')


def _cloud_license_subblock(*args, **kwargs):
    """入口的 _cloud_license_subblock —— 调用时取（monkeypatch 锚点，转发须**原样透传**）。"""
    return hub.call('_cloud_license_subblock', *args, **kwargs)


def _machine_code(*args, **kwargs):
    """入口的 _machine_code —— 调用时取（monkeypatch 锚点，转发须**原样透传**）。"""
    return hub.call('_machine_code', *args, **kwargs)

def _core_state_payload() -> dict:
    """Core 服务真实状态（systemctl is-active ttbox-core），不硬编码。"""
    try:
        out = subprocess.check_output(['systemctl', 'is-active', 'ttbox-core'],
                                      text=True, timeout=3).strip()
    except Exception:
        out = 'inactive'
    if out == 'active':
        return {'installed': True, 'loaded': True, 'status': 'loaded',
                'message': '核心模块已加载', 'version': '2026.05.16'}
    return {'installed': True, 'loaded': False, 'status': 'not_running',
            'message': '核心模块未运行（ttbox-core 未启动）', 'version': '2026.05.16'}


def _cloud_license_subblock() -> dict:
    """/api/license 的 `cloud` 子块（§3.3）：core 投影之上的云端字段增量。

    core 不可达 / 无会话 ⇒ honest 空（online=false、expire_at=''），不造假值。
    """
    sess = _CLOUD_SESSION().load()
    if _HEARTBEAT() is not None:
        hb = _HEARTBEAT().snapshot()
    else:
        hb = {'online': False, 'last_ok_at': 0.0, 'expire_at': '',
              'interval': 60, 'timeout': 180, 'error': ''}
    return {
        'source': 'cloud' if sess else 'none',
        'card_mask': str(sess.get('card_mask', '') or ''),
        'expire_at': str(sess.get('expire_at', '') or ''),
        'max_devices': int(sess.get('max_devices', 0) or 0),
        'heartbeat': {
            'online': bool(hb.get('online')),
            'last_ok_at': float(hb.get('last_ok_at') or 0.0),
            'interval': int(hb.get('interval') or 60),
            'timeout': int(hb.get('timeout') or 180),
            'error': str(hb.get('error') or ''),
        },
        'machine_code': _machine_code(),
    }
