"""_ttbox_paths.py — 根锚发现 + socket 常量（**开发机工具/测试专用，不进出货包**）。

★ V1.0.52 由来
    这些能力原先由 ``plugins/web/lib/paths.py`` 提供。该文件随「去 Python」批次 1
    被删除（业主令「去掉所有 Python 代码」）⇒ core/tests 与 core/tools 下的
    Python 守卫/诊断脚本全部 ImportError。本模块是它们的**开发侧替代品**。
    pack_manifest.txt 不含 core/ ⇒ 本文件绝不会进 OTA 包、不上板。

★ 单一真源纪律（为什么不在这里写死路径）
    socket 路径的权威定义是出货 C++ 代码 ``core/src/common/Paths.hpp``。
    本模块**不复制字面量**，而是运行时从该头文件解析 —— 否则这里就成了第二处定义，
    会被口径门禁 ①（路径字面量单点化）判为未登记的第二处来源。

★ 根锚为什么用 usbproxy 而不是 framework
    原 ``_ROOT_ANCHORS`` 含 ``framework``，该目录已随本批次删除。
    改用的 ``usbproxy`` 在**仓库根**与**板端出货树**（/opt/ttbox/current）中都存在，
    两个场景下都能唯一定位树根。
"""
from __future__ import annotations

import os
import re
from pathlib import Path

_HPP_REL = ("core", "src", "common", "Paths.hpp")

# 根锚：同时含以下四个目录的最近一层即树根。
# ★ 不用 parents[N] / "../.." —— 口径门禁 ⑩ 明令禁止（换布局即静默指错根）。
_ANCHORS = ("plugins", "usbproxy", "scripts", "deploy")


def discover_root(start) -> str:
    """从 ``start`` 逐级向上找树根（最近一层同时含全部 ``_ANCHORS`` 的目录）。

    找不到就抛 ``RuntimeError`` —— **不允许静默回退**（回退到 ``.`` 或某一上层目录
    都会指错根，而且无声；宁可在调用点炸掉，也不要让下游读错文件）。
    """
    cur = Path(start).resolve()
    if cur.is_file():
        cur = cur.parent
    while True:
        if all((cur / _n).is_dir() for _n in _ANCHORS):
            return str(cur)
        if cur.parent == cur:
            raise RuntimeError(
                "找不到 TTBOX 树根：从 %s 向上未发现同时含 %s 的目录"
                % (start, "/".join(_ANCHORS))
            )
        cur = cur.parent


def _paths_hpp() -> Path:
    """定位 core/src/common/Paths.hpp（同样走根锚发现，不写死层级）。"""
    cur = Path(__file__).resolve().parent
    while True:
        if all((cur / _n).is_dir() for _n in _ANCHORS):
            return cur.joinpath(*_HPP_REL)
        if cur.parent == cur:
            raise RuntimeError("找不到 TTBOX 树根（解析 %s 用）" % "/".join(_HPP_REL))
        cur = cur.parent


def _const(name: str) -> str:
    text = _paths_hpp().read_text(encoding="utf-8", errors="ignore")
    m = re.search(r'\b' + re.escape(name) + r'\s*=\s*"([^"]*)"', text)
    if m is None:
        raise RuntimeError("core/src/common/Paths.hpp 缺少常量 %s" % name)
    return m.group(1)


IPC_SOCKET_DEFAULT = _const("kIpcSocketDefault")
MOUSE_CMD_SOCK_DEFAULT = _const("kMouseCmdSocketDefault")
MOUSE_EVENT_SOCK_DEFAULT = _const("kMouseEventSocketDefault")


def ipc_socket() -> str:
    """IPC socket 路径：``TTBOX_IPC_SOCKET`` 优先，否则默认值。

    保留函数形态以兼容原 ``paths.ipc_socket()`` 调用点。
    """
    return os.environ.get("TTBOX_IPC_SOCKET", IPC_SOCKET_DEFAULT)
