# -*- coding: utf-8 -*-
"""cloud_hooks —— 从 bin/ttbox-web.py 原样搬出（2026-10-02 web 换写法 S10 第一步）。

云端授权相关的小钩子：设备停用回调、心跳 worker 拉起、授权缓存失效。
★ 它们的实现体调用入口的 `ipc_request` 等锚点 ⇒ 经 hub 取。
搬出 3 个函数：_cloud_deactivate_callback / _ensure_heartbeat_worker / _invalidate_activation_cache

★ 本模块在 lib/，**不能 import 入口** ⇒ 段外依赖一律经 hub 调用时取。
"""

from __future__ import annotations

from plugins.web.lib import hub

def HeartbeatWorker(*args, **kwargs):
    """入口的 HeartbeatWorker —— 调用时取（monkeypatch 锚点，转发须**原样透传**）。"""
    return hub.call('HeartbeatWorker', *args, **kwargs)


def _ACTIVATION_CACHE(*args, **kwargs):
    """入口的 _ACTIVATION_CACHE —— 调用时取（monkeypatch 锚点，转发须**原样透传**）。"""
    return hub.call('_ACTIVATION_CACHE', *args, **kwargs)


def _CLOUD_CLIENT(*args, **kwargs):
    """入口的 _CLOUD_CLIENT —— 调用时取（monkeypatch 锚点，转发须**原样透传**）。"""
    return hub.call('_CLOUD_CLIENT', *args, **kwargs)


def _CLOUD_SESSION(*args, **kwargs):
    """入口的 _CLOUD_SESSION —— 调用时取（monkeypatch 锚点，转发须**原样透传**）。"""
    return hub.call('_CLOUD_SESSION', *args, **kwargs)


def _HEARTBEAT_START_LOCK(*args, **kwargs):
    """入口的 _HEARTBEAT_START_LOCK —— 调用时取（monkeypatch 锚点，转发须**原样透传**）。

    ★★ 只用于「把它当函数调」的场合。**锁对象请用 `_heartbeat_start_lock()`**：
       入口侧是真`threading.Lock()`，而本函数是转发函数 —— `with _HEARTBEAT_START_LOCK:`
       拿到的是函数对象、没有 `__enter__` ⇒ 启动即崩
       （2026-10-03 板端实测：AttributeError: __enter__，web 服务重启循环）。
    """
    return hub.call('_HEARTBEAT_START_LOCK', *args, **kwargs)


def _heartbeat_start_lock():
    """取真正的锁对象（`threading.Lock` 实例），供 `with` 使用。"""
    return hub.get('_HEARTBEAT_START_LOCK')


def _cloud_deactivate_callback(*args, **kwargs):
    """入口的 _cloud_deactivate_callback —— 调用时取（monkeypatch 锚点，转发须**原样透传**）。"""
    return hub.call('_cloud_deactivate_callback', *args, **kwargs)


def _invalidate_activation_cache(*args, **kwargs):
    """入口的 _invalidate_activation_cache —— 调用时取（monkeypatch 锚点，转发须**原样透传**）。"""
    return hub.call('_invalidate_activation_cache', *args, **kwargs)


def _machine_code(*args, **kwargs):
    """入口的 _machine_code —— 调用时取（monkeypatch 锚点，转发须**原样透传**）。"""
    return hub.call('_machine_code', *args, **kwargs)


def ipc_request(*args, **kwargs):
    """入口的 ipc_request —— 调用时取（monkeypatch 锚点，转发须**原样透传**）。"""
    return hub.call('ipc_request', *args, **kwargs)


def kAppVersion(*args, **kwargs):
    """入口的 kAppVersion —— 调用时取（monkeypatch 锚点，转发须**原样透传**）。"""
    return hub.call('kAppVersion', *args, **kwargs)

def _cloud_deactivate_callback() -> None:
    """云端 403（到期/禁用）⇒ 快路径：IPC ACTIVATE_CLOUD{deactivate:true} 让 core 立即锁定。

    注意：发布到 LicenseGate 由 core 完成（本层不碰授权真相）；本回调只触发并清缓存。
    """
    ipc_request('ACTIVATE_CLOUD', {
        'deactivate': True,
        'source': 'cloud',
        'reason': '云端返回403（卡密到期或已禁用）',
    }, timeout=5)
    _invalidate_activation_cache()


def _ensure_heartbeat_worker() -> None:
    """心跳线程幂等拉起（激活成功 / web 启动时恢复会话后调用）。

    ★★ 心跳句柄是**入口的模块级变量**（`ttbox-web.py::_HEARTBEAT`），
       本模块没有它的全局 —— 写`global _HEARTBEAT` 会NameError
       （2026-10-03 板端实测：NameError: name '_HEARTBEAT' is not defined，
        web 服务重启循环）。必须经 hub.get 读、setattr 写。
    """
    with _heartbeat_start_lock():
        current = hub.get('_HEARTBEAT')
        if current is not None and current.running():
            return
        worker = HeartbeatWorker(
            # ★ 这三个都是本模块的 **lazy 转发函数**（经 hub.call 取入口的对象），
            #   必须**调用**它们拿实例，不能把函数本身当参数/值传过去。
            #   漏括号的后果（2026-10-04 板端 journalctl 实测）：
            #     · session 收到函数 ⇒ worker 里 self._session.load() 抛
            #       「'function' object has no attribute 'load'」⇒ **心跳线程一启动就死**，
            #       /api/license 的 cloud.heartbeat 永远 online=false、last_ok_at=0；
            #     · client_version / machine_code 收到函数 ⇒ 日志与版本号全错。
            #   入口的 _CLOUD_CLIENT/_CLOUD_SESSION 是**模块级单例**（ttbox-web.py:645/647），
            #   所以每次调用拿到的是同一个对象，心跳与 /api/license 读同一份会话。
            _CLOUD_CLIENT(), _CLOUD_SESSION(),
            on_expired=_cloud_deactivate_callback,
            client_version=kAppVersion(),
            machine_code=_machine_code(),
        )
        # ★ 走 hub.set_ 而不是 setattr(hub.entry(), ...)：hub 绑定的是入口的
        #   __dict__（dict），不是模块对象 ⇒ setattr 对 dict 会 AttributeError。
        #   这正是上一轮板端崩溃那类"把转发对象当对象用"的同族错误。
        hub.set_('_HEARTBEAT', worker)
        worker.start()


def _invalidate_activation_cache() -> None:
    """激活/失活后立即失效缓存（使 gate 与页面引导即时翻转）。

    ★ 缓存字典是**入口的模块级变量**（`ttbox-web.py::_ACTIVATION_CACHE`），
      写 `_ACTIVATION_CACHE['ts'] = 0` 是在给**转发函数**下标赋值
      （2026-10-03 板端同类事故：TypeError: 'function' object does not
      support item assignment）。必须经 hub.get 取真字典。
    """
    hub.get('_ACTIVATION_CACHE')['ts'] = 0.0
