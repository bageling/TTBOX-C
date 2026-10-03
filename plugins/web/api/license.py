# -*- coding: utf-8 -*-
"""api/license.py —— 授权域 Blueprint。

从 bin/ttbox-web.py 搬出（2026-10-02 web 换写法 S9-b）。
搬出 5 条路由，**URL 一字未改**。

授权查询 / 激活 / 重置本地身份 / 全量恢复。
★ `CloudLicenseError` 是异常类 —— 经 hub.get 调用时取，
  且**绝不出现在 except 子句**。
段外依赖处理：
    · CloudLicenseError —— 经 hub.get 调用时取。
    · _license_block / ipc_request —— 经 hub.call 调用时取。
    · jsonify / request —— flask，直接 import。
    · card_mask(from cloud_session) —— lib里已有，直接 import。

★ **禁止在 except 子句里调 hub**（求值时机是函数定义时，此时 hub.bind 还没执行 ⇒ 永久绑成 None ⇒ 该 except 永不匹配）。
  需要按异常类型分流时，抽 `_is_xxx(exc)` 函数在调用时才取类。
"""

from __future__ import annotations

from flask import Blueprint, jsonify, request

from plugins.web.lib import hub
from plugins.web.lib.cloud_session import card_mask

from plugins.web.lib.cloud_client import CloudLicenseError

bp = Blueprint('license', __name__)

def _ensure_heartbeat_worker(*args, **kwargs):
    """入口的 _ensure_heartbeat_worker —— 调用时取（monkeypatch 锚点，转发须**原样透传**：
    测试替身常是少参数的 lambda，补默认值会多塞实参而 TypeError）。"""
    return hub.call('_ensure_heartbeat_worker', *args, **kwargs)


def _invalidate_activation_cache(*args, **kwargs):
    """入口的 _invalidate_activation_cache —— 调用时取（monkeypatch 锚点，转发须**原样透传**：
    测试替身常是少参数的 lambda，补默认值会多塞实参而 TypeError）。"""
    return hub.call('_invalidate_activation_cache', *args, **kwargs)


def _ACTIVATION_LOCK():
    """入口的 _ACTIVATION_LOCK —— 调用时取（单例 / 共享状态，身份须唯一）。"""
    return hub.get('_ACTIVATION_LOCK')


def _CLOUD_CLIENT():
    """入口的 _CLOUD_CLIENT —— 调用时取（单例 / 共享状态，身份须唯一）。"""
    return hub.get('_CLOUD_CLIENT')


def _CLOUD_FEATURES_FULL():
    """入口的 _CLOUD_FEATURES_FULL —— 调用时取（单例 / 共享状态，身份须唯一）。"""
    return hub.get('_CLOUD_FEATURES_FULL')


def _CLOUD_SESSION():
    """入口的 _CLOUD_SESSION —— 调用时取（单例 / 共享状态，身份须唯一）。"""
    return hub.get('_CLOUD_SESSION')


def _license_payload(*args, **kwargs):
    """入口的 _license_payload —— 调用时取。"""
    return hub.call('_license_payload', *args, **kwargs)


def _machine_code(*args, **kwargs):
    """入口的 _machine_code —— 调用时取。"""
    return hub.call('_machine_code', *args, **kwargs)


def _license_block(*args, **kwargs):
    """入口的 _license_block —— 调用时取（monkeypatch 锚点）。"""
    return hub.call('_license_block', *args, **kwargs)


def ipc_request(*args, **kwargs):
    """入口的 ipc_request —— 调用时取（monkeypatch 锚点）。"""
    return hub.call('ipc_request', *args, **kwargs)

@bp.get('/api/license')
def get_license():
    return jsonify({'ok': True, 'data': _license_payload()})


@bp.post('/api/license/activate')
def activate_license():
    """M2.07 激活端点（主入口 = 云端卡密 card-login；语义重接，§2.4）。

    链路（§4.1）：入参校验 → 并发锁 → CloudLicenseClient.card_login（HMAC 四头）
    → 成功落 cloud_session.json → IPC ACTIVATE_CLOUD 交 core（唯一落盘/执法点）
    → 心跳线程拉起 → 返回 license 全量投影。
    错误语义：云端 error 原文透传（§7.1，激活页直接展示）。
    判据顺序：400（入参）> 云端 400/403（原样透传）> 502（core 不可达/网络）。

    ★ D7 存量兼容：离线卡 JSON 信封（`{` 开头）保留旧 ACTIVATE_LICENSE 路径
      （Ed25519 验签，core 能力未废），但不再是主入口。
    """
    body = request.get_json(silent=True) or {}
    license_key = str(body.get('license_key') or '').strip()
    if not license_key:
        return jsonify({'ok': False, 'error': 'license_key is required'}), 400

    # ---- 存量离线卡路径（保留不调用为主；仅信封原文显式提交时走）----
    if license_key.lstrip().startswith('{'):
        resp = ipc_request('ACTIVATE_LICENSE', {'card': license_key}, timeout=10)
        if not isinstance(resp, dict) or resp.get('status') != 0:
            err = resp.get('error') if isinstance(resp, dict) else None
            return jsonify({'ok': False,
                            'error': f'激活被拒绝：{err or "IPC 无响应（Core 未运行?）"}'}), 400
        data = resp.get('data') or {}
        _invalidate_activation_cache()
        return jsonify({'ok': True, 'data': {
            'state': data.get('state', 'active'),
            'activated': bool(data.get('activated', True)),
            'plan': data.get('plan', 'none'),
            'is_pro': bool(data.get('is_pro', False)),
            'ui_brand': data.get('ui_brand', 'ttbox'),
            'features': list(data.get('features') or []),
            'expire_unix_ms': data.get('expire_unix_ms', 0),
            'license': _license_payload(),
        }})

    # ---- 云端 card-login（主入口）----
    if not _ACTIVATION_LOCK().acquire(blocking=False):
        return jsonify({'ok': False, 'error': '激活进行中，请稍候'}), 409
    try:
        machine_code = _machine_code()
        if not machine_code:
            return jsonify({'ok': False, 'error': '无法读取设备指纹（cpu_serial）'}), 500
        try:
            login = _CLOUD_CLIENT().card_login(license_key, machine_code)
        except CloudLicenseError as e:
            # 错误语义原样透传；HTTP 映射：400→400、401/403→403、网络/其它→502
            if e.status == 400:
                http_status = 400
            elif e.status in (401, 403):
                http_status = 403
            else:
                http_status = 502
            return jsonify({'ok': False, 'error': e.message or e.code}), http_status

        # 云端验证成功 ⇒ 会话落盘（0600 原子写；D8）
        expire_unix_ms = int(login['expire_unix_s']) * 1000
        session_data = {
            'card_key': license_key,
            'card_mask': card_mask(license_key),
            'client_token': login['client_token'],
            'expire_at': login['expire_at'],
            'expire_unix_ms': expire_unix_ms,
            'max_devices': int(login['max_devices']),
            'heartbeat_interval': int(login['heartbeat_interval']),
            'heartbeat_timeout': int(login['heartbeat_timeout']),
        }
        if not _CLOUD_SESSION().save(session_data):
            return jsonify({'ok': False, 'error': '云端验证成功，但会话落盘失败'}), 500

        # core ACTIVATE_CLOUD（D2：core 是唯一落盘/执法点，web 不直写 LicenseStore）
        params = {
            'expire_unix_ms': expire_unix_ms,
            'features': list(_CLOUD_FEATURES_FULL()),
            'plan': 'subscription',
            'card_mask': card_mask(license_key),
            'max_devices': int(login['max_devices']),
            'source': 'cloud',
        }
        resp = ipc_request('ACTIVATE_CLOUD', params, timeout=10)
        if not isinstance(resp, dict) or resp.get('status') != 0:
            err = resp.get('error') if isinstance(resp, dict) else None
            _CLOUD_SESSION().clear()
            return jsonify({'ok': False,
                            'error': f'云端验证成功但核心激活失败：{err or "IPC 无响应（Core 未运行?）"}'}), 502

        _invalidate_activation_cache()
        _ensure_heartbeat_worker()
        data = resp.get('data') or {}
        return jsonify({'ok': True, 'data': {
            'state': data.get('state', 'valid'),
            'activated': bool(data.get('activated', True)),
            'plan': data.get('plan', 'subscription'),
            'is_pro': bool(data.get('is_pro', False)),
            'ui_brand': data.get('ui_brand', 'ttbox'),
            'features': list(data.get('features') or []),
            'expire_unix_ms': data.get('expire_unix_ms', expire_unix_ms),
            'license': _license_payload(),
        }})
    finally:
        _ACTIVATION_LOCK().release()


@bp.post('/api/activation/reset-local-identity')
def reset_activation_local_identity():
    # ★ T1.07b：旧形**无条件**声称授权"正常"（未激活时也这么答 ⇒ 掩盖真实状态）。
    #   改为读真状态：已激活 ⇒ 保留"正常、拒绝破坏性操作"语义（400）；
    #   未激活 ⇒ 不得称"正常"，如实报 + 明牌本地修复下沉 T2.x（409）。
    lic = _license_block()
    if lic.get('activated'):
        return jsonify({'ok': False, 'error': '当前授权状态正常，已拒绝重置本地授权身份'}), 400
    return jsonify({'ok': False,
                    'error': f"授权未激活（state={lic.get('state')}）；本地重置下沉 T2.x"}), 409


@bp.get('/api/activation/full-recovery')
def get_activation_full_recovery():
    # ★ T1.07b：旧形 `allowed`/`available` **恒 false** 且 reason 恒称"状态正常"
    #   （未激活也这么答 ⇒ 掩盖真实状态）。改为据真状态给值（不再恒 false）：
    #   · `allowed`  = **授权侧是否阻止**恢复：已激活 ⇒ 阻止（保护授权身份）；未激活 ⇒ 不阻止。
    #   · `available`= **本版本是否真有实现**：M1 无本地全量恢复实现（下沉 T2.x）⇒ **恒 false**。
    #     两者分岔是刻意的 —— 不许用 `allowed=true` 冒充"现在就能恢复"（假绿禁）。
    lic = _license_block()
    if lic.get('activated'):
        allowed = False
        reason = '当前授权和 daemon 状态正常，已拒绝本地全量恢复'
    else:
        allowed = True
        reason = f"授权未激活（state={lic.get('state')}）；授权侧不阻止恢复，但本地全量恢复下沉 T2.x"
    return jsonify({'ok': True, 'data': {
        'allowed': allowed,
        'available': False,
        'reason': reason,
        'saved_at': '',
        'size': 0,
        'source': '',
        'version': '',
    }})


@bp.post('/api/activation/full-recovery')
def start_activation_full_recovery():
    # ★ T1.07b：旧形**无条件**称"授权正常"。读真状态后再答：
    #   已激活 ⇒ 保留"拒绝破坏性操作"语义（400）；未激活 ⇒ 不称"正常"，如实报（409）。
    lic = _license_block()
    if lic.get('activated'):
        return jsonify({'ok': False, 'error': '当前授权和 daemon 状态正常，已拒绝本地全量恢复'}), 400
    return jsonify({'ok': False,
                    'error': f"授权未激活（state={lic.get('state')}）；本地全量恢复下沉 T2.x"}), 409

