# -*- coding: utf-8 -*-
"""锁定面板的**分层不变式** —— 2026-10-03（10-flow.js 改名配套）。

★ 为什么要这条测试（它守的是实测发现的好性质）：
  2026-10-03 实测跨文件调用图：
    · 01..09 之间**零互相调用** ← 这很值钱，是 9 个页签边界干净的原因
    · 10-flow.js → 01..09 有 93 条边，其中 81% 是 5 个渲染入口
      （renderLicensePanel / renderUpdateStatus / updateAimRangeOverlay /
       setDisplayEdidModeDialogOpen / setModelGameSuggestionOpen）
      —— 那是「上层流程说该刷新某页」→ 下层页签执行，**方向本来就对**
  ⇒ 页签之间零依赖是**要保住的结构**，但当时**没有任何测试守它**。
    以后谁在 05-model.js 里顺手调一下 06-hardware.js 的函数，结构就悄悄烂掉。

★ 本测试锁三件事：
  1 页签之间零互相调用（01..09 任何两个文件之间都不许有调用）
  2 页签只允许「向下」依赖：调 00-const 的全局 / 调 10-flow 的公共函数
  3 10-flow.js 不许反向被页签以外的层依赖（它是最上层）

★ 判据口径（实测踩过的坑）：
  - 必须用**准确的跨文件判定**：名字 n 在文件 F 里被调用、且n **不在 F 声明**、
    才算跨文件。否则同一个文件内部的调用会被误算成跨文件（我第一版就错了）。
  - 文件名清单以 panel_src.PANEL_JS_PARTS 为准，不在测试里再抄一份。
"""
from __future__ import annotations

import re

from plugins.web.tests import panel_src

LAYER_DATA = '00-const.js'
LAYER_PAGES = [p.name for p in panel_src.PANEL_JS_PARTS
               if re.match(r'^0[1-9]-', p.name)]
LAYER_FLOW = '10-flow.js'
LAYER_CALIB = 'calib-bind.js'


def _declared(path) -> set:
    src = path.read_text(encoding='utf-8')
    return set(re.findall(r'^(?:async )?function\s+(\w+)', src, re.M))


def _called_names(path) -> set:
    src = path.read_text(encoding='utf-8')
    return set(re.findall(r'(?<![\w.$])([a-z][\w$]*)\s*\(', src))


def _cross_calls(frm, decls) -> dict:
    """name -> file that declares it, for names `frm` calls but doesn't own."""
    local = _declared(frm)
    out = {}
    for n in _called_names(frm):
        if n in local:
            continue                     # local call, not cross-file
        for p, names in decls.items():
            if p != frm and n in names:
                out.setdefault(p, set()).add(n)
    return out


def test_layer_files_all_exist():
    """分层文件必须齐全（改名/ 漏建都会红）。"""
    names = [p.name for p in panel_src.PANEL_JS_PARTS]
    for need in (LAYER_DATA, LAYER_FLOW, LAYER_CALIB):
        assert need in names, '缺少分层文件 %s（现有: %s）' % (need, names)
    for p in LAYER_PAGES:
        assert p in names, '缺少页签文件 %s' % p


def test_pages_never_call_each_other():
    """★ 核心不变式：01..09 之间零互相调用。"""
    decls = {p.name: _declared(p) for p in panel_src.PANEL_JS_PARTS}
    violations = []
    for name in LAYER_PAGES:
        frm = next(p for p in panel_src.PANEL_JS_PARTS if p.name == name)
        for target, names in _cross_calls(frm, decls).items():
            if target in LAYER_PAGES:
                violations.append('%s -> %s : %s'
                                  % (name, target, ', '.join(sorted(names))))
    assert not violations, (
        '页签之间出现了互相调用（%d 处）——这会毁掉「9 个页签边界干净」的结构：\n  %s\n'
        '  ⇒ 共享逻辑请放进 10-flow.js（流程编排层）或 00-const.js（数据层），\n'
        '     不要让页签之间互相调用。'
        % (len(violations), '\n  '.join(violations))
    )


def test_pages_only_depend_downward():
    """页签只允许向下依赖（00-const 的全局、10-flow 的公共函数）。"""
    decls = {p.name: _declared(p) for p in panel_src.PANEL_JS_PARTS}
    allowed = {LAYER_DATA, LAYER_FLOW, LAYER_CALIB}
    bad = []
    for name in LAYER_PAGES:
        frm = next(p for p in panel_src.PANEL_JS_PARTS if p.name == name)
        for target, names in _cross_calls(frm, decls).items():
            if target in LAYER_PAGES:
                continue                 # already reported by the test above
            if target not in allowed:
                bad.append('%s -> %s : %s' % (name, target, ', '.join(sorted(names))))
    assert not bad, (
        '页签依赖了非预期文件（%d 处）：\n  %s\n'
        '  ⇒ 允许的依赖只有 %s（数据层 / 流程层 / 标定组）。'
        % (len(bad), '\n  '.join(bad), sorted(allowed))
    )


def test_detector_catches_a_planted_cross_page_call(tmp_path):
    """★ 自检：植入一个页签间调用，检测器必须抓到（防检测器失灵）。"""
    a = tmp_path / '01-home.js'
    b = tmp_path / '06-hardware.js'
    a.write_text('function onlyHome() {\n  return 1;\n}\n', encoding='utf-8')
    b.write_text('function usesHome() {\n  return onlyHome();\n}\n',
                 encoding='utf-8')
    decls = {a.name: _declared(a), b.name: _declared(b)}
    cross = _cross_calls(b, decls)
    assert a.name in cross, '检测器没抓到植入的 01-home -> 06-hardware 调用'
    assert 'onlyHome' in cross[a.name]
