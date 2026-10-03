# -*- coding: utf-8 -*-
"""api/system.py —— 系统与主题域 Blueprint。

从 bin/ttbox-web.py 搬出（2026-10-02 web 换写法 S9-b）。
搬出 15 条路由，**URL 一字未改**。

系统状态 / 存储 / 主机名 / 端口 / 重启 / 关机 / 设备重激活 + 主题 6 条。
★ 15 条路由但每个都短，故本域是「域里路由最多、单条最小」的一个。
主题（themes / theme-assets）属配色皮肤，与 api/brand.py（网页背景图）分开。
段外依赖处理：
    · _license_block —— 经 hub.call 调用时取。
    · subprocess —— 标准库，直接 import。
    · jsonify / request —— flask，直接 import。
    · _rootfs_expand_payload(from rootfs), _storage(from sysinfo), collect_network_summary(from sysinfo), collect_system_stats(from sysinfo) —— lib里已有，直接 import。

★ **禁止在 except 子句里调 hub**（求值时机是函数定义时，此时 hub.bind 还没执行 ⇒ 永久绑成 None ⇒ 该 except 永不匹配）。
  需要按异常类型分流时，抽 `_is_xxx(exc)` 函数在调用时才取类。
"""

from __future__ import annotations

import subprocess

from flask import Blueprint, jsonify, request

from plugins.web.lib import hub
from plugins.web.lib import paths as ttbox_paths
from plugins.web.lib.rootfs import _rootfs_expand_payload
from plugins.web.lib.sysinfo import _storage
from plugins.web.lib.sysinfo import collect_network_summary
from plugins.web.lib.sysinfo import collect_system_stats

bp = Blueprint('system', __name__)


def _license_block(*args, **kwargs):
    """入口的 _license_block —— 调用时取（monkeypatch 锚点）。"""
    return hub.call('_license_block', *args, **kwargs)


def _power_action(*args, **kwargs):
    """入口的 _power_action —— 调用时取（它内部调 _power_action_allowed，
    后者才是补丁锚点；这里只需转调，补丁照样穿透）。"""
    return hub.call('_power_action', *args, **kwargs)

@bp.get('/api/announcement')
def get_announcement():
    # 保持 Web 契约：无公告源可达时返回 503 + error 结构。
    # ★ T1.07b：旧文案把"公告不可达"错说成"授权服务器失败" —— 既是旧授权残留，
    #   又与"授权真相只在 core"冲突（此处与授权无关，不得暗示授权服务器存在）。
    return jsonify({'ok': False, 'error': 'announcement source unavailable'}), 503


@bp.get('/api/system')
def get_system_status():
    return jsonify({'ok': True, 'data': collect_system_stats()})


@bp.get('/api/system/storage')
def get_storage_status():
    s = _storage()
    force = str(request.args.get('force') or '') not in ('', '0', 'false')
    return jsonify({
        'ok': True,
        'data': {
            'free': s['free'],
            'path': ttbox_paths.ttbox_prefix(),
            'percent': s['percent'],
            'root_free': s['free'],
            'root_percent': s['percent'],
            'root_total': s['total'],
            'root_used': s['used'],
            'rootfs': _rootfs_expand_payload('status', force=force),
        },
    })


@bp.post('/api/system/storage/expand')
def expand_storage():
    payload = _rootfs_expand_payload('expand', force=True)
    if not payload['expandable']:
        # 409：请求合法，但当前机器没有可扩空间（或工具/文件系统不支持）。
        # 真实原因走 error.message + data.rootfs，前端原样展示。
        return jsonify({'ok': False, 'error': payload['message'],
                        'data': {'rootfs': payload}}), 409
    if not payload['action_available']:
        # 501：物理上可扩，但本仓没有改分区表的执行通道 —— 就说没实现。
        # 旧行为是回一句"扩容需重启进恢复流程"，把"没做"说成"做了一半"。
        return jsonify({'ok': False,
                        'error': '检测到可扩空间，但扩容执行通道尚未装箱（Web 不会修改分区表）',
                        'data': {'rootfs': payload}}), 501
    # fail-closed：action_available 若在未接线的情况下为真，宁可报错也不假装扩容成功。
    return jsonify({'ok': False, 'error': '扩容执行通道状态异常',
                    'data': {'rootfs': payload}}), 500


@bp.put('/api/system/hostname')
def update_system_hostname():
    body = request.get_json(silent=True) or {}
    hostname = str(body.get('hostname', '')).strip()
    if not hostname or len(hostname) > 63:
        return jsonify({'ok': False, 'error': '主机名无效'})
    r = subprocess.run(['hostnamectl', 'set-hostname', hostname], capture_output=True, text=True, timeout=10)
    if r.returncode != 0:
        return jsonify({'ok': False, 'error': r.stderr or '设置失败'})
    # 保持 Web 契约：返回 hostname/lan_ipv4/lan_url/mdns_url/web_port
    return jsonify({'ok': True, 'data': collect_network_summary()})


@bp.put('/api/system/web-port')
def update_system_web_port():
    # Web 控制台端口已在源码中固定（见文件头 LISTEN_PORT），不再支持运行时修改。
    # 保留这个接口是为了让界面上的"修改端口"拿到一句明确的人话，而不是改完没反应。
    #
    # ★ 2026-09-23（全仓审查复核 #38）：这里原先在这行 return **之后**还留着 30 行
    #   「写 systemd drop-in + daemon-reload + 延时重启」的实现 —— 永不可达。
    #   危害不是"多跑了一段"（它压根不跑），而是**看着像活的实现**：后来人会照着它
    #   推理"端口本来是可改的"，或者以为删掉下面这行 return 就能启用。已整块删除。
    #   真要恢复运行时改端口，得把 LISTEN_PORT 的固定一并重新设计，不是撤掉这行 return 的事。
    return jsonify({'ok': False,
                    'error': 'Web 控制台端口已固定为 8000，不支持在线修改'}), 400


@bp.post('/api/system/reactivate')
def reactivate_device():
    # ★ T1.07b：旧形**焊死**"TTBOX 本地授权恒激活，无需修复" —— 未激活也答"正常"（假绿）。
    #   改为读真状态（core IPC 投影）：已激活才答"正常"；未激活必须如实报，不谎称正常。
    lic = _license_block()
    if lic.get('activated'):
        return jsonify({'ok': False, 'error': '当前授权状态正常，无需修复授权'}), 400
    # 未激活：不谎报"正常"；真实在线修复需服务器，下沉 T2.x（本任务不做）。
    return jsonify({'ok': False,
                    'error': f"授权未激活（state={lic.get('state')}）；在线修复下沉 T2.x"}), 409


@bp.post('/api/system/reboot')
def reboot_system():
    return _power_action('reboot')


@bp.post('/api/system/poweroff')
def poweroff_system():
    return _power_action('poweroff')


@bp.get('/api/themes')
def get_themes():
    # 保持 Web 契约：内置 default 主题结构（标题用 TTBOX 品牌）
    return jsonify({'ok': True, 'data': {
        'active_theme_id': 'default',
        'active_version': '',
        'offline': False,
        'purchase_url': '',
        'themes': [{
            'active': True,
            'compatible': True,
            'description': '系统内置主题，始终可用。',
            'id': 'default',
            'installed': True,
            'installed_version': 'built-in',
            'latest_version': 'built-in',
            'owned': True,
            'previews': [],
            'published': True,
            'title': 'TTBOX 默认主题',
            'update_available': False,
        }],
    }})


@bp.get('/api/themes/<theme_id>/previews/<int:index>')
def theme_preview(theme_id: str, index: int):
    return jsonify({'ok': True, 'data': {}})


@bp.post('/api/themes/redeem')
def redeem_theme():
    body = request.get_json(silent=True) or {}
    # 保持 Web 契约：无主题卡密报错
    if not str(body.get('code') or body.get('redeem_code') or '').strip():
        return jsonify({'ok': False, 'error': '请输入主题卡密'})
    return jsonify({'ok': False, 'error': 'redeem failed: invalid code'})


@bp.post('/api/themes/<theme_id>/install')
def install_theme(theme_id: str):
    body = request.get_json(silent=True) or {}
    # 保持 Web 契约：install 需要 core download url
    if not str(body.get('download_url') or '').strip():
        return jsonify({'ok': False, 'error': 'core download url is required'})
    if theme_id != 'default':
        return jsonify({'ok': False, 'error': f'theme not found: {theme_id}'})
    return jsonify({'ok': True, 'data': {'installed': True, 'theme_id': 'default'}})


@bp.put('/api/themes/current')
def select_theme():
    body = request.get_json(silent=True) or {}
    theme_id = str(body.get('theme_id') or 'default')
    if theme_id != 'default':
        return jsonify({'ok': False, 'error': f'theme not found: {theme_id}'})
    # 保持 Web 契约：返回 active_theme_id + active_version
    return jsonify({'ok': True, 'data': {'active_theme_id': 'default', 'active_version': ''}})


@bp.get('/theme-assets/<theme_id>/<version>/<path:filename>')
def theme_asset(theme_id: str, version: str, filename: str):
    return jsonify({'ok': True, 'data': {}})
