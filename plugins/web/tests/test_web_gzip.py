# -*- coding: utf-8 -*-
"""静态资源 gzip 压缩的行为测试 —— 2026-10-03 性能优化第 1 项。

★ 重点测「不该压的没压」：
  .mjpg 预览视频流被压会破坏 multipart 流边界 ⇒ 必须原样返回。
  这条比「压了」更重要（压错 = 功能坏；没压 = 只是慢）。
"""
from __future__ import annotations

import gzip
import importlib.util
import sys

from plugins.web.lib import web_compress
from plugins.web.tests import panel_src

_WEB = {}
GZ = {'Accept-Encoding': 'gzip, deflate, br'}


def _load_web():
    """Load the web entry module once.

    ★ Root comes from panel_src (which uses paths.repo_root -> discover_root),
      so no `parents[N]` hardcoding here — the conventions gate rejects that.
    """
    if 'web' in _WEB:
        return _WEB['web']
    entry = panel_src.REPO_ROOT / 'plugins' / 'web' / 'bin' / 'ttbox-web.py'
    spec = importlib.util.spec_from_file_location('ttbox_web_gzip_probe', entry)
    web = importlib.util.module_from_spec(spec)
    sys.modules['ttbox_web_gzip_probe'] = web
    spec.loader.exec_module(web)
    _WEB['web'] = web
    return web


def test_gzip_enabled_for_panel_js():
    """面板 JS 必须被压，且解压后与源文件逐字节一致。"""
    web = _load_web()
    r = web.app.test_client().get('/static/panel/10-flow.js', headers=GZ)
    assert r.status_code == 200
    assert r.headers.get('Content-Encoding') == 'gzip', (
        '10-flow.js 应被 gzip 压缩，实际头=%r' % r.headers.get('Content-Encoding'))
    src = (web.STATIC_DIR / 'panel' / '10-flow.js').read_bytes()
    assert gzip.decompress(r.data) == src, 'gzip 解压后与源文件不一致'
    assert len(r.data) < len(src) * 0.6, (
        '压缩率不足：%d -> %d（期望 <60%%）' % (len(src), len(r.data)))


def test_all_panel_assets_roundtrip():
    """★ 所有可压静态资源：gzip 解压后必须与磁盘文件逐字节一致。"""
    web = _load_web()
    c = web.app.test_client()
    files = sorted((web.STATIC_DIR / 'panel').glob('*.js')) \
        + [web.STATIC_DIR / 'panel.css']
    checked = 0
    for p in files:
        rel = '/static/panel/%s' % p.name if p.parent.name == 'panel' \
            else '/static/panel.css'
        r = c.get(rel, headers=GZ)
        if r.headers.get('Content-Encoding') == 'gzip':
            assert gzip.decompress(r.data) == p.read_bytes(), \
                '%s 解压后与磁盘文件不一致' % p.name
            checked += 1
    assert checked >= 10, '实际压缩生效的文件太少：%d' % checked


def test_no_compression_without_accept_encoding():
    """客户端不支持 gzip ⇒ 原样返回。"""
    web = _load_web()
    r = web.app.test_client().get('/static/panel/10-flow.js')
    assert r.headers.get('Content-Encoding') is None, (
        '客户端没声明 Accept-Encoding 时不该压缩')


def test_mjpg_stream_is_never_compressed():
    """★★ 关键安全边界：.mjpg 预览流绝对不能压。"""
    web = _load_web()
    for suffix in ('.mjpg', '.mjpeg', '.mp4', '.png', '.jpg', '.gif',
                   '.webp', '.ico', '.woff', '.woff2', '.ttf', '.zip'):
        assert not web_compress.is_compressible('/static/x%s' % suffix), \
            '%s 被判为可压缩' % suffix
    # 端到端：预览流端点不得带 Content-Encoding
    r = web.app.test_client().get('/api/preview.mjpg')
    assert r.headers.get('Content-Encoding') is None, \
        '★ 预览视频流被压了 —— 会破坏流边界'


def test_compressible_judgement():
    """文本类静态资源判定为可压缩，其它一律不压。"""
    assert web_compress.is_compressible('/static/panel/10-flow.js')
    assert web_compress.is_compressible('/static/panel.css')
    assert web_compress.is_compressible('/static/x.json')
    assert not web_compress.is_compressible('/api/state')
    assert not web_compress.is_compressible('/')


def test_q_zero_means_refuse():
    """Accept-Encoding 里的 q 值语义：gzip;q=0 表示明确拒绝。"""
    assert web_compress._wants_gzip_str('gzip;q=0') is False
    assert web_compress._wants_gzip_str('gzip;q=0.0') is False
    assert web_compress._wants_gzip_str('gzip;q=1.0') is True
    assert web_compress._wants_gzip_str('gzip') is True
    assert web_compress._wants_gzip_str('br') is False
    assert web_compress._wants_gzip_str('') is False
    assert web_compress._wants_gzip_str('gzip, deflate, br') is True


def test_small_files_are_not_compressed():
    """小文件压了反而变大（gzip 头尾开销）⇒ 不压。"""
    assert web_compress.MIN_BYTES >= 512, '阈值太小会把小文件压大'
    web = _load_web()
    r = web.app.test_client().get('/static/panel/04-assist.js', headers=GZ)
    assert r.headers.get('Content-Encoding') is None, \
        '小于 MIN_BYTES 的文件不该被压'
