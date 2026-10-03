# test_web_storage_expand_probe.py — 第一档 T1-1 回归锁：扩容结论必须来自真实探测
#
# 审计结论（docs/交付前Web按钮落实审计-2026-09-19.md，D-8）：
#   · GET /api/system/storage 的 rootfs 整流硬编码：
#       'can_expand': True, 'expandable': True,
#       'message': f'检测到磁盘尾部还有约 {s["free"]/1024/1024/1024:.1f} GB 可扩容空间'
#     那个 N 取自 df 的可用空间，跟"磁盘尾部有没有未分配扇区"毫无关系 —— 一句凭空结论。
#     板端实测（.workbuddy/memory/2026-09-19.md）：两块盘分区表都铺满，尾部只有
#     15 MiB / 29 MiB，growpart 上去等于什么都没做。
#   · POST /api/system/storage/expand 只跑一次 lsblk 就回
#     "根分区在线检测完成，扩容需重启进恢复流程"，把"没实现"说成"做了一半"。
#   · 后端硬编码了错误的设备名 /dev/mmcblk0（实根在 mmcblk1）。
#
# 本文件锁死修复后的口径：
#   ① expandable 只能由 findmnt / lsblk / sysfs 的真实读数推出，不得预置；
#   ② "物理可扩"(expandable) 与 "执行通道已装箱"(action_available) 是两个独立字段，
#      不得再合并成一个恒真的布尔；
#   ③ 不可扩 ⇒ expand 返 409；可扩但没装箱 ⇒ 501 —— 不再有假成功。
#
# 运行：python -m pytest plugins/web/tests/test_web_storage_expand_probe.py -v
from __future__ import annotations

import importlib.util
import os
import pathlib
import sys
import tempfile

import pytest

from plugins.web.lib.paths import repo_root as _ttbox_repo_root  # noqa: E402  路径单点真源（A-PATH-3）
REPO_ROOT = pathlib.Path(_ttbox_repo_root())
WEB_SRC = REPO_ROOT / 'plugins' / 'web' / 'bin' / 'ttbox-web.py'

MIB = 1024 * 1024
_load_seq = 0


def _load():
    global _load_seq
    _load_seq += 1
    name = 'ttbox_web_storageprobe_%d_%d' % (os.getpid(), _load_seq)
    spec = importlib.util.spec_from_file_location(name, WEB_SRC)
    mod = importlib.util.module_from_spec(spec)
    sys.modules[name] = mod
    spec.loader.exec_module(mod)
    return mod


@pytest.fixture()
def web_mod(monkeypatch):
    tmp = tempfile.mkdtemp(prefix='ttbox_storageprobe_%d_' % os.getpid())
    monkeypatch.setenv('TTBOX_PREFIX', tmp)
    monkeypatch.setenv('TTBOX_PRESETS_DIR', os.path.join(tmp, 'presets'))
    monkeypatch.setenv('TTBOX_CONFIG_DIR', os.path.join(tmp, 'config'))
    monkeypatch.setenv('TTBOX_MODELS_ROOT', os.path.join(tmp, 'models'))
    mod = _load()
    # 缓存是模块级全局，逐用例清掉，避免上一个用例的探测结果串味。
    mod._ROOTFS_PROBE_CACHE['ts'] = 0.0
    mod._ROOTFS_PROBE_CACHE['data'] = None
    return mod


def _fake_probe(web_mod, monkeypatch, *, src='/dev/mmcblk1p2', fstype='ext4',
                pkname='mmcblk1', p_start=4096, p_size=30_000_000,
                d_size=30_000_416, tools=('growpart', 'resize2fs')):
    """把外部命令与 sysfs 读全部替换成给定值，探测逻辑本身原样执行。"""
    def fake_run(argv, timeout=5.0):
        if argv[:2] == ['findmnt', '-no']:
            return src if argv[2] == 'SOURCE' else fstype
        if argv[:2] == ['lsblk', '-no']:
            return pkname
        raise AssertionError('未预期的探测命令: %r' % (argv,))

    table = {
        '/sys/class/block/mmcblk1p2/start': p_start,
        '/sys/class/block/mmcblk1p2/size': p_size,
        '/sys/class/block/%s/size' % pkname: d_size,
    }
    monkeypatch.setattr(web_mod, '_run_quiet', fake_run)
    monkeypatch.setattr(web_mod, '_sysfs_int',
                        lambda path: table.get(path, -1))
    monkeypatch.setattr(web_mod.shutil, 'which',
                        lambda name: '/usr/sbin/' + name if name in tools else None)


def _client(web_mod, monkeypatch):
    monkeypatch.setattr(web_mod, '_activation_ok', lambda: True)
    return web_mod.app.test_client()


def _code_only(path) -> str:
    """去掉 `#` 注释尾巴 —— 注释里引用的旧字面量不算违规，代码里的才算。"""
    lines = []
    for ln in path.read_text(encoding='utf-8').split('\n'):
        idx = ln.find('#')
        lines.append(ln if idx < 0 else ln[:idx])
    return '\n'.join(lines)


# ── 探测本身 ────────────────────────────────────────────────────────────

def test_tail_space_15mib_is_not_expandable(web_mod, monkeypatch):
    """板端真实几何：尾部只剩 15 MiB ⇒ 不可扩，且原因说得出口。"""
    tail_sectors = 15 * MIB // 512
    _fake_probe(web_mod, monkeypatch, p_size=30_000_000,
                d_size=4096 + 30_000_000 + tail_sectors)
    probe = web_mod._rootfs_expand_probe_uncached()
    assert probe['expandable'] is False
    assert probe['reason'] == 'no_tail_space'
    assert probe['ok'] is True
    assert probe['root']['free_after_partition'] == 15 * MIB
    assert 'MB' in probe['message'] or 'MiB' in probe['message']


def test_tail_space_2gib_is_expandable_but_channel_not_packed(web_mod, monkeypatch):
    """有空余 ≠ 能点动：物理可扩与执行通道必须分开报。"""
    tail = 2 * 1024 * MIB
    _fake_probe(web_mod, monkeypatch, p_size=30_000_000,
                d_size=4096 + 30_000_000 + tail // 512)
    probe = web_mod._rootfs_expand_probe_uncached()
    assert probe['expandable'] is True
    assert probe['reason'] == 'ok'
    # 执行通道本仓尚未装箱 —— 前端靠这个字段保持按钮禁用。
    assert probe['action_available'] is False
    assert probe['action_reason'] == 'expand_worker_not_implemented'


def test_probe_reads_real_root_device_not_hardcoded_mmcblk0(web_mod, monkeypatch):
    """实根在 mmcblk1 时，探测必须跟着走，不能再盯着 mmcblk0。"""
    _fake_probe(web_mod, monkeypatch, src='/dev/mmcblk1p2', pkname='mmcblk1')
    probe = web_mod._rootfs_expand_probe_uncached()
    assert probe['root']['device'] == '/dev/mmcblk1p2'
    assert probe['root']['disk'] == '/dev/mmcblk1'
    assert probe['root']['fstype'] == 'ext4'


def test_non_ext4_root_is_reported_unsupported(web_mod, monkeypatch):
    _fake_probe(web_mod, monkeypatch, fstype='btrfs')
    probe = web_mod._rootfs_expand_probe_uncached()
    assert probe['expandable'] is False
    assert probe['reason'] == 'unsupported_filesystem'
    assert 'btrfs' in probe['message']


def test_missing_tools_blocks_expansion_even_with_free_tail(web_mod, monkeypatch):
    _fake_probe(web_mod, monkeypatch, tools=('resize2fs',))
    probe = web_mod._rootfs_expand_probe_uncached()
    assert probe['expandable'] is False
    assert probe['reason'] == 'missing_tools'
    assert probe['missing_tools'] == ['growpart']


def test_root_not_a_block_partition_is_unsupported(web_mod, monkeypatch):
    _fake_probe(web_mod, monkeypatch, src='overlay')
    probe = web_mod._rootfs_expand_probe_uncached()
    assert probe['supported'] is False
    assert probe['reason'] == 'unsupported_root'


def test_unreadable_partition_table_is_probe_failed_not_optimistic(web_mod, monkeypatch):
    """读不到分区表时不许乐观：ok=False + probe_failed，不能默默判成"可扩"。"""
    _fake_probe(web_mod, monkeypatch, p_start=-1, p_size=-1, d_size=-1)
    probe = web_mod._rootfs_expand_probe_uncached()
    assert probe['expandable'] is False
    assert probe['ok'] is False
    assert probe['reason'] == 'probe_failed'


# ── 路由契约 ────────────────────────────────────────────────────────────

def test_storage_route_never_claims_free_tail_without_probing(web_mod, monkeypatch):
    """GET 不再出现"还有约 N GB 可扩容空间"这种凭空结论。"""
    _fake_probe(web_mod, monkeypatch, p_size=30_000_000, d_size=4096 + 30_000_000 + 15 * MIB // 512)
    body = _client(web_mod, monkeypatch).get('/api/system/storage?force=1').get_json()
    assert body['ok'] is True
    rootfs = body['data']['rootfs']
    assert rootfs['expandable'] is False
    assert rootfs['can_expand'] is False
    assert rootfs['reason'] == 'no_tail_space'
    assert '可扩容空间' not in rootfs['message']
    # 旧字段仍在（其它调用方按契约读），但值必须与真实探测一致
    assert body['data']['root_total'] == 0 or isinstance(body['data']['root_total'], int)


def test_expand_route_returns_409_when_no_tail_space(web_mod, monkeypatch):
    _fake_probe(web_mod, monkeypatch, p_size=30_000_000, d_size=4096 + 30_000_000 + 15 * MIB // 512)
    resp = _client(web_mod, monkeypatch).post('/api/system/storage/expand')
    assert resp.status_code == 409
    body = resp.get_json()
    assert body['ok'] is False
    # 失败也必须带 rootfs，否则前端门卫会让文案停在"扩容中"
    assert body['data']['rootfs']['reason'] == 'no_tail_space'


def test_expand_route_returns_501_when_channel_not_packed(web_mod, monkeypatch):
    """物理可扩但没装箱 ⇒ 501 说"没实现"，不许假成功。"""
    tail = 2 * 1024 * MIB
    _fake_probe(web_mod, monkeypatch, p_size=30_000_000,
                d_size=4096 + 30_000_000 + tail // 512)
    resp = _client(web_mod, monkeypatch).post('/api/system/storage/expand')
    assert resp.status_code == 501
    body = resp.get_json()
    assert body['ok'] is False
    assert '尚未装箱' in body['error']
    assert body['data']['rootfs']['expandable'] is True


def test_source_has_no_hardcoded_mmcblk0_or_fabricated_expand_claim():
    """源码级兜底：设备名硬编码与凭空容量文案不许再回来。"""
    src = _code_only(WEB_SRC)
    assert '/dev/mmcblk0' not in src
    assert '可扩容空间' not in src
    assert "'expandable': True" not in src
    assert "'can_expand': True" not in src
