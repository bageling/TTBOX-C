# -*- coding: utf-8 -*-
"""lib/ 模块的「hub 转发函数当值用」+ **启动路径真跑** —— 2026-10-03 板端两次事故配套。

★★ 事故记录（都是真板端崩溃，本机 pytest 全绿）：
  ① cloud_hooks._ensure_heartbeat_worker
       with _HEARTBEAT_START_LOCK:      ← 少了括号
       AttributeError: __enter__
  ② 同函数下一行
       if _HEARTBEAT is not None ...   ← global 指向本模块不存在的全局
       NameError: name '_HEARTBEAT' is not defined
  ③ _invalidate_activation_cache
       _ACTIVATION_CACHE['ts'] = 0.0   ← 给转发函数下标赋值
       TypeError: 'function' object does not support item assignment
  三次都是同一类：**转发函数（函数）被当成它所转发的对象（锁/字典/句柄）用**。

★ 为什么本机测不出来：
  这几个函数只在**真实启动路径**跑（main() → _ensure_heartbeat_worker()），
  没有任何测试真的调用过它们。
  ⇒ 本文件的核心不是静态检查，是**真的把它们跑一遍**。

判据：
  A) AST 扫全部 lib 模块：转发函数不得出现在「值位置」
     （下标 / 取属性 / 赋值目标 / for 迭代）
  B) 真跑：hub.bind 一个假入口，逐个调用 cloud_hooks 的**业务函数**
     （不是转发函数本身），任何异常即失败
"""
from __future__ import annotations

import ast
import sys
import threading
import types

import pytest

from plugins.web.tests import panel_src

WEB_DIR = panel_src.STATIC_DIR.parent
LIB_DIR = WEB_DIR / 'lib'


# ---------------------------------------------------------------- A) 静态

def _forwarders_of(tree) -> set:
    out = set()
    for node in tree.body:
        if isinstance(node, ast.FunctionDef):
            a = node.args
            if a.vararg and a.kwarg and not a.args and not a.kwonlyargs:
                src = ast.dump(node)
                if 'hub.call' in src or 'hub.get' in src:
                    out.add(node.name)
    return out


def test_no_forwarder_in_value_position():
    """转发函数不得被当下标 / 属性 / 赋值目标 / 迭代对象。"""
    problems = []
    for p in sorted(LIB_DIR.glob('*.py')):
        tree = ast.parse(p.read_text(encoding='utf-8'))
        fwd = _forwarders_of(tree)
        if not fwd:
            continue
        for node in ast.walk(tree):
            if isinstance(node, ast.Subscript) and isinstance(node.value, ast.Name) \
                    and node.value.id in fwd:
                problems.append('%s:%d %s[...] 当字典/列表用'
                                % (p.name, node.lineno, node.value.id))
            if isinstance(node, ast.Attribute) and isinstance(node.value, ast.Name) \
                    and node.value.id in fwd:
                problems.append('%s:%d %s.%s 取属性（函数对象没这属性）'
                                % (p.name, node.lineno, node.value.id, node.attr))
            if isinstance(node, ast.AugAssign) and isinstance(node.target, ast.Name) \
                    and node.target.id in fwd:
                problems.append('%s:%d %s 作为赋值目标'
                                % (p.name, node.lineno, node.target.id))
            if isinstance(node, (ast.For, ast.AsyncFor)) and isinstance(node.iter, ast.Name) \
                    and node.iter.id in fwd:
                problems.append('%s:%d for ... in %s' % (p.name, node.lineno, node.iter.id))
    assert not problems, (
        '发现 %d 处把 hub 转发函数当「对象」用（板端会 AttributeError/TypeError 崩）：\n  %s\n'
        '  ⇒ 要用真对象时改经 hub.get(name) 取，再操作。'
        % (len(problems), '\n  '.join(problems)))


# ---------------------------------------------------------------- B) 真跑

class _FakeWorker:
    def __init__(self, *a, **k):
        self._running = False
        self.args = a
        self.kw = k

    def running(self):
        return self._running

    def start(self):
        self._running = True


def _fake_entry():
    return types.SimpleNamespace(
        _HEARTBEAT=None,
        _HEARTBEAT_START_LOCK=threading.Lock(),
        _CLOUD_CLIENT='CLIENT',
        _CLOUD_SESSION='SESSION',
        _cloud_deactivate_callback=lambda: None,
        _invalidate_activation_cache=lambda: None,
        kAppVersion='V1.0.18',
        _machine_code='MACHINE',
        _ACTIVATION_CACHE={'ts': 1.0},
        ipc_request=lambda *a, **k: {'ok': True},
        HeartbeatWorker=_FakeWorker,
    )


@pytest.fixture()
def wired_cloud_hooks():
    """把 cloud_hooks 接到一个假入口上，返回 (module, entry_namespace)。"""
    sys.path.insert(0, str(WEB_DIR.parent.parent))
    from plugins.web.lib import hub
    ns = _fake_entry()
    hub.bind(ns)
    for mod in list(sys.modules):
        if mod.endswith('cloud_hooks'):
            del sys.modules[mod]
    from plugins.web.lib import cloud_hooks
    return cloud_hooks, ns


def test_ensure_heartbeat_worker_actually_runs(wired_cloud_hooks):
    """★ 事故 ①② 的直接回归：真跑 _ensure_heartbeat_worker。"""
    mod, ns = wired_cloud_hooks
    mod._ensure_heartbeat_worker()
    assert isinstance(ns._HEARTBEAT, _FakeWorker), (
        '_HEARTBEAT 没被写进入口模块（应经 hub.setattr）')
    assert ns._HEARTBEAT.running() is True
    # 幂等：第二次不应再建一个
    first = ns._HEARTBEAT
    mod._ensure_heartbeat_worker()
    assert ns._HEARTBEAT is first, '_ensure_heartbeat_worker 不幂等'


def test_invalidate_activation_cache_actually_runs(wired_cloud_hooks):
    """★ 事故 ③ 的直接回归：真跑 _invalidate_activation_cache。"""
    mod, ns = wired_cloud_hooks
    ns._ACTIVATION_CACHE['ts'] = 123.0
    mod._invalidate_activation_cache()
    assert ns._ACTIVATION_CACHE['ts'] == 0.0


def test_cloud_deactivate_callback_actually_runs(wired_cloud_hooks):
    """★ _cloud_deactivate_callback 也会走缓存失效路径。"""
    mod, ns = wired_cloud_hooks
    ns._ACTIVATION_CACHE['ts'] = 55.0
    mod._cloud_deactivate_callback()
    assert ns._ACTIVATION_CACHE['ts'] == 0.0


def test_every_public_cloud_hooks_function_runs(wired_cloud_hooks):
    """★ 全量真跑：cloud_hooks 里每个**业务函数**都必须能跑通。

    转发函数（`*_CLIENT` 之类）本身要传参、不算业务函数，单独排除
    （它们是给别的模块当锚点用的）。
    """
    mod, ns = wired_cloud_hooks
    failures = []
    for name in sorted(dir(mod)):
        if name.startswith('__'):
            continue
        obj = getattr(mod, name)
        if not callable(obj) or isinstance(obj, type):
            continue
        if name in ('_heartbeat_start_lock',):
            continue
        # 跳过纯转发（无下划线开头但 docstring 说明是锚点的也跳过）
        doc = (getattr(obj, '__doc__', '') or '')
        if '转发须' in doc or 'monkeypatch 锚点' in doc:
            continue
        try:
            obj()
        except TypeError as e:
            # 需要必填参数的属业务函数签名，跳过（不是崩溃）
            if 'positional argument' in str(e) or 'required' in str(e):
                continue
            failures.append('%s: TypeError %s' % (name, e))
        except Exception as e:
            failures.append('%s: %s %s' % (name, type(e).__name__, e))
    assert not failures, (
        'cloud_hooks 有函数跑不通（板端启动即崩）：\n  %s\n'
        '  ⇒ 静态检查抓不到这类问题，必须真跑。' % '\n  '.join(failures))
