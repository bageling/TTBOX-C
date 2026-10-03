# -*- coding: utf-8 -*-
"""sysutil —— 从 bin/ttbox-web.py 原样搬出（2026-10-02 web 换写法 S10 第一步）。

系统基建：静态资源判断、凭据退休。
这一簇全是**无状态小函数**（≤ 18 行），不碰 IPC 不碰硬件。

★  **留在入口**：它在 L34 被 
  模块级立即调用（import 期就要结果），搬进 lib 就得等 hub.bind 之后，
  而 _TREE_ROOT 又被别的模块级代码用 ⇒ 只能在入口算。

★ `WEB_CREDENTIALS_PATH` 是入口常量（含板端凭据文件路径）
  ⇒ 经 hub 取，不在 lib 里复制一份（避免路径双真源）。
★ `ttbox_logging` = lib/logging_setup 的别名，直接 import 即可；
  `_LOG` 是入口用get_logger() 建好的 logger 实例，经 hub 取（同一实例）。
搬出 2 个函数：_is_static / _retire_web_credentials

★ 本模块在 lib/，**不能 import 入口** ⇒ 段外依赖一律经 hub 调用时取。
"""

from __future__ import annotations

import os
from pathlib import Path

from plugins.web.lib import hub
from plugins.web.lib import logging_setup as ttbox_logging

def WEB_CREDENTIALS_PATH():
    """入口的 WEB_CREDENTIALS_PATH —— 调用时取（常量 / 单例，必须与入口**同一个对象**）。"""
    return hub.get('WEB_CREDENTIALS_PATH')


def _LOG():
    """入口的 _LOG —— 调用时取（常量 / 单例，必须与入口**同一个对象**）。"""
    return hub.get('_LOG')

def _is_static(path: str) -> bool:
    """**纯静态资源**放行（§2.3：/static/*、/favicon.ico）。

    ★ 页面路由不在此函数内放行（历史上误将 _PAGE_ROUTES 并入本函数 ⇒ 未激活跳转
      /activate 的分支永不达，页面恒 200）。页面路由交由 _enforce_gate 依激活态处理。
    ★ 接管加固（fail-closed）：含"上跳段(..)"、反斜杠、或**百分号编码**的点/斜杠/
      反斜杠（%2e / %2f / %5c）的路径**一律不认**，交回入口 gate（未激活 403 /
      已激活交 Flask safe_join 兜底）。理由：`path.startswith('/static/')` 是前缀匹配，
      `/static/../api/state` 这类路径会绕过唯一执法点。收紧后变形路径恒 False。
      合法性：真实静态资源 URL 不含 `..` 段、`\\`、或编码分隔符，故不误伤
      （实测 /static/*.js|css 仍 200）。
    """
    low = path.lower()
    if ('\\' in path or '..' in path.split('/')
            or '%2e' in low or '%2f' in low or '%5c' in low):
        return False
    return (path == '/favicon.ico'
            or path.startswith('/static/'))


def _retire_web_credentials() -> None:
    """web_credentials.json 退役：改名 .retired（D10）。失败仅记日志，不阻断启动。"""
    try:
        if os.path.exists(WEB_CREDENTIALS_PATH()):
            os.replace(WEB_CREDENTIALS_PATH(), WEB_CREDENTIALS_PATH() + '.retired')
            _LOG().info('已退役旧凭据文件 -> %s.retired', WEB_CREDENTIALS_PATH())
            ttbox_logging.log_operation('retire-web-credentials')  # §5.2 客户侧数据迁移，留痕
    except OSError as e:
        _LOG().warning('凭据退役失败（忽略，不阻断启动）: %s', e)
