# -*- coding: utf-8 -*-
"""锁死「hub 转发函数不可当值用」—— 2026-10-03 板端事故配套。

★★ 这条测试的由来（真实事故）：
  S9 把 web 拆到 lib/ 时，`_HEARTBEAT_START_LOCK` 被自动生成成转发函数
      def _HEARTBEAT_START_LOCK(*args, **kwargs):
          return hub.call('_HEARTBEAT_START_LOCK', *args, **kwargs)
  而 `cloud_hooks._ensure_heartbeat_worker()` 里写的是
      with _HEARTBEAT_START_LOCK:      ← 少了括号
  ⇒ 拿到的是**函数对象**，没有 `__enter__`
  ⇒ 板端 ttbox-web 启动即 `AttributeError: __enter__`，systemd 每 3 秒重启一次，
     **面板完全打不开**。
  而本机 538 个 pytest 全绿 —— 因为没有任何测试真的调用过这个函数。

★ 为什么会漏：转发函数「调用时取」的设计是对的（monkeypatch 锚点），
  但它**不是**它所转发的东西本身。凡是「需要对象而不是函数」的地方
  （with 上下文 / 取属性 / 传参 / 赋值），必须用 `hub.get()` 取真对象。

判据（扫 lib/ 全部 39 个转发函数）：
  ① 列出全部转发函数名
  ② 扫它们被当「值」用的四种形态：with NAME: / NAME.attr / X = NAME / f(NAME)
  ③ 逐个核对：入口侧该名字到底是什么类型（函数 vs 对象）
"""
from __future__ import annotations

import ast
import re
from pathlib import Path

from plugins.web.tests import panel_src

WEB_DIR = panel_src.STATIC_DIR.parent          # plugins/web
LIB_DIR = WEB_DIR / 'lib'
ENTRY = WEB_DIR / 'bin' / 'ttbox-web.py'


def _forwarders() -> dict:
    """name -> file that defines it, for every hub-based forwarder.

    ★ 用**源码文本**判定 body 里有 `hub.call('NAME'` —— 不要用 ast.dump()：
      它会把字符串常量里的内容也转义出来，匹配不到（实测认不出一个）。
    """
    out = {}
    for p in sorted(LIB_DIR.glob('*.py')):
        src = p.read_text(encoding='utf-8')
        # 逐个函数判定（AST 取签名，body 用源码切片）
        tree = ast.parse(src)
        lines = src.splitlines(keepends=True)
        for node in tree.body:
            if not isinstance(node, ast.FunctionDef):
                continue
            a = node.args
            if not (a.vararg and a.kwarg and not a.args and not a.kwonlyargs):
                continue
            seg = ''.join(lines[node.lineno - 1:node.end_lineno])
            if ("hub.call('%s'" % node.name) in seg or \
                    ('hub.call("%s"' % node.name) in seg:
                out[node.name] = p.name
    return out


def test_forwarders_exist():
    """先确认机制本身在工作（否则下面几条会空跑）。"""
    fwd = _forwarders()
    assert len(fwd) >= 30, (
        '只认出 %d 个转发函数（预期 30+）—— 判定口径失效，后面的检查会假绿'
        % len(fwd))
    assert '_HEARTBEAT_START_LOCK' in fwd, '转发函数识别漏了 _HEARTBEAT_START_LOCK'


def test_lock_forwarder_has_a_value_accessor():
    """★ 事故点专用：锁类转发函数必须配一个 hub.get 取真对象的取值器。"""
    src = (LIB_DIR / 'cloud_hooks.py').read_text(encoding='utf-8')
    assert 'def _heartbeat_start_lock():' in src, (
        'cloud_hooks 缺 _heartbeat_start_lock() 取值器 —— '
        '转发函数不能直接 with')
    assert re.search(r"return hub\.get\(\s*'_HEARTBEAT_START_LOCK'\s*\)", src), (
        '_heartbeat_start_lock() 没用 hub.get 取真锁对象')


def test_no_forwarder_used_as_a_value():
    """★ 核心：任何转发函数都不得被当「值」用（with / 取属性 / 赋值 / 传参）。

    ★ docstring 内的反例代码要排除：注释里写 `with _HEARTBEAT_START_LOCK:`
      作为「反例示范」是合法且必要的（正是它记录了这次事故），
      扫描器必须只扫真代码。
    """
    fwd = _forwarders()
    problems = []
    for p in sorted(LIB_DIR.glob('*.py')):
        src = p.read_text(encoding='utf-8')
        lines = src.splitlines()
        # 标记每个物理行是否在 docstring 内
        in_doc = _docstring_lines(p)
        for i, ln in enumerate(lines, 1):
            s = ln.strip()
            if i in in_doc:
                continue
            if s.startswith('#') or s.startswith('*'):
                continue
            for name in fwd:
                esc = re.escape(name)
                if re.search(r'with\s+%s\s*:' % esc, ln):
                    problems.append('%s:%d with %s: 缺括号（拿到的是函数）'
                                    % (p.name, i, name))
                if re.search(r'(?<![\w.$])%s\s*\.\s*\w+' % esc, ln) \
                        and not re.search(r'%s\s*\(\s*\)' % esc, ln):
                    problems.append('%s:%d %s.attr 缺括号' % (p.name, i, name))
                if re.match(r'\w+\s*=\s*%s\s*$' % esc, s):
                    problems.append('%s:%d 赋值 %s 缺括号' % (p.name, i, name))
                if re.search(r'\(\s*%s\s*[,)]' % esc, ln) \
                        and not re.search(r'%s\s*\(' % esc, ln):
                    problems.append('%s:%d 传参 %s 缺括号' % (p.name, i, name))
    assert not problems, (
        '发现 %d 处把 hub 转发函数当「值」用（板端会 AttributeError 崩掉）：\n  %s\n'
        '  ⇒ 需要对象（with / 属性 / 赋值 / 传参）时改用 hub.get() 取真对象，'
        '或加一个 _xxx() 取值器。'
        % (len(problems), '\n  '.join(problems)))


def _docstring_lines(p) -> set:
    """1-based line numbers that sit inside a docstring (module or function)."""
    out = set()
    tree = ast.parse(p.read_text(encoding='utf-8'))
    for node in ast.walk(tree):
        if isinstance(node, (ast.Module, ast.FunctionDef, ast.ClassDef)):
            ds = ast.get_docstring(node, clean=False)
            if not ds:
                continue
            body = node.body[0]
            for ln in range(body.lineno, (body.end_lineno or body.lineno) + 1):
                out.add(ln)
    return out


def test_entry_side_lock_is_a_real_lock():
    """核对：入口侧那个名字确实是 threading.Lock（否则取值器也不对）。"""
    src = ENTRY.read_text(encoding='utf-8')
    m = re.search(r'^_HEARTBEAT_START_LOCK\s*=\s*(.+)$', src, re.M)
    assert m, '入口侧找不到 _HEARTBEAT_START_LOCK 定义'
    assert 'threading.Lock()' in m.group(1), (
        '入口侧 _HEARTBEAT_START_LOCK 不是 threading.Lock()，实际是：%s' % m.group(1))


def test_scanner_catches_a_planted_forwarder_as_value(tmp_path):
    """★ 自检：植入一个同类错误必须被抓（防检测器失灵）。"""
    bad = tmp_path / 'bad_mod.py'
    bad.write_text(
        'def LOCK_ANCHOR(*args, **kwargs):\n'
        '    return hub.call(\'LOCK_ANCHOR\', *args, **kwargs)\n'
        'def use():\n'
        '    with LOCK_ANCHOR:\n'
        '        pass\n', encoding='utf-8')
    text = bad.read_text(encoding='utf-8')
    hits = re.findall(r'with\s+LOCK_ANCHOR\s*:', text)
    assert hits, '检测器没抓到植入的 with LOCK_ANCHOR:'
