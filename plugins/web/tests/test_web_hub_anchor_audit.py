# -*- coding: utf-8 -*-
"""列出 web 换写法里**每个 hub 转发锚点**在入口的真实类型（def 函数 / 对象赋值 / 缺失）。

★ 为什么要它：今天在 hub 语义上连踩 4 次（2026-10-04），根因都是
  「分不清某个锚点在入口里是**函数**还是**对象**」：
    - `hub.call` = 调用语义 → 对**函数**用，**对对象必炸**（TypeError）；
    - `hub.get`  = 取值语义 → 对**对象**用。
  而且同一个名字在 lib/ 里**多处转发**（kAppVersion 在 cloud_hooks 与 core_state 都有），
  入口 import 的是哪一个决定一切。本脚本把事实摊平，别再靠猜。

用法：python -m pytest plugins/web/tests/test_web_hub_anchor_audit.py -q
     或直接 python plugins/web/tests/test_web_hub_anchor_audit.py（打印表）
"""
from __future__ import annotations

import re
import sys
from pathlib import Path

from plugins.web.lib.paths import discover_root

REPO = Path(discover_root(__file__))
WEB = REPO / 'plugins' / 'web'
ENTRY = WEB / 'bin' / 'ttbox-web.py'
LIB = WEB / 'lib'


def entry_text() -> str:
    return ENTRY.read_text(encoding='utf-8')


def classify(name: str) -> tuple[str, str]:
    """返回 (类型, 证据)。类型 ∈ {function, object, imported, missing}。"""
    src = entry_text()
    # ① 入口里 def 出来的函数
    if re.search(r'^def\s+%s\s*\(' % re.escape(name), src, re.M):
        return 'function', '入口 def'
    # ② 入口里赋值（含模块级单例/dict/Lock）
    m = re.search(r'^%s\s*=\s*(.*)$' % re.escape(name), src, re.M)
    if m:
        return 'object', '入口赋值: ' + m.group(1)[:60]
    # ③ 入口从 lib import 进来
    m = re.search(r'from\s+plugins\.web\.lib\.\w+\s+import\s+\(([^)]*)\)', src, re.S)
    if m and re.search(r'\b%s\b' % re.escape(name), m.group(1)):
        return 'imported', '入口从 lib import'
    m = re.search(r'from\s+plugins\.web\.lib\.\w+\s+import\s+[^\n(]*\b%s\b' % re.escape(name), src)
    if m:
        return 'imported', '入口从 lib import（单行）'
    return 'missing', '入口里找不到'


def all_anchors() -> dict[str, list[str]]:
    """扫出所有 hub.call / hub.get 用到的锚点名 → 用它的文件列表。"""
    out: dict[str, list[str]] = {}
    for p in list(LIB.glob('*.py')) + list((WEB / 'api').glob('*.py')):
        txt = p.read_text(encoding='utf-8')
        # ★ 必须剥注释：① `#` 行内注释 ② **三引号 docstring**。
        #   讲事故时我们会在 docstring 里写 `hub.call('_CLOUD_CLIENT')` 这类示例，
        #   不剥就会被当成真违规 —— 今天因此**连续两轮假阳性**（一次剥 # 不够，
        #   第二次才发现 docstring 也要剥）。别再写第三轮。
        txt = re.sub(r'#[^\n]*', '', txt)
        txt = re.sub(r'"""(?:.|\n)*?"""', '', txt)
        txt = re.sub(r"'''(?:.|\n)*?'''", '', txt)
        for m in re.finditer(r"hub\.(call|get)\(\s*'([A-Za-z_][A-Za-z0-9_]*)'", txt):
            out.setdefault(m.group(2), []).append('%s:%s' % (p.name, m.group(1)))
    return out


def resolve_imported(name: str) -> tuple[str, str]:
    """对 imported 的锚点，追到**入口 import 语句指定的那个模块**，看它是函数还是对象。

    ★ 曾经的错法：扫 lib/ 找"第一个含 def <name> 的文件" —— 那经常是**转发函数**
      （cloud_hooks.page_ctx/core_state 都定义了同名转发），于是把
      `settings.kAppVersion`（**字符串常量** '2026.08.03.1'）误判成 function
      ⇒ 判据放行了 `hub.call('kAppVersion')` ⇒ 板端 web 启动即崩
      （'str' object is not callable，2026-10-04 现场）。
    ★ 正确：先从入口源码解析出这个名字是从**哪个 lib 模块** import 的，只看那个模块。
    """
    src = entry_text()
    target = None
    for m in re.finditer(r'from\s+plugins\.web\.lib\.(\w+)\s+import\s+\(([^)]*)\)', src, re.S):
        if re.search(r'\b%s\b' % re.escape(name), m.group(2)):
            target = m.group(1)
            break
    if target is None:
        for m in re.finditer(r'from\s+plugins\.web\.lib\.(\w+)\s+import\s+([^\n(]*\b%s\b[^\n(]*)' % re.escape(name), src):
            target = m.group(1)
            break
    if target is None:
        return 'unknown', '入口 import 语句里找不到它来自哪个模块'
    f = LIB / (target + '.py')
    if not f.exists():
        return 'unknown', '模块 %s 不存在' % target
    txt = f.read_text(encoding='utf-8')
    m = re.search(r'^def\s+%s\s*\(' % re.escape(name), txt, re.M)
    if m:
        return 'function', '%s: def' % target
    m = re.search(r'^%s\s*=\s*(.*)$' % re.escape(name), txt, re.M)
    if m:
        return 'object', '%s: %s' % (target, m.group(1)[:40])
    return 'unknown', '%s 里找不到定义' % target


def main() -> int:
    rows = []
    for name, uses in sorted(all_anchors().items()):
        kind, why = classify(name)
        if kind == 'imported':
            kind2, why2 = resolve_imported(name)
            rows.append((name, kind, why, sorted(set(uses))))
            if kind2 != 'unknown':
                rows.append((name + '  →lib', kind2, why2, ''))
        else:
            rows.append((name, kind, why, sorted(set(uses))))
    w = max(len(r[0]) for r in rows) + 2
    print('%-*s %-10s %-46s %s' % (w, '锚点', '类型', '证据', '使用处'))
    for name, kind, why, uses in rows:
        u = (', '.join(uses)[:38] + '…') if len(', '.join(uses)) > 38 else ', '.join(uses)
        print('%-*s %-10s %-46s %s' % (w, name, kind, why[:46], u))
    print()
    obj = [r[0] for r in rows if r[1] == 'object']
    print('★ 对象型锚点（必须用 hub.get，绝不能用 hub.call）：%s' % ', '.join(obj))
    return 0


if __name__ == '__main__':
    sys.exit(main())
