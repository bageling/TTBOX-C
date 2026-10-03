# -*- coding: utf-8 -*-
"""hw_mouse —— 从 bin/ttbox-web.py 原样搬出（2026-10-02 web 换写法 S10 第一步）。

鼠标当前模式判定、鼠标配置下发载荷、显示器模式条目构造。
★ usbproxy 那一组是补丁锚点（_usbproxy_unit_mode ×4、
  _usbproxy_send_set_config ×3）⇒ 一律 hub 转发，替身才打得到。
★ `USB_PROXY_CONFIG_KEYS` 是**常量**（协议字段白名单），
  代码里迭代它 ⇒ hub.get 访问器 + 自动补括号。
搬出 3 个函数：_mouse_current_mode / _mouse_apply_payload / _display_mode_entry

★ 本模块在 lib/，**不能 import 入口** ⇒ 段外依赖一律经 hub 调用时取。
"""

from __future__ import annotations

import glob
import re
import subprocess

from plugins.web.lib import hub

def USB_PROXY_CONFIG_KEYS():
    """入口的 USB_PROXY_CONFIG_KEYS —— 调用时取（常量 / 单例，必须与入口**同一个对象**）。"""
    return hub.get('USB_PROXY_CONFIG_KEYS')


def _LOG():
    """入口的 _LOG —— 调用时取（常量 / 单例，必须与入口**同一个对象**）。"""
    return hub.get('_LOG')


def _loopout_payload():
    """入口的 _loopout_payload —— 调用时取（常量 / 单例，必须与入口**同一个对象**）。"""
    return hub.get('_loopout_payload')


def _usbproxy_effective_mode():
    """入口的 _usbproxy_effective_mode —— 调用时取（常量 / 单例，必须与入口**同一个对象**）。"""
    return hub.get('_usbproxy_effective_mode')


def _usbproxy_send_set_config():
    """入口的 _usbproxy_send_set_config —— 调用时取（常量 / 单例，必须与入口**同一个对象**）。"""
    return hub.get('_usbproxy_send_set_config')


def _mouse_current_mode(*args, **kwargs):
    """入口的 _mouse_current_mode —— 调用时取（monkeypatch 锚点，转发须**原样透传**）。"""
    return hub.call('_mouse_current_mode', *args, **kwargs)


def _usbproxy_config_for_form(*args, **kwargs):
    """入口的 _usbproxy_config_for_form —— 调用时取（monkeypatch 锚点，转发须**原样透传**）。"""
    return hub.call('_usbproxy_config_for_form', *args, **kwargs)


def _usbproxy_current_gadget_config(*args, **kwargs):
    """入口的 _usbproxy_current_gadget_config —— 调用时取（monkeypatch 锚点，转发须**原样透传**）。"""
    return hub.call('_usbproxy_current_gadget_config', *args, **kwargs)


def _usbproxy_gadget_config_path(*args, **kwargs):
    """入口的 _usbproxy_gadget_config_path —— 调用时取（monkeypatch 锚点，转发须**原样透传**）。"""
    return hub.call('_usbproxy_gadget_config_path', *args, **kwargs)


def _usbproxy_unit_mode(*args, **kwargs):
    """入口的 _usbproxy_unit_mode —— 调用时取（monkeypatch 锚点，转发须**原样透传**）。"""
    return hub.call('_usbproxy_unit_mode', *args, **kwargs)

def _mouse_current_mode(mouse: dict) -> str:
    """当前生效的透传模式：以 systemd 单元为准（web 改不了它），读不到才回落 profile。

    旧实现里有一句固定映射 "proxy/synthetic/local_hid -> full_passthrough"，
    等于无论实际什么模式都报透传；GET 又只取 profile（用户请求值）而非真值。
    前端单选值的 full 对应名是 full_passthrough，此处对齐。
    """
    unit = _usbproxy_unit_mode()
    if unit:
        return 'full_passthrough' if unit == 'full' else unit
    return str(mouse.get('mode') or mouse.get('proxy_mode') or '').strip() or 'full_passthrough'


def _mouse_apply_payload(mouse=None, applied=False, apply_error=''):
    """保持 Web 契约：PUT 后返回 applied/mode/service_*/timing 结构。

    applied 由真实下发结果决定，不再恒 True；apply_error 带失败人话原因；
    config 回填实际落盘的身份字段，让表单显示"已应用什么"而不是用户刚输入的。
    """
    import glob
    hidg = sorted(glob.glob('/dev/hidg*'))
    mouse = mouse or {}
    service_active = bool(hidg)
    try:
        out = subprocess.check_output(['systemctl', 'is-active', 'ttbox-usbproxy'],
                                      text=True, timeout=3).strip()
        service_active = out == 'active' or bool(hidg)
    except Exception:
        pass
    gadget_cfg = _usbproxy_current_gadget_config()
    return {
        'applied': bool(applied),
        'apply_error': apply_error,
        'gadget_config_path': _usbproxy_gadget_config_path(),
        'gadget_config': _usbproxy_config_for_form(gadget_cfg),
        'config': _usbproxy_config_for_form(gadget_cfg),
        'mode': _mouse_current_mode(mouse),
        'service_active': service_active,
        'service_active_text': 'active' if service_active else 'inactive',
        'service_enabled': service_active,
        'service_enabled_text': 'enabled' if service_active else 'disabled',
        'set_config_supported': True,
        'timing': {
            'identity_change_settle_delay_sec': float(mouse.get('identity_change_settle_delay_sec', 0.5)),
            'max_delay_sec': float(mouse.get('max_delay_sec', 30.0)),
            'mouse_settle_delay_sec': float(mouse.get('mouse_settle_delay_sec', 8.0)),
        },
    }


def _display_mode_entry(token, label, w, h, refresh, pc_khz):
    """保持 Web 契约 advertised/available modes 条目结构（含 source + hdmi_raw_gbps）。"""
    hdmi_raw_gbps = round(pc_khz * 30 / 1e6, 5)  # 保持 Web 契约：pc_khz*30/1e6
    return {
        'token': token,
        'label': label,
        'width': w, 'height': h, 'refresh': refresh,
        'pixel_clock_khz': pc_khz,
        'source': 'safe',
        'hdmi_raw_gbps': hdmi_raw_gbps,
    }
