"""sysinfo.py — 板载资源采集（procfs / sysfs 真实读数，非占位）。

从 ``plugins/web/bin/ttbox-web.py`` 拆出（2026-10-02 web 换写法 S1）。
**行为逐字保留**：函数名、返回结构、异常兜底口径与原实现一致。

读者：``/api/system``（系统状态页）、``/api/system/hostname``、``/api/system/web-port``
     （两者经 ``collect_network_summary`` 返回网络摘要，Web 契约不变）。

这些名字**不在**测试的 monkeypatch 打点清单里（见 ``lib/hub.py`` 顶部），
所以本模块可以安全持有自己的全局状态（``_CPU_T0`` 等采样缓存）。
"""
from __future__ import annotations

import os

from plugins.web.lib.settings import LISTEN_PORT


_CPU_T0 = [0, 0.0]
_CPU_TOTAL0 = [0, 0.0]


def _read_float(path, default=0.0):
    try:
        with open(path, 'r') as f:
            return float(f.read().strip())
    except Exception:
        return default


def _read_int(path, default=0):
    try:
        with open(path, 'r') as f:
            return int(f.read().strip())
    except Exception:
        return default


def _cpu_percent():
    """两次 /proc/stat 采样差分 → CPU 占用%（0~100）。"""
    try:
        with open('/proc/stat') as f:
            parts = f.readline().split()
        vals = [int(x) for x in parts[1:]]
        if len(vals) < 4:
            return 0.0
        idle = vals[3] + (vals[4] if len(vals) > 4 else 0)
        total = sum(vals)
        if _CPU_T0[0] > 0:
            didle = idle - _CPU_T0[1]
            dtotal = total - _CPU_TOTAL0[1]
            _CPU_T0[0] += 1
            _CPU_TOTAL0[0] += 1
            if dtotal > 0:
                return round(max(0.0, min(100.0, 100.0 * (1.0 - didle / dtotal))), 1)
        _CPU_T0[0] += 1
        _CPU_TOTAL0[0] += 1
        _CPU_T0[1] = idle
        _CPU_TOTAL0[1] = total
        return 0.0
    except Exception:
        return 0.0


def _memory():
    try:
        with open('/proc/meminfo') as f:
            mem = {}
            for line in f:
                k, _, v = line.partition(':')
                if k in ('MemTotal', 'MemFree', 'MemAvailable', 'Buffers', 'Cached'):
                    mem[k] = int(v.strip().split()[0]) * 1024
        total = mem.get('MemTotal', 0)
        avail = mem.get('MemAvailable', mem.get('MemFree', 0))
        used = max(0, total - avail)
        return {
            'total': total, 'used': used, 'free': avail,
            'percent': round(100.0 * used / total, 1) if total else 0.0,
        }
    except Exception:
        return {'total': 0, 'used': 0, 'free': 0, 'percent': 0.0}


def _temperature():
    """优先 SoC 温度（thermal_zone0 soc-thermal），回退第一个可用 zone。"""
    best = None
    try:
        import glob
        for z in sorted(glob.glob('/sys/class/thermal/thermal_zone*')):
            try:
                with open(z + '/type') as f:
                    ztype = f.read().strip()
            except Exception:
                continue
            temp = _read_float(z + '/temp', 0.0) / 1000.0
            if temp <= 0:
                continue
            if ztype == 'soc-thermal':
                return {'celsius': round(temp, 1), 'label': 'SoC', 'zone': ztype}
            if best is None:
                best = (temp, ztype)
    except Exception:
        pass
    if best:
        return {'celsius': round(best[0], 1), 'label': best[1], 'zone': best[1]}
    return {'celsius': 0.0, 'label': 'thermal', 'zone': ''}


def _storage():
    try:
        st = os.statvfs('/')
        total = st.f_blocks * st.f_frsize
        free = st.f_bfree * st.f_frsize
        used = total - free
        avail = st.f_bavail * st.f_frsize
        return {
            'total': total, 'used': used, 'free': avail,
            'percent': round(100.0 * used / total, 1) if total else 0.0,
        }
    except Exception:
        return {'total': 0, 'used': 0, 'free': 0, 'percent': 0.0}


def _load_average():
    try:
        with open('/proc/loadavg') as f:
            parts = f.read().split()
        return [float(x) for x in parts[:3]]
    except Exception:
        return []


def _hostname():
    try:
        import socket as _s
        return _s.gethostname()
    except Exception:
        return 'ttbox'


def _lan_ipv4():
    try:
        import subprocess as _sp
        out = _sp.check_output(['hostname', '-I'], text=True, timeout=2).split()
        for ip in out:
            if ip and not ip.startswith('127.'):
                return ip
    except Exception:
        pass
    return ''


def _uptime_seconds():
    try:
        with open('/proc/uptime') as f:
            return float(f.read().split()[0])
    except Exception:
        return 0.0


def collect_system_stats() -> dict:
    return {
        'hostname': _hostname(),
        'uptime_seconds': _uptime_seconds(),
        'cpu_percent': _cpu_percent(),
        'load_average': _load_average(),
        'memory': _memory(),
        'temperature': _temperature(),
        'storage': _storage(),
        'lan_ipv4': _lan_ipv4(),
        'lan_url': '',
        'mdns_url': '',
        'web_port': LISTEN_PORT,
        'os': 'Orange Pi 1.2.0',
        'version': '',
        'app_version': 'ttbox-0.1.0',
    }


def collect_network_summary() -> dict:
    """保持 Web 契约 hostname PUT / web-port PUT 返回的网络摘要结构。"""
    stats = collect_system_stats()
    return {
        'hostname': stats.get('hostname', ''),
        'lan_ipv4': stats.get('lan_ipv4', ''),
        'lan_url': stats.get('lan_url', ''),
        'mdns_url': stats.get('mdns_url', ''),
        'web_port': stats.get('web_port', LISTEN_PORT),
    }
