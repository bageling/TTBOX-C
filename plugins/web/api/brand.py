# -*- coding: utf-8 -*-
"""api/brand.py —— 网页背景（branding background）域Blueprint。

从 bin/ttbox-web.py 搬出（2026-10-02 web 换写法 S9-a，第 1 刀）。
搬出 5 条路由 + 1 个私有辅助，**URL 一字未改**：

    GET    /api/branding/background          get_branding_background
    POST   /api/branding/background          upload_branding_background
    PATCH  /api/branding/background          update_branding_background
    DELETE /api/branding/background          delete_branding_background
    GET    /api/branding/background/image    get_branding_background_image

段外依赖处理：
    · jsonify —— 直接 from flask import（非补丁锚点）。

偏离清单：**0 处**（函数体逐行原样）。
本域是全站最独立的域：读接口全部返回同一个「仅对授权系统开放」的拒绝响应，
不碰 IPC、不碰磁盘、不读配置。
"""

from __future__ import annotations

from flask import Blueprint, jsonify

bp = Blueprint('brand', __name__)


def _branding_rejected():
    # 保持 Web 契约：网页背景仅对 TTBOX 授权系统开放
    return jsonify({'ok': False, 'error': '网页背景仅对 TTBOX 授权系统开放'})


@bp.get('/api/branding/background')
def get_branding_background():
    return _branding_rejected()


@bp.post('/api/branding/background')
def upload_branding_background():
    return _branding_rejected()


@bp.patch('/api/branding/background')
def update_branding_background():
    return _branding_rejected()


@bp.delete('/api/branding/background')
def delete_branding_background():
    return _branding_rejected()


@bp.get('/api/branding/background/image')
def get_branding_background_image():
    return jsonify({'ok': True, 'data': {}})
