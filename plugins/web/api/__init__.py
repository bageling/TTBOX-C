# -*- coding: utf-8 -*-
"""plugins/web/api/ —— 按域拆分的 Flask Blueprint（2026-10-02 web 换写法 S9）。

为什么现在建它：
    入口 `bin/ttbox-web.py` 已从 6099 行降到 3175 行，剩下的最大一块是
    89 个路由/钩子（约 1400 行）。它们按域聚成 15 个文件后，
    加功能 = 新增一个域文件，而不是改一个 3000 行的大文件。

约定（铁律，改之前先读）：
    · **URL 一字不改**。所有路由仍写完整路径（`@bp.get('/api/xxx')`），
      不用 url_prefix —— 少一层前缀拼接，就少一处可能拼错的可能。
      校验工具：scripts/check_route_snapshot.py（S9 安全网，89 条逐条比对）。
    · 每个域文件顶部必须写「本域搬出的路由清单 + 段外依赖处理方式」。
    · 域文件不互相 import；共享能力走 plugins.web.lib。
    · 入口保留对这些视图函数的 re-export（补丁锚点与测试依赖，见硬约束详述.md）。

注册方式：`api/__init__.py::register_blueprints(app)`，由入口调用一次。
"""

from __future__ import annotations

__all__ = ['register_blueprints']


def register_blueprints(app):
    """把所有域 Blueprint 注册到 app。入口只调这一个函数。"""
    from plugins.web.api import brand, pages, motion, diagnostics, preview, control, aim, calib, presets, state, license, ota, models, hardware, system
    for bp in (brand.bp, pages.bp, motion.bp, diagnostics.bp, preview.bp, control.bp, aim.bp, calib.bp, presets.bp, state.bp, license.bp, ota.bp, models.bp, hardware.bp, system.bp):
        app.register_blueprint(bp)
    return app
