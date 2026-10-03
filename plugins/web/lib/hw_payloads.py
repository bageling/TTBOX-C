# -*- coding: utf-8 -*-
"""硬件载荷簇 —— 从 bin/ttbox-web.py 原样搬出（2026-10-02 web 换写法 S7 第二刀）。

搬出的内容（入口 L1087-1211，逐行未改）：
    _auto_start_enabled / _auto_start_payload   —— 开机自启状态投影
    _fan_enabled                —— PWM 是否在转（可测的纯判定）
    _fan_control_payload        —— 风扇 PWM + NPU 温度（真实硬件读取）
    _loopout_payload            —— DRM connector 状态（真实读取）

★ 偏离清单：**0 处**。段内只用到 os / time / subprocess，均非补丁锚点，直接 import。
   `import glob as _glob` 是原代码里的函数内 import（刻意为之，保留原样）。

lib 侧不持有任何 Flask 对象，可脱离 app 直接跑（可测性比 usbproxy_cfg 更好）。
"""

from __future__ import annotations

import os
import subprocess
import time

def _auto_start_enabled() -> bool:
    try:
        out = subprocess.check_output(['systemctl', 'is-enabled', 'ttbox-core'],
                                      text=True, timeout=3).strip()
        return out == 'enabled'
    except Exception:
        return False


def _auto_start_payload() -> dict:
    """保持 Web 契约：enabled=false -> status=disabled/message=''；
    enabled=true -> status=next_boot/message='将在下次开机时自动启动'。"""
    enabled = _auto_start_enabled()
    if not enabled:
        return {'enabled': False, 'status': 'disabled', 'message': '', 'updated_at': 0}
    return {'enabled': True, 'status': 'next_boot',
            'message': '将在下次开机时自动启动', 'updated_at': int(time.time())}


def _fan_enabled(pwm_path: str, pwm_raw: int) -> bool:
    """风扇是否在转：有 PWM 节点且占空比 > 0。

    单拎出来是为了可测 —— Core 启动时就把 pwm 写成 255（满转），
    这里若恒返 False，面板显示"未启用"而风扇实际在转，是反向的假信息。
    """
    return bool(pwm_path) and pwm_raw > 0


def _fan_control_payload() -> dict:
    """保持 Web 契约 fan_control 结构（真实硬件读取）。"""
    import glob as _glob
    # 找 NPU 温控 PWM 节点（hwmon8/pwm1 是历史固定节点，TTBOX 动态探测）
    pwm_path = ''
    try:
        for p in _glob.glob('/sys/class/hwmon/hwmon*/pwm1'):
            name = open(os.path.join(os.path.dirname(p), 'name')).read().strip()
            if name in ('pwm-fan', 'pwmfan', 'fan', 'soc-thermal'):
                pwm_path = p
                break
        if not pwm_path and _glob.glob('/sys/class/hwmon/hwmon*/pwm1'):
            pwm_path = sorted(_glob.glob('/sys/class/hwmon/hwmon*/pwm1'))[0]
    except Exception:
        pass
    pwm_raw = 0
    pwm_percent = 0
    pwm_writable = False
    if pwm_path:
        try:
            pwm_raw = int(open(pwm_path).read().strip())
            pwm_percent = int(round(pwm_raw * 100 / 255))
        except Exception:
            pass
        pwm_writable = os.access(pwm_path, os.W_OK)
    # NPU 温度（devfreq 或 thermal zone）
    temp_c = 0.0
    try:
        for z in _glob.glob('/sys/class/thermal/thermal_zone*/type'):
            if 'soc' in open(z).read().lower() or 'npu' in open(z).read().lower():
                temp_c = int(open(os.path.join(os.path.dirname(z), 'temp')).read().strip()) / 1000.0
                break
    except Exception:
        pass
    return {
        'control_available': bool(pwm_path),
        # enabled 必须反映硬件真实状态：Core 启动时会把 pwm 写成满转
        # （core/src/app/Application.cpp 的"风扇满转"段，pwm << 255），
        # 此处若硬编码 False，面板会显示"未启用"而风扇实际在转 —— 与物理事实相反。
        'enabled': _fan_enabled(pwm_path, pwm_raw),
        'fan_rpm': 0,
        'last_error': '' if (not pwm_path or pwm_writable) else f'{pwm_path} 不可写（权限或只读挂载）',
        'pwm_path': pwm_path,
        'pwm_percent': pwm_percent,
        'pwm_raw': pwm_raw,
        'pwm_writable': pwm_writable,
        'source': 'npu',
        'source_label': 'NPU',
        'tachometer_available': False,
        'temperature_celsius': temp_c,
        'updated_at_ms': int(time.time() * 1000),
    }


def _loopout_payload() -> dict:
    """保持 Web 契约 loopout 结构（读真实 DRM connector 状态）。"""
    import glob as _glob
    connected = False
    connector_id = 0
    for base in sorted(_glob.glob('/sys/class/drm/card*-HDMI-*')):
        try:
            if open(os.path.join(base, 'status')).read().strip() == 'connected':
                connected = True
                connector_id = int(os.path.basename(base).split('-')[-1]) if os.path.basename(base).rsplit('-', 1)[-1].isdigit() else 0
                break
        except Exception:
            pass
    return {
        'active': False,
        'available': bool(_glob.glob('/dev/dri/card0')),
        'connected': connected,
        'connector_id': connector_id,
        'crtc_id': 0,
        'drm_device': '/dev/dri/card0',
        'drm_open': False,
        'dropped': 0,
        'enabled': False,
        'fps': 0.0,
        'frames': 0,
        'height': 0,
        'last_error': '',
        'overlay_active': False,
        'overlay_available': False,
        'overlay_draw_ms': 0.0,
        'overlay_dropped': 0,
        'overlay_enabled': False,
        'overlay_last_error': '',
        'overlay_pixel_format': '',
        'overlay_plane_id': 0,
        'overlay_plane_name': '',
        'overlay_status': 'disabled',
        'overlay_updates': 0,
        'pixel_format': 'rgb888',
        'refresh': 0,
        'status': 'disabled',
        'width': 0,
    }
