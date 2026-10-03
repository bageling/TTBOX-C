# -*- coding: utf-8 -*-
"""license_payload —— 从 bin/ttbox-web.py 原样搬出（2026-10-02 web 换写法 S10 第一步）。

把云端授权块投影成前端要的 payload 形状（脱敏 + 格式化）。
★ 脱敏逻辑在这里，改它要连带看前端：
  面板只认这组字段名与状态字符串（active / grace / expired…）。
★ `_LOG` 是 logger 实例（常量语义）⇒ hub.get，
  必须与入口同一个 logger，否则日志会分裂到不同 handler。
★ `CloudLicenseError` 是异常类 ⇒ 这里只做转发，
  **绝不能出现在 except 子句**（求值时机是定义时，
  那时 hub.bind 还没跑 ⇒ 永久绑成 None ⇒ 该 except 永不匹配）。
搬出 1 个函数：_license_payload

★ 本模块在 lib/，**不能 import 入口** ⇒ 段外依赖一律经 hub 调用时取。
"""

from __future__ import annotations

import json
import os
import re

from plugins.web.lib import hub

def CloudLicenseError():
    """入口的 CloudLicenseError —— 调用时取（常量 / 单例，必须与入口**同一个对象**）。"""
    return hub.get('CloudLicenseError')


def _LOG():
    """入口的 _LOG —— 调用时取（常量 / 单例，必须与入口**同一个对象**）。"""
    return hub.get('_LOG')


def kAppVersion():
    """入口的 kAppVersion —— 调用时取（常量 / 单例，必须与入口**同一个对象**）。"""
    return hub.get('kAppVersion')


def _auto_start_payload(*args, **kwargs):
    """入口的 _auto_start_payload —— 调用时取（monkeypatch 锚点，转发须**原样透传**）。"""
    return hub.call('_auto_start_payload', *args, **kwargs)


def _cloud_license_subblock(*args, **kwargs):
    """入口的 _cloud_license_subblock —— 调用时取（monkeypatch 锚点，转发须**原样透传**）。"""
    return hub.call('_cloud_license_subblock', *args, **kwargs)


def _core_state_payload(*args, **kwargs):
    """入口的 _core_state_payload —— 调用时取（monkeypatch 锚点，转发须**原样透传**）。"""
    return hub.call('_core_state_payload', *args, **kwargs)


def _license_block(*args, **kwargs):
    """入口的 _license_block —— 调用时取（monkeypatch 锚点，转发须**原样透传**）。"""
    return hub.call('_license_block', *args, **kwargs)


def _ota_current_version(*args, **kwargs):
    """入口的 _ota_current_version —— 调用时取（monkeypatch 锚点，转发须**原样透传**）。"""
    return hub.call('_ota_current_version', *args, **kwargs)


def _ui_block(*args, **kwargs):
    """入口的 _ui_block —— 调用时取（monkeypatch 锚点，转发须**原样透传**）。"""
    return hub.call('_ui_block', *args, **kwargs)

def _license_payload() -> dict:
    """保持 Web 契约 license 结构：app_version/auto_start/core/device/license/ui/ui_brand/version。"""
    import glob as _glob
    # 真实设备指纹（保持 Web 契约 device）
    cpu_serial = ''
    try:
        with open('/proc/cpuinfo') as f:
            for line in f:
                if line.startswith('Serial'):
                    cpu_serial = line.split(':', 1)[1].strip()
                    break
    except Exception:
        pass
    macs = []
    try:
        for iface in ('enP3p49s0', 'eth0', 'end0', 'wlan0'):
            p = f'/sys/class/net/{iface}/address'
            if os.path.exists(p):
                macs.append(open(p).read().strip())
    except Exception:
        pass
    device_id = f'opi-{cpu_serial}' if cpu_serial else 'opi-ttbox-local'
    fingerprint = device_id
    # ---- T1.07b：license 子块 = core IPC 投影（单一真相源）----
    # 授权语义（activated/state/plan/is_pro/features/到期/诊断）**全部来自 core**，
    # Web 零推导；被删的伪造字段（本地 license_id / license_version / max_version /
    # 硬编码 issued_at 与"永不到期"expires_at / features 恒满配 / online_* 伪值）
    # 均无 IPC 来源 ⇒ **不得回填**（★ 注释里也不写这些字面量，否则 §4.2 全仓 grep 门禁
    # 会被自己的注释命中，门禁形同虚设）。
    # device_id / fingerprint 是真实设备读取（/proc/cpuinfo + /sys/class/net），非授权源，保留。
    # 与 /api/state 共用 _license_block()，杜绝两处投影分叉。
    license_data = dict(_license_block())
    license_data['device_id'] = device_id
    license_data['device_fingerprint_hash'] = fingerprint
    # ui 块 = 品牌表投影（唯一来源，与 /api/state 同源，杜绝两处分叉）。
    # S1-2026-09-18：无线/AP 热点已随无线页签整体移除，default_hotspot_ssid 仅剩品牌表展示字段。
    ui = _ui_block(license_data.get('ui_brand'))
    core_version = '2026.05.16'
    # 与 /api/state 同源：对外可见版本 = 系统部署版本（current 软链名），非 web 侧构建常量。
    app_version = _ota_current_version() or kAppVersion()
    return {
        'app_version': app_version,
        'auto_start': _auto_start_payload(),
        'core': _core_state_payload(),
        'device': {
            'binding_hardware': {
                'board_mac_addresses': macs,
                'cpu': {'Serial': cpu_serial},
                'schema': 'orangepi-board-v4',
            },
            'device_id': device_id,
            'fingerprint_hash': fingerprint,
            'hardware': {'Serial': cpu_serial},
        },
        'license': license_data,
        'ui': ui,
        'ui_brand': ui['ui_brand'],
        'version': app_version,
        # ★ M2.07：云端字段增量（§3.3）—— expire_at 北京时间串 / 解绑余量 /
        #   心跳在线状态 / machine_code。core 投影（上方 license 子块）零改动。
        'cloud': _cloud_license_subblock(),
    }
