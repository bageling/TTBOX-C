# -*- coding: utf-8 -*-
"""usbproxy 配置簇 —— 从 bin/ttbox-web.py 原样搬出（2026-10-02 web 换写法 S7）。

搬出的内容（入口 L2534-2669，逐行未改）：
    USB_PROXY_MAGIC / VERSION / REQ_SET_CONFIG / RESP_SET_CONFIG / RESP_ERROR
    USB_PROXY_CONFIG_KEYS / USB_PROXY_HEX_KEYS
    _usbproxy_gadget_config_path / _usbproxy_send_set_config
    _usbproxy_current_gadget_config / _usbproxy_config_for_form
    _usbproxy_unit_mode / _usbproxy_effective_mode

★ 偏离清单（共 3 处，均为「让补丁能穿透到 lib」的机械改写，无逻辑改动）：
    1. `import socket`→ 改为 _socket()，经 hub 取入口那个模块对象。
       原因：tests/test_web_status_honesty.py 有 1 处 `monkeypatch.setattr(web_mod,'socket',…)`，
       若 lib 自带 socket，替身打不进来（实测会打不进来）。
    2. `_run_quiet(...)` → `_run_quiet(...)` 转发函数（经 hub.call）。
       原因：_run_quiet 是补丁锚点，测试用 3 处 monkeypatch。
    3. `os` / `json` / `struct` / `ttbox_paths` 由本模块直接 import（它们不是补丁锚点）。

lib 侧不自建socket 连接逻辑，协议编码与回包校验全部保持原样。
"""

from __future__ import annotations

import json
import os
import struct

from plugins.web.lib import hub
from plugins.web.lib import paths as ttbox_paths


def _socket():
    """入口的 socket 模块（monkeypatch 锚点 ×1）—— 调用时取，替身才打得到。"""
    return hub.get('socket')


def _run_quiet(*args, **kwargs):
    """入口的 _run_quiet（monkeypatch 锚点 ×3）—— 调用时取。"""
    return hub.call('_run_quiet', *args, **kwargs)


USB_PROXY_MAGIC = 0x4F50
USB_PROXY_VERSION = 1
USB_PROXY_REQ_SET_CONFIG = 14
USB_PROXY_RESP_SET_CONFIG = 15
USB_PROXY_RESP_ERROR = 3
USB_PROXY_CONFIG_KEYS = (
    'usb_vid', 'usb_pid', 'usb_bcd_usb', 'usb_bcd_device', 'usb_device_class',
    'usb_device_subclass', 'usb_device_protocol', 'usb_max_power', 'hid_protocol',
    'hid_subclass', 'hid_report_length', 'hid_interval', 'usb_manufacturer',
    'usb_product', 'usb_serial', 'usb_configuration', 'hid_report_desc_hex',
)
USB_PROXY_HEX_KEYS = ('usb_vid', 'usb_pid', 'usb_bcd_usb', 'usb_bcd_device')


def _usbproxy_gadget_config_path() -> str:
    """usb-proxy 落盘的 gadget 配置文件（unit 的 WorkingDirectory=<release>/usbproxy/）。"""
    return os.path.join(ttbox_paths.repo_root(), 'usbproxy', 'gadget-config.json')


def _usbproxy_send_set_config(cfg: dict, apply_now: bool) -> tuple:
    """把 gadget 身份配置真正下发给 usb-proxy（cmd.sock，SOCK_SEQPACKET）。

    返回 (是否被 usb-proxy 确认, 失败原因人话)。连不上 / 无回包 / 协议不符 /
    被拒绝，一律算失败 —— 不再有"无条件 applied=True"这个分支。
    """
    def u16(value) -> bytes:
        return struct.pack('<H', int(value) & 0xFFFF)

    def u8(value) -> bytes:
        return struct.pack('<B', int(value) & 0xFF)

    def ustr(value) -> bytes:
        raw = str(value).encode('utf-8')
        return u16(len(raw)) + raw

    payload = b''.join([
        u16(cfg.get('usb_vid', 0)), u16(cfg.get('usb_pid', 0)),
        u16(cfg.get('usb_bcd_usb', 0x0200)), u16(cfg.get('usb_bcd_device', 0x0100)),
        u8(cfg.get('usb_device_class', 0)), u8(cfg.get('usb_device_subclass', 0)),
        u8(cfg.get('usb_device_protocol', 0)), u16(cfg.get('usb_max_power', 250)),
        u8(cfg.get('hid_protocol', 2)), u8(cfg.get('hid_subclass', 1)),
        u8(cfg.get('hid_report_length', 4)), u8(cfg.get('hid_interval', 1)),
        ustr(cfg.get('usb_manufacturer', '')), ustr(cfg.get('usb_product', '')),
        ustr(cfg.get('usb_serial', '')), ustr(cfg.get('usb_configuration', '')),
        ustr(cfg.get('hid_report_desc_hex', '')),
    ])
    sock_path = ttbox_paths.MOUSE_CMD_SOCK_DEFAULT
    try:
        conn = _socket().socket(_socket().AF_UNIX, _socket().SOCK_SEQPACKET)
        conn.settimeout(5.0)
        conn.connect(sock_path)
    except Exception as exc:
        return False, f'usb-proxy 未运行（{sock_path} 连不上：{exc}）'
    try:
        conn.sendall(struct.pack('<HBB', USB_PROXY_MAGIC, USB_PROXY_VERSION,
                                 USB_PROXY_REQ_SET_CONFIG) + struct.pack('<I', 1) +
                     u8(1 if apply_now else 0) + payload)
        reply = conn.recv(512)
    except Exception as exc:
        return False, f'usb-proxy 无回包：{exc}'
    finally:
        try:
            conn.close()
        except Exception:
            pass
    if len(reply) < 8:
        return False, f'usb-proxy 回包过短（{len(reply)} 字节）'
    magic, version, rtype = struct.unpack_from('<HBB', reply, 0)
    if magic != USB_PROXY_MAGIC or version != USB_PROXY_VERSION:
        return False, f'usb-proxy 回包协议不符（magic=0x{magic:04x} version={version}）'
    if rtype == USB_PROXY_RESP_ERROR:
        detail = reply[8:].decode('utf-8', 'replace').strip('\x00').strip()
        return False, f'usb-proxy 拒绝该配置：{detail or "未给出原因"}'
    if rtype != USB_PROXY_RESP_SET_CONFIG:
        return False, f'usb-proxy 回包类型异常（type={rtype}）'
    return True, ''


def _usbproxy_current_gadget_config() -> dict:
    """读 usb-proxy 实际落盘的 gadget 配置；读不到返回空 dict（不编造）。"""
    try:
        with open(_usbproxy_gadget_config_path(), encoding='utf-8') as fh:
            data = json.load(fh)
    except Exception:
        return {}
    return data if isinstance(data, dict) else {}


def _usbproxy_config_for_form(cfg: dict) -> dict:
    """把落盘配置转成前端表单格式（vid/pid/bcd 用 0xXXXX 字符串，与 GET 一致）。"""
    out = {}
    for key in USB_PROXY_CONFIG_KEYS:
        if key not in cfg:
            continue
        value = cfg[key]
        if key in USB_PROXY_HEX_KEYS and isinstance(value, int):
            value = f'0x{value:04X}'
        out[key] = value
    return out


def _usbproxy_unit_mode() -> str:
    """usb-proxy 请求的透传模式 —— 读 systemd 单元的 Environment=USB_PROXY_MODE。

    1.5.62 起单元里已不再设置这一项（合成模式删除，只有物理透传一种模式）；
    这里仍保留读取，只是为了让历史 drop-in（10-mode.conf）残留时不被误判。
    """
    out = _run_quiet(['systemctl', 'show', '-p', 'Environment', 'ttbox-usbproxy'])
    for token in out.replace('"', ' ').split():
        if token.startswith('USB_PROXY_MODE='):
            return token.split('=', 1)[1].strip()
    return ''


def _usbproxy_effective_mode() -> str:
    """usb-proxy 进程**实际**跑的模式（1.5.62 起只有物理透传一种）。

    1.5.62 删除了合成模式：命令行带 --vendor_id 才是真在透传物理鼠标。
    没找到物理鼠标时进程不会起来（启动脚本一直等），读不到就返回 ''（不编造）。
    """
    try:
        for pid in os.listdir('/proc'):
            if not pid.isdigit():
                continue
            try:
                with open(f'/proc/{pid}/cmdline', 'rb') as fh:
                    cmd = fh.read().decode('utf-8', 'replace')
            except Exception:
                continue
            if 'usb-proxy' not in cmd:
                continue
            if '--vendor_id' in cmd:
                return 'full_passthrough'
    except Exception:
        pass
    return ''
