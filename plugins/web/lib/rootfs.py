"""rootfs.py — 根分区扩容可行性探测（2026-10-02 web 换写法 S2）。

从 ``plugins/web/bin/ttbox-web.py`` 拆出。**行为逐字保留**。

## 两个问题必须分开回答，不能挤进同一个布尔

  ① ``expandable``       = 这台机器物理上有没有可扩空间（读分区表算出来）
  ② ``action_available`` = 本仓有没有把"改分区表 + resize2fs"装箱成执行通道

旧实现把两者合成一个恒真的 ``expandable=True``，还附赠一句凭空的
"检测到磁盘尾部还有约 N GB 可扩容空间"（那个 N 取自 df 的可用空间，
与磁盘尾部有没有未分配扇区毫无关系 —— 纯编造）。

## 与入口模块的关系

``_run_quiet`` / ``_sysfs_int`` **定义在入口模块**（测试会 monkeypatch 这两个名字，
见 ``lib/hub.py`` 顶部说明），本模块经 hub 在**调用时**取，补丁因此照旧生效。
"""
from __future__ import annotations

import os
import shutil
import time

from plugins.web.lib import hub
from plugins.web.lib.sysinfo import _storage


ROOTFS_TAIL_MIN_BYTES = 64 * 1024 * 1024
ROOTFS_EXPAND_FS = ('ext2', 'ext3', 'ext4')
ROOTFS_EXPAND_TOOLS = ('growpart', 'resize2fs')
_ROOTFS_PROBE_CACHE: dict = {'ts': 0.0, 'data': None}
_ROOTFS_PROBE_TTL_SEC = 5.0


def _run_quiet(argv, timeout=5.0):
    """转发到入口模块的 ``_run_quiet``（测试的 monkeypatch 落点）。"""
    return hub.call('_run_quiet', argv, timeout)


def _sysfs_int(path):
    """转发到入口模块的 ``_sysfs_int``（测试的 monkeypatch 落点）。"""
    return hub.call('_sysfs_int', path)


def _human_bytes(n: int) -> str:
    if n < 0:
        return '未知'
    value = float(n)
    for unit in ('B', 'KB', 'MB', 'GB'):
        if value < 1024 or unit == 'GB':
            return f'{value:.1f} {unit}'
        value /= 1024
    return f'{value:.1f} GB'


def _rootfs_expand_probe_uncached() -> dict:
    """探测根分区扩容可行性 —— 数据全部来自 findmnt / lsblk / sysfs，无预置结论。"""
    probe = {
        'ok': True,
        'supported': True,
        'expandable': False,
        'reason': '',
        'message': '',
        'method': 'growpart',
        'missing_tools': [],
        # 执行通道：本仓尚未把"改分区表 + resize2fs"装箱（没有特权 worker）。
        # 探测到可扩空间时 expandable 为真、按钮依然不可用，由前端读这个字段。
        'action_available': False,
        'action_reason': 'expand_worker_not_implemented',
        'root': {},
    }
    src = _run_quiet(['findmnt', '-no', 'SOURCE', '/'])
    fstype = _run_quiet(['findmnt', '-no', 'FSTYPE', '/'])
    root = {'device': src, 'label': '根分区', 'disk': '', 'fstype': fstype}
    probe['root'] = root

    if not src.startswith('/dev/'):
        probe.update(supported=False, reason='unsupported_root',
                     message=f'根文件系统不是块设备分区（{src or "未知"}），不支持在线扩容')
        return probe

    part = os.path.basename(src)
    disk_lines = _run_quiet(['lsblk', '-no', 'PKNAME', src]).splitlines()
    disk = disk_lines[0].strip() if disk_lines else ''
    if disk:
        root['disk'] = '/dev/' + disk

    p_start = _sysfs_int(f'/sys/class/block/{part}/start')
    p_size = _sysfs_int(f'/sys/class/block/{part}/size')
    d_size = _sysfs_int(f'/sys/class/block/{disk}/size') if disk else -1
    tail = -1
    if p_start >= 0 and p_size >= 0 and d_size >= 0:
        tail = (d_size - (p_start + p_size)) * 512
        root['free_after_partition'] = tail

    missing = [t for t in ROOTFS_EXPAND_TOOLS if shutil.which(t) is None]
    probe['missing_tools'] = missing

    if fstype and fstype not in ROOTFS_EXPAND_FS:
        probe.update(reason='unsupported_filesystem',
                     message=f'根分区文件系统是 {fstype}，本仓只支持 ext4 在线扩容')
        return probe
    if missing:
        probe.update(reason='missing_tools',
                     message='缺少扩容工具：' + '、'.join(missing))
        return probe
    if tail < 0:
        probe.update(ok=False, reason='probe_failed',
                     message=f'读不到 {root["disk"] or part} 的分区表，扩容可行性未知')
        return probe
    if tail < ROOTFS_TAIL_MIN_BYTES:
        probe.update(reason='no_tail_space',
                     message=(f'磁盘尾部只剩 {_human_bytes(tail)}，'
                              f'不足 {_human_bytes(ROOTFS_TAIL_MIN_BYTES)}，无法扩容'))
        return probe

    probe.update(expandable=True, reason='ok',
                 message=f'磁盘尾部有 {_human_bytes(tail)} 未分配空间，可扩容')
    return probe


def _rootfs_expand_probe(force: bool = False) -> dict:
    """带 5 秒缓存的探测（findmnt/lsblk 各一次，别被轮询打成热点）。"""
    now = time.time()
    cached = _ROOTFS_PROBE_CACHE['data']
    if not force and cached is not None and now - _ROOTFS_PROBE_CACHE['ts'] < _ROOTFS_PROBE_TTL_SEC:
        return dict(cached)
    probe = _rootfs_expand_probe_uncached()
    _ROOTFS_PROBE_CACHE['ts'] = now
    _ROOTFS_PROBE_CACHE['data'] = dict(probe)
    return probe


def _rootfs_expand_payload(action: str, force: bool = False) -> dict:
    """探测结果 + df 容量，拼成一份完整的 rootfs 契约对象。"""
    s = _storage()
    payload = dict(_rootfs_expand_probe(force=force))
    payload.update({
        'action': action,
        'can_expand': payload['expandable'],
        'percent': s['percent'],
        'total': s['total'],
        'used': s['used'],
        'free': s['free'],
    })
    return payload
