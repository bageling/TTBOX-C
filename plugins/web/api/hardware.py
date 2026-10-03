# -*- coding: utf-8 -*-
"""api/hardware.py —— 硬件域 Blueprint。

从 bin/ttbox-web.py 搬出（2026-10-02 web 换写法 S9-b）。
搬出 5 条路由，**URL 一字未改**。

鼠标/显示器的读与写。
★ **本域是唯一还需要 S9-c 单独处理的**：`get_display_hardware` 152 行、
  `get_mouse_hardware` 137 行，两者都是「读硬件 + 校验 + 拼装」三件事混一起。
  本刀**只搬不改**，S9-c 再动内部结构（拆 probe/validate）。
★ `_mouse_save_or_ipc`（5 处）/ `_loopout_payload`（3 处）/
  `_usbproxy_send_set_config`（3 处）是补丁锚点，都经 hub。

★ 非路由装饰器 `_config_write_serialized` 从 `lib.locks` **直接 import**：
  装饰器在被装饰函数的 def 行执行（import 期）就被应用，
  那时入口的 hub.bind() 还没跑 ⇒ 走 hub 转发必然 RuntimeError。
段外依赖处理：
    · _mouse_save_or_ipc —— 经 hub.get 调用时取。
    · _get_runtime_profile / _loopout_payload / _usbproxy_config_for_form / _usbproxy_current_gadget_config / _usbproxy_effective_mode / _usbproxy_gadget_config_path / _usbproxy_send_set_config —— 经 hub.call 调用时取。
    · json / os / re / subprocess / time —— 标准库，直接 import。
    · jsonify / request —— flask，直接 import。
    · TTBOX_HDMIRX_EDID(from settings), USB_PROXY_CONFIG_KEYS(from usbproxy_cfg) —— lib里已有，直接 import。

★ **禁止在 except 子句里调 hub**（求值时机是函数定义时，此时 hub.bind 还没执行 ⇒ 永久绑成 None ⇒ 该 except 永不匹配）。
  需要按异常类型分流时，抽 `_is_xxx(exc)` 函数在调用时才取类。
★ 非路由装饰器 `_config_write_serialized` 从 lib.locks **直接 import**（不走 hub）：
  装饰器在 def 行执行（import 期）就被应用，那时 hub.bind 还没跑。
"""

from __future__ import annotations

import glob
import json
import os
import re
import subprocess
import time

from flask import Blueprint, app, jsonify, request

from plugins.web.lib import hub
from plugins.web.lib.settings import TTBOX_HDMIRX_EDID
from plugins.web.lib.usbproxy_cfg import USB_PROXY_CONFIG_KEYS
from plugins.web.lib.locks import config_write_serialized as _config_write_serialized
from plugins.web.lib import paths as ttbox_paths
from flask import Blueprint, app, jsonify, request

bp = Blueprint('hardware', __name__)

def _mouse_save_or_ipc(*args, **kwargs):
    """入口的 _mouse_save_or_ipc —— 调用时取（monkeypatch 锚点，转发须**原样透传**：
    测试替身常是少参数的 lambda，补默认值会多塞实参而 TypeError）。"""
    return hub.call('_mouse_save_or_ipc', *args, **kwargs)


def _DISPLAY_CACHE():
    """入口的 _DISPLAY_CACHE —— 调用时取（单例 / 共享状态，身份须唯一）。"""
    return hub.get('_DISPLAY_CACHE')


def _activation_ok(*args, **kwargs):
    """入口的 _activation_ok —— 调用时取。"""
    return hub.call('_activation_ok', *args, **kwargs)


def _display_mode_entry(*args, **kwargs):
    """入口的 _display_mode_entry —— 调用时取。"""
    return hub.call('_display_mode_entry', *args, **kwargs)


def _mouse_apply_payload(*args, **kwargs):
    """入口的 _mouse_apply_payload —— 调用时取。"""
    return hub.call('_mouse_apply_payload', *args, **kwargs)


def _mouse_current_mode(*args, **kwargs):
    """入口的 _mouse_current_mode —— 调用时取。"""
    return hub.call('_mouse_current_mode', *args, **kwargs)


def _get_runtime_profile(*args, **kwargs):
    """入口的 _get_runtime_profile —— 调用时取（monkeypatch 锚点）。"""
    return hub.call('_get_runtime_profile', *args, **kwargs)


def _loopout_payload(*args, **kwargs):
    """入口的 _loopout_payload —— 调用时取（monkeypatch 锚点）。"""
    return hub.call('_loopout_payload', *args, **kwargs)


def _usbproxy_config_for_form(*args, **kwargs):
    """入口的 _usbproxy_config_for_form —— 调用时取（monkeypatch 锚点）。"""
    return hub.call('_usbproxy_config_for_form', *args, **kwargs)


def _usbproxy_current_gadget_config(*args, **kwargs):
    """入口的 _usbproxy_current_gadget_config —— 调用时取（monkeypatch 锚点）。"""
    return hub.call('_usbproxy_current_gadget_config', *args, **kwargs)


def _usbproxy_effective_mode(*args, **kwargs):
    """入口的 _usbproxy_effective_mode —— 调用时取（monkeypatch 锚点）。"""
    return hub.call('_usbproxy_effective_mode', *args, **kwargs)


def _usbproxy_gadget_config_path(*args, **kwargs):
    """入口的 _usbproxy_gadget_config_path —— 调用时取（monkeypatch 锚点）。"""
    return hub.call('_usbproxy_gadget_config_path', *args, **kwargs)


def _usbproxy_send_set_config(*args, **kwargs):
    """入口的 _usbproxy_send_set_config —— 调用时取（monkeypatch 锚点）。"""
    return hub.call('_usbproxy_send_set_config', *args, **kwargs)

def _default_usb_cfg():
    """USB 描述符默认值（未探测到真实鼠标时用）。"""
    return {
        'hid_interval': 1, 'hid_protocol': 2, 'hid_report_desc_hex': '',
        'hid_report_length': 64, 'hid_subclass': 1,
        'usb_bcd_device': '0x0100', 'usb_bcd_usb': '0x0200',
        'usb_configuration': '', 'usb_device_class': 0,
        'usb_device_protocol': 0, 'usb_device_subclass': 0,
        'usb_manufacturer': '', 'usb_max_power': 100,
        'usb_pid': '0x0000', 'usb_product': '', 'usb_serial': '',
        'usb_vid': '0x0000',
    }


def _probe_usb_mouse():
    """扫 sysfs 找真实 USB 鼠标（bInterfaceClass=03 ⇒ HID）。

    返回 (usb_cfg, physical, connected)：
      · 接口子目录（如 3-1:1.1）没有 vid/pid，要向上找到父设备（3-1）
      · 任一步失败都不抛 —— 返回默认值 + connected=False，
        面板据此显示「未接入」，不能因读不到 sysfs 就 500

    ★ 这是本函数里最大的一块（70 行纯 I/O），拆出来后能单独测：
      注入假的 glob / os 就能验解析逻辑，不用真的插鼠标。
    """
    usb_cfg = _default_usb_cfg()
    physical = {'device': '', 'interface': '', 'name': ''}
    connected = False
    try:
        for dev in sorted(glob.glob('/sys/bus/usb/devices/*')):
            bdev = os.path.basename(dev)
            if bdev.count('-') < 1 or not os.path.isdir(dev):
                continue
            iface = os.path.join(dev, 'bInterfaceClass')
            if not os.path.exists(iface):
                continue
            try:
                cls = open(iface).read().strip()
            except Exception:
                continue
            if cls == '03':  # HID 接口
                # 去掉接口后缀，向上找父 USB 设备
                parent = dev
                while parent.count(':') > 0:
                    parent = parent.rsplit(':', 1)[0]

                def _rd(p, _parent=parent):
                    try:
                        return open(os.path.join(_parent, p)).read().strip()
                    except Exception:
                        return ''

                vid = _rd('idVendor')
                pid = _rd('idProduct')
                mfr = _rd('manufacturer')
                prod = _rd('product')
                cfg_name = _rd('configuration')
                bcd_dev = _rd('bcdDevice')
                bcd_usb = _rd('version')
                dev_cls = _rd('bDeviceClass')
                dev_sub = _rd('bDeviceSubClass')
                dev_proto = _rd('bDeviceProtocol')
                max_power = _rd('bMaxPower')  # 形如 "98mA"
                usb_cfg['usb_vid'] = '0x' + vid if vid else usb_cfg['usb_vid']
                usb_cfg['usb_pid'] = '0x' + pid if pid else usb_cfg['usb_pid']
                usb_cfg['usb_manufacturer'] = mfr
                usb_cfg['usb_product'] = prod
                usb_cfg['usb_configuration'] = cfg_name
                usb_cfg['usb_serial'] = _rd('serial')
                if bcd_dev:
                    usb_cfg['usb_bcd_device'] = '0x' + bcd_dev
                if bcd_usb:
                    usb_cfg['usb_bcd_usb'] = '0x' + bcd_usb.replace('.', '')
                if dev_cls:
                    usb_cfg['usb_device_class'] = int(dev_cls, 16)
                if dev_sub:
                    usb_cfg['usb_device_subclass'] = int(dev_sub, 16)
                if dev_proto:
                    usb_cfg['usb_device_protocol'] = int(dev_proto, 16)
                if max_power.endswith('mA'):
                    usb_cfg['usb_max_power'] = int(max_power[:-2])
                # 鼠标接口(1.x) 真实协议与端点 interval
                try:
                    # ★ 原代码这里写的是 `if _rd('bInterfaceProtocol')
                    #   if False else True:` —— `if False` 恒假 ⇒ 整个条件恒为
                    #   True ⇒ 那次 _rd() 调用从来没执行过（死代码，2026-10-02 清）。
                    iface_proto = open(os.path.join(
                        dev, 'bInterfaceProtocol')).read().strip()
                    iface_sub = open(os.path.join(
                        dev, 'bInterfaceSubClass')).read().strip()
                    if iface_proto:
                        usb_cfg['hid_protocol'] = int(iface_proto)
                    if iface_sub:
                        usb_cfg['hid_subclass'] = int(iface_sub)
                    eps = sorted(glob.glob(os.path.join(dev, 'ep_*')))
                    for ep in eps:
                        iv = open(os.path.join(ep, 'bInterval')).read().strip()
                        if iv:
                            usb_cfg['hid_interval'] = int(iv)
                            break
                except Exception:
                    pass
                physical = {'device': os.path.basename(parent),
                            'interface': bdev, 'name': prod or mfr}
                connected = True
                break
    except Exception:
        pass
    return usb_cfg, physical, connected


def _probe_usbproxy_service(hidg_present):
    """usbproxy 服务是否在跑。

    语义（保持 Web 契约）：service_active 指**服务在跑**，不是「设备插着」。
    读不到 systemctl（超时/无权限）时退回「有 hidg 节点就算 active」。
    """
    try:
        out = subprocess.check_output(
            ['systemctl', 'is-active', 'ttbox-usbproxy'],
            text=True, timeout=3).strip()
        return out == 'active' or bool(hidg_present)
    except Exception:
        return bool(hidg_present)


@bp.get('/api/hardware/mouse')
def get_mouse_hardware():
    """保持 Web 契约：11 字段结构（config/config_source/connected/mode/
    physical_mouse/service_active/service_active_text/service_enabled/
    service_enabled_text/set_config_supported/timing）。

    ★ 拆成 probe 的收益：USB 解析（70 行）与服务状态探测能各自单独测。
    """
    hidg = sorted(glob.glob('/dev/hidg*'))
    prof = _get_runtime_profile()
    mouse = prof.get('mouse') or {}
    usb_cfg, physical, connected = _probe_usb_mouse()
    service_active = _probe_usbproxy_service(bool(hidg))
    service_enabled = bool(mouse.get('enabled', False)) or service_active
    requested_mode = _mouse_current_mode(mouse)
    effective_mode = _usbproxy_effective_mode() or requested_mode
    return jsonify({
        'ok': True,
        'data': {
            'config': usb_cfg,
            'config_source': 'sysfs_usb_mouse' if connected else 'default',
            'connected': connected,
            'mode': requested_mode,
            # effective_mode 是进程实际在跑的模式（读不到进程 = 没在透传）。
            # mode_degraded：1.5.62 删除合成模式后恒为 False；保留键以免老面板取不到。
            'effective_mode': effective_mode,
            'mode_degraded': False,
            'physical_mouse': physical,
            'service_active': service_active,
            'service_active_text': 'active' if service_active else 'inactive',
            'service_enabled': service_enabled,
            'service_enabled_text': 'enabled' if service_enabled else 'disabled',
            'set_config_supported': True,
            'gadget_config_path': _usbproxy_gadget_config_path(),
            'gadget_config': _usbproxy_config_for_form(
                _usbproxy_current_gadget_config()),
            'timing': {
                'identity_change_settle_delay_sec': float(
                    mouse.get('identity_change_settle_delay_sec', 0.5)),
                'max_delay_sec': float(mouse.get('max_delay_sec', 30.0)),
                'mouse_settle_delay_sec': float(
                    mouse.get('mouse_settle_delay_sec', 8.0)),
            },
        },
    })


@bp.put('/api/hardware/mouse')
@_config_write_serialized
def update_mouse_hardware():
    body = request.get_json(silent=True) or {}
    prof = _get_runtime_profile()
    mouse = prof.get('mouse') or {}
    # 前端字段：enabled/mode/config
    if 'mode' in body:
        mouse['mode'] = body['mode']
        mouse['proxy_mode'] = body['mode']
    if 'enabled' in body:
        # ★ 强转 bool：字符串 "false" 是真值，原样写会把"关闭"存成"开启"。
        mouse['enabled'] = bool(body['enabled']) if isinstance(body['enabled'], bool) \
            else str(body['enabled']).strip().lower() in ('1', 'true', 'yes', 'on')
    if 'config' in body and isinstance(body['config'], dict):
        mouse.update({k: v for k, v in body['config'].items() if k in (
            'usb_vid', 'usb_pid', 'usb_manufacturer', 'usb_product', 'usb_serial',
            'usb_max_power', 'hid_report_length', 'hid_interval', 'hid_protocol',
            'hid_subclass', 'usb_bcd_device', 'usb_bcd_usb', 'usb_configuration',
            'usb_device_class', 'usb_device_subclass', 'usb_device_protocol',
        )})
    prof['mouse'] = mouse
    ok, detail = _mouse_save_or_ipc(prof, mouse)
    # 身份配置的真源是 usb-proxy 的 gadget-config.json，不是 Core 的 RuntimeProfile
    # （Core 没有 usb_*/hid_* 成员，先前发过去等于丢掉）。这里走 cmd.sock 真下发。
    gadget_cfg = {k: v for k, v in mouse.items() if k in USB_PROXY_CONFIG_KEYS}
    if gadget_cfg:
        applied, apply_error = _usbproxy_send_set_config(gadget_cfg, bool(body.get('apply_now')))
    else:
        applied, apply_error = False, '未提供任何 USB 身份字段，未下发配置'
    payload = _mouse_apply_payload(mouse, applied=applied, apply_error=apply_error)
    if detail:
        payload['_core_offline'] = True
        payload['_detail'] = detail
    # ok 取两个通道的合取：任一路没落实就不能报成功。
    both_ok = bool(ok and applied)
    return jsonify({'ok': both_ok, 'data': payload,
                    'error': '' if both_ok else (apply_error or detail)})


@bp.put('/api/hardware/mouse/mode')
def update_mouse_proxy_mode():
    body = request.get_json(silent=True) or {}
    mode = str(body.get('mode') or '').strip()
    if not mode:
        return jsonify({'ok': False, 'error': 'mode is required'})
    prof = _get_runtime_profile()
    mouse = prof.get('mouse') or {}
    mouse['mode'] = mode
    mouse['proxy_mode'] = mode
    prof['mouse'] = mouse
    ok, detail = _mouse_save_or_ipc(prof, mouse)
    # 透传模式的真源是 systemd 单元的 Environment=USB_PROXY_MODE，改它要 root +
    # daemon-reload，web（ttbox 身份）做不到。旧实现把这个写进 profile 就报成功，
    # 而单元里的模式从未变过 —— 典型的假落实。如实报失败，并告知当前真实模式。
    mode_error = ('切换 USB 透传模式需修改 systemd 单元的 USB_PROXY_MODE 并以 root '
                  '重载服务，Web 无此权限；请在板端运维通道执行')
    payload = _mouse_apply_payload(mouse, applied=False, apply_error=mode_error)
    if detail:
        payload['_core_offline'] = True
        payload['_detail'] = detail
    return jsonify({'ok': False, 'error': mode_error, 'data': payload})


def _probe_hdmi_timing():
    """读 v4l2 当前时序（connected / locked / 宽高 / 刷新率）。

    ★ 超时 1.5 s 是关键：信号重协商时 v4l2-ctl 会阻塞，
      64 个 waitress 线程各卡 1.5 s 会把整个服务打瘫。
    """
    hdmi = {'connected': False, 'locked': False, 'width': 0, 'height': 0,
            'refresh': 0}
    try:
        r = subprocess.run(
            ['v4l2-ctl', '-d', '/dev/video0', '--query-dv-timing'],
            capture_output=True, text=True, timeout=1.5,
        )
        txt = r.stdout
        if r.returncode == 0 and 'Active width' in txt:
            w = re.search(r'Active width:\s*(\d+)', txt)
            h = re.search(r'Active height:\s*(\d+)', txt)
            fps = re.search(r'\(([\d.]+) frames per second\)', txt)
            hdmi['connected'] = True
            hdmi['locked'] = True
            if w:
                hdmi['width'] = int(w.group(1))
            if h:
                hdmi['height'] = int(h.group(1))
            if fps:
                hdmi['refresh'] = float(fps.group(1))
    except Exception:
        pass
    return hdmi


def _probe_display_config():
    """读板端保存的显示器配置（Web 兼容结构：前端 populateDisplayHardware 消费）。"""
    cfg_disp = {}
    cpath = ttbox_paths.config_dir() + '/hardware_display.json'
    try:
        if os.path.exists(cpath):
            cfg_disp = json.load(open(cpath))
    except Exception:
        pass
    return cfg_disp


def _probe_edid_modes():
    """读 EDID 模式表（Modes: 段直到第一个空行为止）。

    ★ 这段原本是**两份几乎逐行相同的代码**（advertised / available_modes 各一份，
      20+ 行重复），差别只有空行处一个用 `break`、一个用 `in_modes = False` ——
      **两者行为等价**（都是「遇空行结束模式区」），所以合并成一个函数。
      合并前实测确认：同一份 EDID 文本，两份原代码输出完全一致。
    """
    modes = []
    try:
        out = subprocess.check_output(
            [TTBOX_HDMIRX_EDID, '--list'],
            text=True, timeout=5)
        in_modes = False
        for lm in out.splitlines():
            lm = lm.strip()
            if lm.startswith('Modes:'):
                in_modes = True
                continue
            if in_modes:
                if not lm:
                    break            # 空行 = 模式区结束（原两份代码一致）
                parts = lm.split()
                if not parts:
                    continue
                token = parts[0]
                dims = re.search(r'(\d+)x(\d+)@(\d+)', lm)
                pc = re.search(r'pixel_clock=(\d+)', lm)
                modes.append(_display_mode_entry(
                    token,
                    f'{dims.group(1)}x{dims.group(2)}@{dims.group(3)}'
                    if dims else token,
                    int(dims.group(1)) if dims else 0,
                    int(dims.group(2)) if dims else 0,
                    int(dims.group(3)) if dims else 0,
                    int(pc.group(1)) if pc else 0,
                ))
    except Exception:
        pass
    return modes


def _probe_edid_status():
    """读当前生效 EDID 的显示器身份（名称/厂商/产品号/序列号）。"""
    name = vendor = pid = serial = ''
    valid = False
    status_text = ''
    try:
        out = subprocess.check_output(
            [TTBOX_HDMIRX_EDID, '--status'],
            text=True, timeout=5)
        status_text = out
        nm = re.search(r'name=(\S+)', out)
        vd = re.search(r'vendor=(\S+)', out)
        p = re.search(r'product=(0x[0-9a-fA-F]+)', out)
        ser = re.search(r'serial=(0x[0-9a-fA-F]+)', out)
        if nm:
            name = nm.group(1)
            vendor = vd.group(1) if vd else ''
            pid = p.group(1) if p else ''
            serial = ser.group(1) if ser else ''
            valid = True
    except Exception:
        pass
    return {'name': name, 'vendor': vendor, 'product_id': pid,
            'serial': serial, 'valid': valid, 'raw': status_text}


def _probe_hdmirx_monitor(data):
    """读 hdmirx RX 状态并回填 data（真实 RX 独立于板端其它服务）。"""
    # V-07 收口：scripts 目录已在文件头经 ttbox_paths.scripts_dir() append 进 sys.path
    # （A-PATH-3 相对派生）；此处**不再**以绝对路径 insert(0, …) 注入 ——
    # 绝对路径注入既散落字面量（A-PATH-3/5违背），又有 insert(0) 遮蔽 stdlib 的风险。
    try:
        from edid.monitor import read_hdmirx_status
        rx = read_hdmirx_status()
        data['hdmirx'] = rx
        if not data['available'] and rx.get('connected'):
            data['available'] = True
    except Exception:
        pass
    return data


@bp.get('/api/hardware/display')
def get_display_hardware():
    """显示器硬件状态汇总 = 5 个 probe + 1 次拼装。

    ★ 缓存 3 秒：v4l2-ctl query-dv-timing 在信号重协商时阻塞，
      防止 waitress 线程耗尽。
    ★ 拆成 probe 的收益：每段能单独测（原来 151 行里改一行就要跑全量）。
    """
    now = time.time()
    if _DISPLAY_CACHE()['data'] is not None and now - _DISPLAY_CACHE()['ts'] < 3:
        return jsonify({'ok': True, 'data': _DISPLAY_CACHE()['data']})

    hdmi = _probe_hdmi_timing()
    cfg_disp = _probe_display_config()
    # 广播模式（当前 EDID 生效的模式，同 available_modes 结构）
    advertised = _probe_edid_modes()
    # available_modes（Web 结构：token/label/width/height/refresh/pixel_clock_khz）
    available_modes = _probe_edid_modes()

    data = dict(hdmi)
    data['available'] = hdmi.get('connected', False)
    data['config'] = cfg_disp
    data = _probe_hdmirx_monitor(data)

    edid = _probe_edid_status()
    data['status'] = {'output': edid['raw'] or 'EDID 状态读取失败'}
    data['loopout'] = _loopout_payload()
    data['display_mode'] = {
        'loopout_enabled': bool(cfg_disp.get('loopout_enabled', False)),
        'real_monitor': {
            'connected': hdmi.get('connected', False),
            'width': hdmi.get('width', 0), 'height': hdmi.get('height', 0),
            'refresh': hdmi.get('refresh', 0),
            'name': edid['name'] or cfg_disp.get('name', ''),
            'vendor': edid['vendor'] or cfg_disp.get('vendor', ''),
            'product_id': edid['product_id'] or cfg_disp.get('product_id', ''),
            'serial': edid['serial'] or cfg_disp.get('serial', ''),
            'edid_valid': edid['valid'],
        },
        'advertised_modes': advertised[:16],
        'available_modes': available_modes,
    }
    _DISPLAY_CACHE()['ts'] = time.time()
    _DISPLAY_CACHE()['data'] = data
    return jsonify({'ok': True, 'data': data})


@bp.put('/api/hardware/display')
def update_display_hardware():
    body = request.get_json(silent=True) or {}
    cfg_in = body.get('config') or {}
    apply_now = bool(body.get('apply'))
    if not cfg_in:
        return jsonify({'ok': False, 'error': '缺少 config'})
    cpath = ttbox_paths.config_dir() + '/hardware_display.json'
    try:
        cur = json.load(open(cpath)) if os.path.exists(cpath) else {}
    except Exception:
        cur = {}
    # 只合并白名单键（防注入）
    for k in ('device', 'name', 'vendor', 'product_id', 'serial',
              'native_mode', 'native_only', 'profile',
              'loopout_enabled', 'loopout_overlay_enabled',
              'loopout_pixel_format', 'loopout_overlay_thickness', 'loopout_overlay_color'):
        if k in cfg_in:
            # native_mode 保护：空/非法值不覆盖已有配置（防退化成 1080p60）
            if k == 'native_mode':
                v = str(cfg_in[k] or '').strip()
                if v and v != 'auto':
                    try:
                        # edid 包随 scripts 目录在标题头已 append 进 sys.path（A-PATH-3）
                        from edid.timing_db import mode_info
                        if mode_info(v) is None:
                            continue  # 非法 token，拒绝覆盖
                    except Exception:
                        continue
                else:
                    continue  # 空/auto 不覆盖
            cur[k] = cfg_in[k]
    # hardware_display.device 表示 HDMI-RX 输入设备，不能接受 DRM 输出节点。
    # auto/空值统一落为真实 V4L2 节点，避免界面显示与 EDID 实际注入路径漂移。
    requested_device = str(cur.get('device', 'auto') or 'auto').strip()
    if requested_device == 'auto' or not requested_device:
        cur['device'] = '/dev/video0'
    elif requested_device.startswith('/dev/dri/') or 'card' in requested_device or 'renderD' in requested_device:
        return jsonify({'ok': False, 'error': 'HDMI-RX 输入必须使用 /dev/video0，/dev/dri/card0 仅用于 loopout'}), 400
    json.dump(cur, open(cpath, 'w'), indent=2, ensure_ascii=False)

    result = {}
    if apply_now:
        apply_env = dict(os.environ)
        apply_env['TTBOX_EDID_REHANDSHAKE'] = '1'
        # V-09：不再覆写 TTBOX_EDID_REHANDSHAKE_ATTEMPTS —— 重试次数单一真源在
        # edid_apply.sh（★ 2026-09-28 起默认 2，此前 12；每次重试都要切一次 HPD = 源端黑屏一次），
        # Web 与手动路径必须同值（原 Web 私自设 6 ⇒ 行为不可预期）。
        r = subprocess.run(['bash', ttbox_paths.scripts_dir() + '/edid/edid_apply.sh'],
                           capture_output=True, text=True, timeout=60, env=apply_env)
        result = {'exit': r.returncode, 'output': (r.stdout + r.stderr).strip()[-500:]}
        if r.returncode != 0:
            return jsonify({'ok': False, 'error': f'EDID 应用失败: {r.stderr or r.stdout}'[-300:]})
        # 内核持久化（前端 patch_boot_image=true 时执行）
        if body.get('patch_boot_image'):
            pr = subprocess.run(
                ['bash', ttbox_paths.scripts_dir() + '/edid/edid_patch_boot_image.sh',
                 ttbox_paths.ttbox_prefix() + '/runtime/edid/current.bin'],
                capture_output=True, text=True, timeout=60)
            result['patch_boot_image'] = {
                'exit': pr.returncode,
                'output': (pr.stdout + pr.stderr).strip()[-300:],
            }
    # 返回 Web 兼容结构（前端 populateDisplayHardware 消费 display_mode/loopout）
    # 简化：直接返回 GET 的完整结构（含 real_monitor/available_modes/advertised）
    with app.test_request_context('/api/hardware/display'):
        pass
    gv = get_display_hardware()
    gv_data = gv.get_json().get('data', {})
    gv_data['config'] = cur
    gv_data['result'] = result
    gv_data['message'] = '显示器配置已应用'
    gv_data['loopout'] = {
        'enabled': bool(cur.get('loopout_enabled')),
        'overlay_enabled': bool(cur.get('loopout_overlay_enabled')),
        'pixel_format': cur.get('loopout_pixel_format', 'rgb888'),
        'width': 0, 'height': 0, 'refresh': 0,
        'overlay_status': '等待环出' if cur.get('loopout_overlay_enabled') else '',
    }
    return jsonify({'ok': True, 'data': gv_data})

