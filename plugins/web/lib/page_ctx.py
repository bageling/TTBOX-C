# -*- coding: utf-8 -*-
"""page_ctx —— 从 bin/ttbox-web.py 原样搬出（2026-10-02 web 换写法 S10 第一步）。

`/ /desktop /mobile /activate` 四个页面共用的上下文构造器，外加机器码计算。
★ `_ui_block` / `kAppVersion` 是函数（实现体在 lib/branding.py）
  ⇒ hub.call 透传；`_PAGE_MODULE_LABELS` / `_PAGE_ASSET_VERSION`
  是模块级常量 ⇒ hub.get 访问器（代码里当值用，如 list(...) 迭代）。
★ `DEFAULT_UI_BRAND` / `device_serial` 直接从 lib import ——
  前者是字符串常量（两边不一致会「白标错乱」），
  后者是纯函数（不需要同一实例）。
搬出 2 个函数：_page_context / _machine_code

★ 本模块在 lib/，**不能 import 入口** ⇒ 段外依赖一律经 hub 调用时取。
"""

from __future__ import annotations

import json

from plugins.web.lib import hub
from plugins.web.lib.branding import DEFAULT_UI_BRAND
from plugins.web.lib.cloud_session import device_serial

def _PAGE_ASSET_VERSION():
    """入口的 _PAGE_ASSET_VERSION —— 调用时取（常量 / 单例，必须与入口**同一个对象**）。"""
    return hub.get('_PAGE_ASSET_VERSION')


def _PAGE_MODULE_LABELS():
    """入口的 _PAGE_MODULE_LABELS —— 调用时取（常量 / 单例，必须与入口**同一个对象**）。"""
    return hub.get('_PAGE_MODULE_LABELS')


def kAppVersion():
    """入口的 kAppVersion —— 调用时取（常量 / 单例，必须与入口**同一个对象**）。"""
    return hub.get('kAppVersion')


def _get_status(*args, **kwargs):
    """入口的 _get_status —— 调用时取（monkeypatch 锚点，转发须**原样透传**）。"""
    return hub.call('_get_status', *args, **kwargs)


def _ui_block(*args, **kwargs):
    """入口的 _ui_block —— 调用时取（monkeypatch 锚点，转发须**原样透传**）。"""
    return hub.call('_ui_block', *args, **kwargs)

def _page_context() -> dict:
    """品牌化页面上下文；render_template 用 ** 展开。"""
    ui = _ui_block()
    return {
        'app_title': ui['app_title'],
        'ui_brand': ui['ui_brand'],
        # ★ 模板插槽 data-ui-brand / body.ui-brand-* 的取值（闭集皮肤；缺省 yu）。
        'ui_skin': ui['skin'],
        'brand_mark': ui['brand_mark'],
        'brand_eyebrow': ui['brand_eyebrow'],
        'brand_title': ui['brand_title'],
        'default_theme': ui['default_theme'],
        'allow_theme_switch': ui['allow_theme_switch'],
        'default_hotspot_ssid': ui['default_hotspot_ssid'],
        'default_local_name': ui['default_local_name'],
        'asset_version': _PAGE_ASSET_VERSION(),
        # color_scheme 跟随品牌默认主题：原厂 ttbox = 'dark'（与改动前逐字节一致）；
        # 渠道品牌可声明 'light'，此时 data-theme 直接落 light —— 与 yu 的
        # xh（default_theme=light + allow_theme_switch=False）行为对齐。
        'visual_theme': {'id': 'default', 'version': 'built-in',
                         'color_scheme': ui['default_theme'], 'styles': []},
        'module_labels': list(_PAGE_MODULE_LABELS()),
        'motion_training_available': True,
        'motion_training_collection_available': True,
        'show_aim_trace_button': True,
        # 渠道皮肤静态前缀：非默认品牌时指向 static/<brand>/，模板据此可选加载皮肤 CSS。
        # 默认品牌 = '' ⇒ 模板走静态托管根路径（现网行为零变化）。
        'brand_static_prefix': ui.get('static_prefix') or '',
        'brand_has_custom_skin': (ui.get('static_prefix') or '') not in ('', DEFAULT_UI_BRAND),
    }


def _machine_code() -> str:
    """machine_code 单源（§7.4）：cpu_serial（/proc/cpuinfo Serial）；
    读取失败回退 core GET_STATUS.license.bind_device；两者皆空 ⇒ ''（调用方拒绝激活）。"""
    s = device_serial()
    if s:
        return s
    try:
        return str(_get_status().get('license', {}).get('bind_device', '') or '')
    except Exception:
        return ''
