# -*- coding: utf-8 -*-
"""DOM 元素引用必须显式走 $("id") —— 2026-10-03（拆 bind*Events 配套）。

★ 这条规则锁的是「拆函数时最容易犯、且最难被测出来」的一类 bug：
  搬代码时把`if (preview)`、`if (modelGameInput)` 搬进了新函数，
  但`const preview = $("previewImage")` 那个声明留在了原函数里。
  结果新函数里`preview` 是个**未声明标识符**。

★ 为什么不能靠"扫未声明标识符"来抓（我试过，失败）：
  纯静态单行分析在真实代码上误报 400+ 处 —— 多行链式表达式
  （`.replace(/x/, y)`换行续写）、catch(err) 、函数参数解构、箭头函数参数、
  对象键、三元表达式…… 要全部排除，得写一个 JS parser。
  ★ 已放弃那条路（过度工程，且每次修一类误报就冒出新一类）。
  ⇒ 改成锁**可判定的那一小块**：凡是「看起来在引用 DOM 元素」的裸名字，
    必须在**同一个函数体内**有对应的 $("...") 声明。

★ 口径（零误报，已按真实代码校准）：
  - 关注对象 =4 个历史上真踩过的名字（preview / modelGameInput /
    modelGameToggle / modelGameList），不是全量扫描
  - 判据 = 该函数体内是否存在 `const <name> = $(` 形式的声明
  - 自检 = 植入一个未声明用法必须被抓到（防检测器失灵）
"""
from __future__ import annotations

import re

from plugins.web.tests import panel_src

# 历史上真踩过的「靠 window[id] 也能跑」的名字 -> 正确取法
NAMED_RISK = {
    'preview': 'previewImage',
    'modelGameInput': 'modelImportGameProfile',
    'modelGameToggle': 'modelGameSuggestionToggle',
    'modelGameList': 'modelGameSuggestionList',
}

TOP_FN = re.compile(r'^(?:async )?function\s+(\w+)\s*\(')


def _function_blocks(src: str):
    """Yield (name, start_lineno, block_lines) per top-level function.

    边界取「下一个顶层 function 之前」——单行函数在同一行内深度就归零，
    按花括号深度切会只拿到 1 行（实测踩过）。
    """
    lines = src.splitlines()
    starts = [i for i, ln in enumerate(lines) if TOP_FN.match(ln)]
    for k, i in enumerate(starts):
        end = starts[k + 1] if k + 1 < len(starts) else len(lines)
        yield TOP_FN.match(lines[i]).group(1), i + 1, lines[i:end]


def _code_only(block) -> str:
    """Drop comment-only lines -- a comment mentioning the name is not a use."""
    return '\n'.join(ln for ln in block
                     if not ln.strip().startswith(('//', '*', '/*')))


def _uses_name(block, name: str) -> bool:
    return bool(re.search(r'(?<![\w.$])%s(?![\w$])' % re.escape(name), _code_only(block)))


def _declares_via_dollar(block, name: str) -> bool:
    return bool(re.search(r'const\s+%s\s*=\s*\$\(\s*"' % re.escape(name),
                          _code_only(block)))


def _find_missing(p) -> list:
    src = p.read_text(encoding='utf-8')
    out = []
    for fname, fstart, block in _function_blocks(src):
        text = '\n'.join(block)
        for name in NAMED_RISK:
            if not _uses_name(block, name):
                continue
            if _declares_via_dollar(block, name):
                continue
            out.append('%s:%d 函数 %s() 用了 %s 但没有 const %s = $("...")'
                       % (p.name, fstart, fname, name, name))
    return out


def test_risk_names_never_rely_on_window_globals():
    """点名的 4 个名字：用到它的地方必须同时有 $("...") 声明。"""
    problems = []
    for p in panel_src.PANEL_JS_PARTS:
        problems += _find_missing(p)
    assert not problems, (
        '发现 %d 处靠 window[id] 也能跑的裸引用：\n  %s\n'
        '  ★这类引用平时不炸，但元素未解析完成 / id 改名时直接 ReferenceError。\n'
        '  ⇒ 修法：在该函数内加 const %s = $("对应 id")。'
        % (len(problems), '\n  '.join(problems), list(NAMED_RISK.values())[0])
    )


def test_detector_catches_a_planted_case(tmp_path):
    """★ 自检：植入一个未声明用法必须被抓到（防检测器失灵）。"""
    f = tmp_path / 'bad.js'
    f.write_text('function bindStuff() {\n'
                 '  if (preview) {\n'
                 '    preview.focus();\n'
                 '  }\n'
                 '}\n', encoding='utf-8')
    hits = _find_missing(f)
    assert hits, '检测器没抓到植入的裸用 preview'


def test_detector_accepts_correct_code(tmp_path):
    """★ 反向自检：显式 $() 取元素的写法不许误报。"""
    f = tmp_path / 'ok.js'
    f.write_text('function bindStuff() {\n'
                 '  const preview = $("previewImage");\n'
                 '  if (preview) {\n'
                 '    preview.focus();\n'
                 '  }\n'
                 '}\n', encoding='utf-8')
    assert not _find_missing(f), '合法写法被误报: %s' % _find_missing(f)
