# -*- coding: utf-8 -*-
"""锁定 panel/*.js 的「加载顺序不变式」—— 2026-10-03（拆出 00-const.js 配套）。

★ 为什么需要这个文件（这是本轮拆分的真实代价）：
  2026-10-03 把 00-shared.js 里的 59 个 const/let/var 拆到 00-const.js，
  index.html 的外链从 11 个变成 12 个。而**加载顺序清单在 6 个测试文件里
  各抄了一份**（panel_src.py / test_web_panel_smoke.py /
  test_web_console_parity.py 4 处 / test_web_aim_head_box.py /
  test_web_calibration_hot_reload.py），共 12 处硬编码。
  ⇒ 以后再加/删一个面板 JS 文件，就得同步改 12 处，漏一处就是「测试假绿」
  或「面板某个文件根本没被加载」。

  与其靠人肉同步，不如让测试自己守住不变式：
    ① 磁盘上的 panel/*.js 集合 == panel_src.PANEL_JS_PARTS（不许有孤儿文件）
    ② index.html 的 <script> 顺序 == PANEL_JS_PARTS（顺序必须一致）
    ③ 每个被引用的文件必须真的存在（引了不存在的 ⇒ 浏览器 404 静默失败）
    ④ **00-const.js 必须排第一**（const 不 hoist，排后面 ⇒ TDZ 报错）
    ⑤ 00-const.js 里不许出现 function（它是纯声明文件，出现 function 就
       说明有人把逻辑误搬进来了，或者把胶水函数误搬出去了）
"""
from __future__ import annotations

import re
from pathlib import Path

from plugins.web.tests import panel_src

PANEL_DIR = panel_src.STATIC_DIR / 'panel'
# ★ 加载顺序 = 分层顺序（2026-10-03 改名 00-shared.js → 10-flow.js 时确立）：
#   00-const  数据层（必须最先，const 不hoist）
#   01..09    页签层（互不调用，只依赖 00-const）
#   calib-bind 标定组
#   10-flow   流程编排层（调用 01..09，故排最后）
EXPECTED_ORDER = [
    '00-const.js',
    '01-home.js',
    '02-hotkey.js',
    '03-pointer.js',
    '04-assist.js',
    '05-model.js',
    '06-hardware.js',
    '07-preset.js',
    '08-license.js',
    '09-fan.js',
    'calib-bind.js',
    '10-flow.js',
]


def _on_disk() -> list:
    return sorted(p.name for p in PANEL_DIR.glob('*.js'))


def test_no_orphan_js_files():
    """① 磁盘上的每个 .js 都必须在清单里；清单里的也必须在磁盘上。"""
    disk = _on_disk()
    listed = [p.name for p in panel_src.PANEL_JS_PARTS]
    assert sorted(listed) == disk, (
        'panel/*.js 与 PANEL_JS_PARTS 不一致。\n'
        '  磁盘上多出/缺少: %s\n'
        '  ⇒ 新增或删除面板 JS 文件后，必须同步 panel_src.PANEL_JS_PARTS。'
        % sorted(set(disk) ^ set(listed))
    )


def test_index_html_script_order_matches_list():
    """② index.html 的加载顺序必须与 PANEL_JS_PARTS 逐项一致（含顺序）。"""
    html = panel_src.html_src()
    links = re.findall(r"""filename='(panel/[^']+\.js)'""", html)
    listed = ['panel/%s' % p.name for p in panel_src.PANEL_JS_PARTS]
    assert links == listed, (
        'index.html 的 <script> 顺序与 PANEL_JS_PARTS 不一致。\n'
        '  index.html: %s\n  清单      : %s' % (links, listed)
    )


def test_every_referenced_js_exists():
    """③ 引用的文件必须真实存在（否则浏览器 404 静默失败，面板半残）。"""
    for p in panel_src.PANEL_JS_PARTS:
        assert p.exists(), 'index.html 引用了不存在的文件: %s' % p


def test_const_file_loads_first():
    """④ 00-const.js 必须排第一。

    JS 的 const/let 不 hoist（无函数声明那样的提升），
    排到后面 ⇒ 00-shared.js 与 01..09 取常量时抛
    ReferenceError: Cannot access 'X' before initialization。
    """
    html = panel_src.html_src()
    links = re.findall(r"""filename='(panel/[^']+\.js)'""", html)
    assert links[0] == 'panel/00-const.js', (
        '00-const.js 必须第一个加载（它含全部 59 个 const/let/var），'
        '实际第一个是 %s' % links[0]
    )


def test_const_file_is_declarations_only():
    """⑤ 00-const.js 只许有声明，不许有 function 定义。

    它排在最前，一旦里面有 function 且函数体调用了 00-shared.js 的东西，
    就会在加载期直接炸（被调方还没加载）。
    """
    src = (PANEL_DIR / '00-const.js').read_text(encoding='utf-8')
    bad = [ln.strip() for ln in src.splitlines()
           if re.match(r'^(?:async\s+)?function\s+\w+', ln)]
    assert not bad, (
        '00-const.js 里出现 function 定义：%s\n'
        '  ⇒ 它必须只含const/let/var 声明（纯数据）。\n'
        '  胶水函数请放回 00-shared.js。' % bad
    )
    # 反向：59 个声明一个都不能少（数量锁死，拆漏了立刻红）
    n = len(re.findall(r'^(?:const|let|var)\s+\w+', src, re.M))
    assert n == 59, '00-const.js 应恰好 59 个顶层声明，实际 %d 个' % n


def test_expected_order_is_documented():
    """⑥ 本文件里的 EXPECTED_ORDER 与实际清单一致（防止两处都漂）。"""
    listed = [p.name for p in panel_src.PANEL_JS_PARTS]
    assert listed == EXPECTED_ORDER, (
        '加载顺序变了：\n  本文件记录: %s\n  实际清单  : %s\n'
        '  ⇒ 若确实要调整加载顺序，请同时改 index.html、panel_src.PANEL_JS_PARTS '
        '与本文件。' % (EXPECTED_ORDER, listed)
    )
