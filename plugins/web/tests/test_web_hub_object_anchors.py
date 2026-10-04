# -*- coding: utf-8 -*-
"""web 换写法（hub 转发）的两条硬口径 —— 都是**板端实炸过**的。

★ 本文件的价值：这两条都曾让 **web 服务起不来**，进而让「面板升级按钮」和
  「手动装版」双双失效，形成"不修就升不上、升不上就修不了"的死锁
  （2026-10-04 18:44 现场）。本机 pytest 全绿也没拦住 —— 因为**测试 stub 与实现
  一起错**（见 test_web_hub_forwarder_runtime.py 的注释）。
"""
from __future__ import annotations

import re
from pathlib import Path

import pytest

from plugins.web.lib.paths import discover_root

REPO = Path(discover_root(__file__))
ENTRY = REPO / 'plugins' / 'web' / 'bin' / 'ttbox-web.py'
CLOUD_HOOKS = REPO / 'plugins' / 'web' / 'lib' / 'cloud_hooks.py'


def _src(p: Path) -> str:
    return p.read_text(encoding='utf-8')


# ★★ 口径 1：对象型锚点只能用 hub.get（取值），绝不能 hub.call（调用）
OBJECT_ANCHORS = {
    '_CLOUD_CLIENT': 'CloudLicenseClient(...) 单例',
    '_CLOUD_SESSION': 'CloudSessionStore(...) 单例',
    '_ACTIVATION_CACHE': 'dict（缓存状态）',
    '_HEARTBEAT_START_LOCK': 'threading.Lock() 实例',
}


def _anchors_in_entry(name: str) -> bool:
    """确认该锚点在入口里确实是**对象赋值**（不是 def）。"""
    m = re.search(r'^%s\s*=\s*(?!def)' % re.escape(name), _src(ENTRY), re.M)
    return bool(m)


def test_object_anchors_are_really_objects_in_entry():
    """前提自检：这 4 个锚点在入口里必须是对象赋值 —— 口径 1 的判据依赖它。"""
    for name, desc in OBJECT_ANCHORS.items():
        assert _anchors_in_entry(name), (
            '入口里找不到 %s 的对象赋值（%s）—— 若它已改成函数，本护栏的判据要重写'
            % (name, desc))


def test_object_anchors_never_use_hub_call():
    """★★ 板端实炸：`hub.call('_CLOUD_CLIENT')` → 调用对象 → TypeError
    ⇒ web 启动崩溃 ⇒ 面板升级按钮失效 + 手动装版过不了健康检查 = 死锁。"""
    hooks = _src(CLOUD_HOOKS)
    body = hooks[hooks.index('OBJECT_ANCHORS' if 'OBJECT_ANCHORS' in hooks else '# ══'):]
    for name in OBJECT_ANCHORS:
        bad = re.search(r"return\s+hub\.call\(\s*'%s'" % re.escape(name), body)
        assert not bad, (
            'cloud_hooks 用 hub.call 取 %s —— 它在入口里是**对象**（%s），'
            '必须用 hub.get；hub.call 是「调用」语义，对象不可调用'
            % (name, OBJECT_ANCHORS[name]))


def test_object_anchors_use_hub_get():
    """正向钉住：4 个对象型锚点都必须走 hub.get。"""
    hooks = _src(CLOUD_HOOKS)
    for name in OBJECT_ANCHORS:
        assert re.search(r"return\s+hub\.get\(\s*'%s'" % re.escape(name), hooks), (
            '%s 的转发应使用 hub.get（取值语义）' % name)


# ★ 口径 2：传给构造函数的值必须是值，不是转发函数本身
def test_heartbeat_worker_receives_values_not_forwarders():
    """构造参数里不能出现转发函数**本身**（否则 worker 里 self._session.load() 炸）。"""
    hooks = _src(CLOUD_HOOKS)
    m = re.search(r'worker\s*=\s*HeartbeatWorker\((.*?)\n\s*\)', hooks, re.S)
    assert m, '找不到 HeartbeatWorker(...) 构造调用'
    args = m.group(1)
    # 形如 `_CLOUD_CLIENT(), _CLOUD_SESSION(),` —— 必须带括号（是调用）
    for name in ('_CLOUD_CLIENT', '_CLOUD_SESSION'):
        # 允许 `name()` / `name( )` / `name(  )` 等写法（★ 只看"紧跟右括号"，
        # 前面用 (?<![\w.]) 排除 `hub.get('_CLOUD_CLIENT')` 里的字符串命中）
        # 「标识符 + 紧跟左括号」就足以区分调用与字符串命中
        # （`'_CLOUD_CLIENT'` 后面是引号不是括号）
        called = re.search(r'(?<![\w.])%s\s*\(' % re.escape(name), args)
        assert called, '构造参数里应有 %s（且是**调用**转发函数的形式）' % name
    for name in ('kAppVersion', '_machine_code'):
        assert re.search(r'%s\(\)' % re.escape(name), args), (
            '%s 在构造参数里应已调用（取到值）' % name)
