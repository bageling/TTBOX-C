# -*- coding: utf-8 -*-
"""模型导入事务锁 —— 从 bin/ttbox-web.py 原样搬出（2026-10-02 web 换写法 S8 第一刀）。

搬出的内容（入口 L1470-1511，含上方 10 行背景注释，逐行未改）：
    _MODEL_IMPORT_EVENTS / _MODEL_IMPORT_EVENTS_LOCK   —— 事务锁状态
    _begin_import / _end_import / _is_importing / _wait_import_done

为什么整段搬（含两个模块级变量）：
    这是一份**自包含状态机** —— 事件表 + 锁只被这4 个函数用，
    不与入口其他代码交叉。变量跟段搬走，入口经re-export 访问的是**同一份对象**，
    因此 lib 侧登记的事务，入口读得到（反之亦然）。

★ 偏离清单：**0 处**。段外只要 threading（直接 import）。

★ 补丁穿透说明：`_is_importing` 已被 lib/state_snapshot.py 经 hub 转发
  （那里查『某模型是否在导入中』）。本模块是它的**实现体**，
  入口 re-export 后 hub 转发的目标即本模块的定义 —— 身份一致，无第二份。
"""

from __future__ import annotations

import threading



# ── 模型导入事务锁（1.5.61：修 "上传后立刻切换 → MODEL_NOT_FOUND"）──────────
# 背景：Core 的 IPC 是「每连接一线程」，导入链 IMPORT→VALIDATE→INSTALL 三次 IPC
#   之间是锁空闲窗口。此刻到达的 MODEL_ACTIVATE 会抢在 INSTALL 前面跑，而
#   installed/<id> 还没建出来（staging 不在 ModelRegistry 的搜索路径里）⇒
#   必然回裸 MODEL_NOT_FOUND。过一会再点又好了——就是用户看到的现象。
# 修法：web 侧把「正在导入」的 model_id 登记进事件表；/api/models/select 命中时
#   有界等待导入结束再 ACTIVATE（不报错）。**不能**在 Core 的 activate 里重试——
#   activate 与 install 共用 registry mutex_，重试期间持锁会把 install 挡在外面。
_MODEL_IMPORT_EVENTS: dict = {}
_MODEL_IMPORT_EVENTS_LOCK = threading.Lock()


def _begin_import(model_id: str):
    with _MODEL_IMPORT_EVENTS_LOCK:
        ev = _MODEL_IMPORT_EVENTS.get(model_id)
        if ev is None:
            ev = threading.Event()
            _MODEL_IMPORT_EVENTS[model_id] = ev
        return ev


def _end_import(model_id: str):
    with _MODEL_IMPORT_EVENTS_LOCK:
        ev = _MODEL_IMPORT_EVENTS.pop(model_id, None)
    if ev is not None:
        ev.set()


def _is_importing(model_id: str) -> bool:
    with _MODEL_IMPORT_EVENTS_LOCK:
        return model_id in _MODEL_IMPORT_EVENTS


def _wait_import_done(model_id: str, timeout: float) -> bool:
    """等该模型的导入事务结束。未在导入中 ⇒ 立返 True；超时 ⇒ False。"""
    with _MODEL_IMPORT_EVENTS_LOCK:
        ev = _MODEL_IMPORT_EVENTS.get(model_id)
    if ev is None:
        return True
    return ev.wait(timeout)
