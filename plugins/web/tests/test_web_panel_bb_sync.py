# test_web_panel_bb_sync.py — 面板（index.html）与后端（ttbox-web.py）的「一套口径」守卫
#
# 背景：辅助功能页收敛成 4 个分区（压枪 / 自动开火 / 拉枪曲线 / 选靶），
# 分区里的字段（2026-09-30 重数；删 lead1/trigger/三段查表/垂直修正/闭环后）
# 由 index.html 的 BB_CTRL_MODULES **表驱动**读写，而这张表是从
# ttbox-web.py 的后端表机械生成的。两处一旦不同步，症状是"面板能调但存不下去"或者
# "存下去了但面板打开显示成别的值"—— 都不会报错，只会静默错。
#
# 本文件锁三件事：
#   ① 面板表 == 后端表（逐项字段名/类型/默认值）；
#   ② 老界面（压枪三段查表 / 垂直修正 / 开火期闭环、连点 rapid_fire、
#      自动开火占位编辑器）已经下线。
#
# 运行：python -m pytest plugins/web/tests/test_web_panel_bb_sync.py -v
import importlib.util
import json
import os
import pathlib
import re
import sys

WEB_SRC = pathlib.Path(__file__).resolve().parents[3] / 'plugins' / 'web' / 'bin' / 'ttbox-web.py'
INDEX = pathlib.Path(__file__).resolve().parents[3] / 'plugins' / 'web' / 'templates' / 'index.html'

_load_seq = 0


def _load_backend():
    global _load_seq
    _load_seq += 1
    name = 'ttbox_web_sync_%d_%d' % (os.getpid(), _load_seq)
    spec = importlib.util.spec_from_file_location(name, WEB_SRC)
    mod = importlib.util.module_from_spec(spec)
    sys.modules[name] = mod
    spec.loader.exec_module(mod)
    return mod


def _panel_source():
    return INDEX.read_text(encoding='utf-8')


def _panel_modules():
    """把 index.html 里的 BB_CTRL_MODULES 解析成 {前缀: [(字段, 类型, 默认值), ...]}。"""
    src = _panel_source()
    start = src.index('const BB_CTRL_MODULES = [')
    end = src.index('\n];', start)
    body = src[start + len('const BB_CTRL_MODULES = '):end + 2]
    body = re.sub(r',(\s*[\]\}])', r'\1', body)          # 去掉尾逗号，变成合法 JSON
    raw = json.loads(body)
    out = {}
    for prefix, fields in raw:
        out[prefix] = [(f[0], f[1], f[2]) for f in fields]
    return out


# ---------------------------------------------------------------------------
# ① 面板表 == 后端表
# ---------------------------------------------------------------------------

def test_panel_table_matches_backend_tables():
    mod = _load_backend()
    panel = _panel_modules()

    want = {prefix: [(n, k, d) for (n, k, d) in fields] for (prefix, _obj, fields) in mod.CTRL_BLOCKS}
    want['selector'] = [(wk[len('selector_'):], k, d)
                        for (wk, _core, k, d) in mod.CTRL_SELECTOR_FIELDS]

    assert sorted(panel) == sorted(want), '面板模块前缀与后端不一致'

    for prefix in sorted(want):
        assert panel[prefix] == list(want[prefix]), \
            '模块 %s 的面板表与后端表不一致' % prefix


def test_panel_table_values_are_json_scalars():
    """表里的默认值必须是标量（数组字段走虚拟键单独处理，不进表）。"""
    for prefix, fields in _panel_modules().items():
        for name, kind, dflt in fields:
            assert kind in ('b', 'n', 'i', 'key'), (prefix, name, kind)
            assert isinstance(dflt, (bool, int, float)), (prefix, name, dflt)
            if kind == 'b':
                assert isinstance(dflt, bool), (prefix, name)
            if kind == 'key':
                assert isinstance(dflt, int), (prefix, name)


def test_panel_element_ids_cover_the_table():
    """每个表里的键都要有对应的界面元素：id = 前缀_字段（表里没有的界面元素会写不进配置）。"""
    src = _panel_source()
    ids = set(re.findall(r'\bid="([^"]+)"', src))
    missing = ['%s_%s' % (p, n) for p, fields in _panel_modules().items()
               for (n, _k, _d) in fields if '%s_%s' % (p, n) not in ids]
    assert missing == [], '表里有键但界面没渲染：%s' % missing


# ---------------------------------------------------------------------------
# ② 老界面已下线
# ---------------------------------------------------------------------------

def test_retired_panel_surfaces_are_gone():
    """已下线的界面不得残留：三段查表 / 垂直修正 / 开火期闭环 / 连点 / 占位编辑器。

    压枪在 2026-09-30 对照 yu 重做过：旧的多引擎（三段查表 + 垂直修正 + 闭环）
    收敛成一套速率引擎，面板只剩 8 个速率/门控参数。
    """
    src = _panel_source()
    for token in ('recoil_bb', 'vertical_correction', 'vc_enabled', 'vc_ramp1_enabled',
                  'recoil_cl', 'recoil_cl_enabled',
                  'recoil_humanize', 'humanize_curve_strength',
                  'rapid_fire', 'RAPID_FIRE', 'autoTrigger', 'auto-trigger',
                  'assist-section-rapid', 'unimplemented'):
        assert token not in src, '老界面残留：%s' % token


def test_panel_has_exactly_four_assist_sections():
    src = _panel_source()
    sections = re.findall(r'<section id="(assist-section-[a-z0-9-]+)"', src)
    assert sections == ['assist-section-recoil', 'assist-section-trigger',
                        'assist-section-lead', 'assist-section-selector'], sections
    tabs = re.findall(r'data-assist-section-target="(assist-section-[a-z0-9-]+)"', src)
    assert tabs == sections
    # 四个页签都不该是 disabled（内核已实现，功能可进入）
    assert not re.search(r'data-assist-section-target="[^"]+"[^>]*\bdisabled\b', src)
