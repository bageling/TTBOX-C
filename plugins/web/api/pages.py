# -*- coding: utf-8 -*-
"""api/pages.py —— 页面路由域 Blueprint。

从 bin/ttbox-web.py 搬出（2026-10-02 web 换写法 S9-a，第 2 刀）。
搬出 4 条路由，**URL 一字未改**：

    GET /            index            —— 面板引导（未激活由 before_request 302 /activate）
    GET /desktop     desktop          —— 同 '/'，带 mode=desktop
    GET /mobile      mobile           —— 同 '/'，带 mode=mobile
    GET /activate    activate_page    —— 激活页；已激活直接进面板

段外依赖处理：
    · render_template / redirect —— 直接 from flask（非补丁锚点）。
    · _page_context / _brand_template_name / _activation_ok —— 入口的实现体，
      经 hub 调用时取（测试有 monkeypatch，且它们本身还依赖入口状态）。

偏离清单：**0 处**（函数体逐行原样）。
"""

from __future__ import annotations

from flask import Blueprint, redirect, render_template

from plugins.web.lib import hub

bp = Blueprint('pages', __name__)


def _page_context(*args, **kwargs):
    """入口的 _page_context —— 调用时取。"""
    return hub.call('_page_context', *args, **kwargs)


def _brand_template_name(*args, **kwargs):
    """入口的 _brand_template_name —— 调用时取。"""
    return hub.call('_brand_template_name', *args, **kwargs)


def _activation_ok(*args, **kwargs):
    """入口的 _activation_ok —— 调用时取。"""
    return hub.call('_activation_ok', *args, **kwargs)


@bp.get('/')
def index():
    # M2.07：免密直通；未激活由 before_request 统一 302 /activate（D9）。
    return render_template(_brand_template_name('index.html'), **_page_context())


@bp.get('/desktop')
def desktop():
    # M2.07：与 '/' 同一引导（未激活 → /activate，before_request 执法）
    return render_template(_brand_template_name('index.html'),
                           mode='desktop', **_page_context())


@bp.get('/mobile')
def mobile():
    # M2.07：与 '/' 同一引导（未激活 → /activate，before_request 执法）。
    # mobile 的品牌皮肤按 yu 的做法走 templates/<brand>/mobile.html；
    # 本仓无 mobile.html（三端共用 index.html + mode 参数）。
    return render_template(_brand_template_name('index.html'),
                           mode='mobile', **_page_context())


@bp.get('/activate')
def activate_page():
    """激活页（M2.07 新增，1:1 复刻竞品暗色激活卡片）。已激活直接进面板。

    ★ 页面上下文：激活页**全品牌共用**（未激活时品牌未定，见 _brand_template_name），
      但 {{ brand_mark }} 仍取自 _page_context()（未激活 ⇒ _ui_block 落默认品牌），
      否则 Jinja Undefined 会把标识渲染成空白。故必须传 **_page_context()。
    """
    if _activation_ok():
        return redirect('/')
    return render_template('activate.html', **_page_context())
