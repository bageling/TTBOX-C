"""settings.py — web 面板的静态常量与目录派生（2026-10-02 web 换写法 S1）。

从 ``plugins/web/bin/ttbox-web.py`` 拆出。**只放"从不被打补丁"的常量**。

## 为什么不把被测试打补丁的常量也放进来

测试用 ``monkeypatch.setattr(入口模块, 'PRESETS_DIR', <tmp>)`` 把落盘目录改到临时目录
（见 ``lib/hub.py`` 顶部的打点清单）。``from settings import PRESETS_DIR`` 会在入口模块里
**新建一个绑定**，补丁只改入口那一个名字；本模块自己的全局仍是原值 ⇒ 补丁静默失效。

所以约定：``PRESETS_DIR`` / ``TEMPLATE_DIR`` 这类**可被打补丁的常量定义在入口模块**，
本模块与其它 lib 模块在**调用时**经 ``hub.get('PRESETS_DIR')`` 取。

## 值来源

根锚与目录一律经 ``lib/paths.py``（A-PATH-3/A-PATH-5），**不在本文件重写路径字面量**。
"""
from __future__ import annotations

import os
from pathlib import Path

from plugins.web.lib import hub
from plugins.web.lib import paths as ttbox_paths


# ---------------------------------------------------------------------------
# 本插件目录（= <树根>/plugins/web；systemd 单元的 TTBOX_ROOT 给同一值）
# ---------------------------------------------------------------------------
# 用 ``paths.web_dir()`` 派生（lib 的父目录）而非 ``parents[N]`` —— 与仓库层级深度解耦。
ROOT_DIR = Path(os.environ.get('TTBOX_ROOT', ttbox_paths.web_dir())).resolve()
WEB_DIR = ROOT_DIR
STATIC_DIR = WEB_DIR / 'static'

# ---------------------------------------------------------------------------
# IPC / 监听
# ---------------------------------------------------------------------------
# IPC socket 唯一真源（A-PATH-5）：TTBOX_IPC_SOCKET 环境变量 > paths.py 默认。
IPC_SOCKET = ttbox_paths.ipc_socket()

# Web 控制台监听地址与端口（V-06/V-20）：**在源码里定死**，不读任何 env。
# 端口真源 = plugins/web/lib/paths.py::WEB_PORT_DEFAULT（跨语言同值，门禁断言）；
# 面板是设备唯一入口，端口若能被子系统/界面改动，现场就会出现"面板打不开/书签失效"。
LISTEN_HOST = '0.0.0.0'
LISTEN_PORT = ttbox_paths.WEB_PORT_DEFAULT

# ---------------------------------------------------------------------------
# 领域资源
# ---------------------------------------------------------------------------
MOTION_PROFILES_DIR = Path(ttbox_paths.motion_profiles_dir())

# TTBOX 自己的 EDID 工具（完全独立于板端其它 EDID 工具）
TTBOX_HDMIRX_EDID = ttbox_paths.hdmirx_edid_tool()

# 面板对外版本（B-CONST-3：与 core kCoreVersion / release 版本三名分离）
kAppVersion = '2026.08.03.1'

# ★ T1.07b：授权面**不设本地常量**（原"恒激活"伪造常量块已删，其名字亦不再出现 ——
#   否则「grep 该名 ⇒ 0」这条门禁会被自己的注释命中而失效）。
# Web 不再持有任何授权真相，只投影 core IPC `GET_STATUS.license`（单一真相源）。


# ---------------------------------------------------------------------------
# 预设名枚举的**唯一真源**（2026-09-23）
# ---------------------------------------------------------------------------
# 坑：预设目录里除了用户预设，还会落**自动生成的非预设文件** —— 例如
#   scripts/ttbox_dtb_fix.sh 写的 DTB 诊断报告 `/opt/ttbox/presets/_dtbfix.json`
#   （故意放在这里，好让面板 `/api/presets/_dtbfix/export` 能读回）。
# 该文件由 root 生成、644、目录归 ttbox ⇒ **可读不可写**。
# 但此前三处枚举都是裸 `glob('*.json')`，把它当成了一个预设列进列表；
# 目录里又只有它一个 json ⇒ 前端把它当"当前预设"，切换模型后的自动保存就去写它
# ⇒ PermissionError ⇒ HTTP 500 ⇒ 面板显示"预设保存失败"。
# 约定：**以 `_` 开头的名字是保留名**（自动产物），不进预设列表、不允许写入/改名/删除，
# 只读（export）仍然可用。
_RESERVED_PRESET_PREFIX = '_'


def _is_preset_name(name: str) -> bool:
    """是否为**可写**的用户预设名（保留名 `_xxx` 一律否）。"""
    return bool(name) and not str(name).startswith(_RESERVED_PRESET_PREFIX)


def _preset_names() -> list:
    """列出用户预设名（保留名除外）。目录不存在时顺手建出来。

    ★ 经 ``hub`` 取 ``PRESETS_DIR``：测试会把它 patch 到临时目录（见模块头说明）。
    """
    d = Path(hub.get('PRESETS_DIR'))
    d.mkdir(parents=True, exist_ok=True)
    return sorted(p.stem for p in d.glob('*.json') if _is_preset_name(p.stem))
