# -*- coding: utf-8 -*-
"""api/ota.py —— OTA 与更新域 Blueprint。

从 bin/ttbox-web.py 搬出（2026-10-02 web 换写法 S9-b）。
搬出 4 条路由，**URL 一字未改**。

OTA 安装 / 更新安装 / 更新状态 / 检查更新。
★ 这 4 条的实现体在 S6 就已搬到 lib/ota.py，本文件只做 HTTP 编排。
★ 铁律：切云端 latest 前必须确认这一版就是要上生产的 ——
  实测从切版到在线盒子点「检查更新」跑起来只隔 10 秒。
段外依赖处理：
    · OTA_SERVER_URL —— 经 hub.get 调用时取。
    · _ota_current_version / _ota_install_impl / _ota_server_latest / _ota_ver_key —— 经 hub.call 调用时取。
    · json / os / time —— 标准库，直接 import。
    · jsonify —— flask，直接 import。

★ **禁止在 except 子句里调 hub**（求值时机是函数定义时，此时 hub.bind 还没执行 ⇒ 永久绑成 None ⇒ 该 except 永不匹配）。
  需要按异常类型分流时，抽 `_is_xxx(exc)` 函数在调用时才取类。
"""

from __future__ import annotations

import json
import os
import time

from flask import Blueprint, jsonify

from plugins.web.lib import hub

bp = Blueprint('ota', __name__)

def OTA_DEFAULT_KEY_ID():
    """入口的 OTA_DEFAULT_KEY_ID —— 调用时取（单例 / 共享状态，身份须唯一）。"""
    return hub.get('OTA_DEFAULT_KEY_ID')


def OTA_STATUS_FILE():
    """入口的 OTA_STATUS_FILE —— 调用时取（单例 / 共享状态，身份须唯一）。"""
    return hub.get('OTA_STATUS_FILE')


def OTA_SERVER_URL():
    """入口的 OTA_SERVER_URL —— 调用时取（补丁锚点 / 单例 / 异常类，身份须唯一）。"""
    return hub.get('OTA_SERVER_URL')


def _ota_current_version(*args, **kwargs):
    """入口的 _ota_current_version —— 调用时取（monkeypatch 锚点）。"""
    return hub.call('_ota_current_version', *args, **kwargs)


def _ota_install_impl(*args, **kwargs):
    """入口的 _ota_install_impl —— 调用时取（monkeypatch 锚点）。"""
    return hub.call('_ota_install_impl', *args, **kwargs)


def _ota_server_latest(*args, **kwargs):
    """入口的 _ota_server_latest —— 调用时取（monkeypatch 锚点）。"""
    return hub.call('_ota_server_latest', *args, **kwargs)


def _ota_ver_key(*args, **kwargs):
    """入口的 _ota_ver_key —— 调用时取（monkeypatch 锚点）。"""
    return hub.call('_ota_ver_key', *args, **kwargs)

@bp.post('/api/ota/install')
def api_ota_install():
    """OTA-01：调度入口；实现与 /api/update/install 共用 _ota_install_impl（单一实现点）。"""
    return _ota_install_impl()


@bp.post('/api/update/install')
def api_update_install():
    """YU 前端兼容：POST /api/update/install → 转发 OTA 调度（含 capabilities.ota 门控）。"""
    return _ota_install_impl()


@bp.get('/api/update/status')
def api_update_status():
    """读更新器的 ota_status.json（2026-09-18 定案 D-14）。

    更新器在 run() 开始时写 RUNNING、结束写 SUCCESS/FAILED；本端点投影成前端
    轮询期望的形状。文件不存在 = 从未跑过更新 ⇒ idle（无假壳）。
    """
    try:
        doc = json.loads(open(OTA_STATUS_FILE(), encoding='utf-8').read())
    except Exception:
        doc = {}
    state = str(doc.get('state') or '').upper()
    if state == 'RUNNING':
        # 2026-09-19 修复「卡50」：进度由更新器分阶段写入，这里只做投影。
        # 另加超时保护：状态文件 30 分钟没动过 ⇒ 更新器大概率已死，报失败而不是永远转圈。
        try:
            progress = max(1, min(99, int(doc.get('progress'))))
        except (TypeError, ValueError):
            progress = 10
        try:
            stale = (time.time() - os.path.getmtime(OTA_STATUS_FILE())) > 1800
        except OSError:
            stale = False
        if stale:
            return jsonify({'ok': True, 'data': {'status': 'failed', 'progress': 100,
                                                 'error': '更新进程中断（状态超过30分钟无进展）'}})
        return jsonify({'ok': True, 'data': {'status': 'running', 'progress': progress,
                                             'message': str(doc.get('phase') or '更新进行中'),
                                             'version': doc.get('version') or ''}})
    if state == 'SUCCESS':
        # 2026-09-20 修复「开页面就自动刷新」：状态文件装完后永久停在 SUCCESS，
        # 前端无法区分"刚装完"和"几十小时前的旧结果"。补 finished_at（文件 mtime）
        # 让前端只在"刚刚完成"时才提示刷新，历史残留忽略。
        try:
            finished_at = int(os.path.getmtime(OTA_STATUS_FILE()))
        except OSError:
            finished_at = 0
        return jsonify({'ok': True, 'data': {'status': 'success', 'progress': 100,
                                             'version': doc.get('version') or '',
                                             'finished_at': finished_at}})
    if state == 'FAILED':
        try:
            finished_at = int(os.path.getmtime(OTA_STATUS_FILE()))
        except OSError:
            finished_at = 0
        return jsonify({'ok': True, 'data': {'status': 'failed', 'progress': 100,
                                             'error': str(doc.get('detail')
                                                          or doc.get('error') or '更新失败'),
                                             'finished_at': finished_at}})
    return jsonify({'ok': True, 'data': {'status': 'idle'}})


@bp.post('/api/update/check')
def api_update_check():
    """检查更新（2026-09-18 定案 §2.1/§三 H-25）：向写死的服务器查一次，比对版本。

    返回 data：{update_available, current_version, latest_version,
                package_url, sign_url, key_id}。前端拿 package_url 直接安装；
    sign_url 必须等于 package_url + '.sign.json'（更新器旁车规则，契约已钉死）。
    服务器地址含 example.com（占位值）⇒ fail-closed 503，不给假结果。
    """
    if 'example.com' in OTA_SERVER_URL():
        return jsonify({'ok': False, 'error': 'ota_server_not_configured',
                        'detail': '分发服务器地址尚未配置（OTA_SERVER_URL() 仍为占位值）'}), 503
    if not OTA_SERVER_URL().startswith('https://'):
        return jsonify({'ok': False, 'error': 'ota_server_not_configured',
                        'detail': 'OTA_SERVER_URL 必须是 https'}), 503
    try:
        latest = _ota_server_latest()
    except Exception as e:
        return jsonify({'ok': False, 'error': 'ota_server_unreachable',
                        'detail': repr(e)}), 502
    cur = _ota_current_version()
    ver = str(latest.get('latest_version') or '').strip()
    pkg = str(latest.get('package_url') or '').strip()
    sig = str(latest.get('sign_url') or (pkg + '.sign.json' if pkg else '')).strip()
    update_available = bool(ver and pkg and _ota_ver_key(ver) > _ota_ver_key(cur)) if cur else bool(ver and pkg)
    # 增量包探测（2026-09-19）：服务器若存了 ttbox-update-<ver>-delta-from-<cur>.tgz，
    # 就把 package_url 换成增量地址（旁车 sign.json 存在才算数）。更新器侧仍 fail-closed：
    # 基线不匹配会整包拒绝，绝不带病浇筑。全量地址保留在 full_package_url 便于排查。
    full_pkg, full_sig = pkg, sig
    if update_available and pkg.startswith('https://') and cur:
        try:
            import urllib.request as _ur2
            name = pkg.rsplit('/', 1)[-1]
            if name.startswith('ttbox-update-') and name.endswith('.tgz'):
                d_url = pkg.rsplit('/', 1)[0] + '/' + name[:-4] + '-delta-from-%s.tgz' % cur
                with _ur2.urlopen(d_url + '.sign.json', timeout=6) as r:
                    d_sign = json.loads(r.read().decode('utf-8', 'replace'))
                if isinstance(d_sign, dict) and d_sign.get('sha256'):
                    pkg, sig = d_url, d_url + '.sign.json'
        except Exception:
            pass  # 探测不到增量 ⇒ 用全量，正常路径
    return jsonify({'ok': True, 'data': {
        'update_available': update_available,
        'current_version': cur,
        'latest_version': ver,
        'package_url': pkg,
        'sign_url': sig,
        'full_package_url': full_pkg,
        'full_sign_url': full_sig,
        'delta': pkg != full_pkg,
        'key_id': OTA_DEFAULT_KEY_ID(),
    }})
