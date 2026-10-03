"""ota.py — OTA 更新通道的 web 侧助手（2026-10-02 web 换写法 S6）。

从 ``plugins/web/bin/ttbox-web.py`` 原样搬出 4 个函数：
``_ota_current_version`` / ``_ota_ver_key`` / ``_ota_server_latest`` / ``_ota_install_impl``。
入口按老名字 re-export。

## 依赖接缝

四个 OTA 常量（``OTA_SERVER_URL`` / ``OTA_JOBS_DIR`` / ``OTA_UPDATER_PATH`` /
``OTA_DEFAULT_KEY_ID``）是测试的 monkeypatch 锚点（分别 2 / 3 / 3 处）⇒ **留在入口**，
本模块各自用一个小取数函数在**调用时** ``hub.get()``，不在 import 期快照。
``_get_status``（20 处锚点）与 ``_license_block``（12 处锚点）同法处理。

⇒ 与搬运原文的唯一偏离是「裸常量名 → 取数函数」这一处机械改写，逻辑一行未改。
"""
from __future__ import annotations

import json
import os
import re
import time

from flask import jsonify, request

from plugins.web.lib import hub
from plugins.web.lib import paths as ttbox_paths


def _ota_server_url():
    return hub.get('OTA_SERVER_URL')


def _ota_jobs_dir():
    return hub.get('OTA_JOBS_DIR')


def _ota_updater_path():
    return hub.get('OTA_UPDATER_PATH')


def _ota_default_key_id():
    return hub.get('OTA_DEFAULT_KEY_ID')


def _get_status(*args, **kwargs):
    """留在入口（补丁锚点 ×20），此处仅转发。"""
    return hub.call('_get_status', *args, **kwargs)


def _license_block(*args, **kwargs):
    """留在入口（补丁锚点 ×12），此处仅转发。"""
    return hub.call('_license_block', *args, **kwargs)


def _ota_current_version() -> str:
    """当前版本：current 软链目录名优先，IPC GET_STATUS.version 兜底（web 侧单点）。"""
    try:
        name = os.path.basename(os.path.realpath(ttbox_paths.prefix_path('current')).rstrip('/'))
        if name and name != 'current':
            return name
    except Exception:
        pass
    try:
        return str((_get_status() or {}).get('version') or '')
    except Exception:
        return ''


def _ota_ver_key(v):
    """与 scripts/ttbox_ota_updater.py::_version_key 同源（数字段按值、其余按字典序）。"""
    import re as _re
    parts = []
    for seg in _re.split(r'[.\-_+]', str(v or '')):
        if not seg:
            continue
        if seg.isdigit():
            parts.append((0, int(seg), ''))
        else:
            parts.append((1, 0, seg))
    return tuple(parts)


def _ota_server_latest() -> dict:
    """向写死的分发服务器发**一次查询**（O11 契约，docs/protocols/ota-server-contract.md）。

    返回 {'latest_version':…, 'package_url':…, 'sign_url':…}；任何异常上抛由调用方转错。
    """
    import urllib.request as _ur
    url = _ota_server_url().rstrip('/') + '/latest?current=' + _ota_current_version()
    with _ur.urlopen(url, timeout=6) as r:
        doc = json.loads(r.read().decode('utf-8', 'replace'))
    if not isinstance(doc, dict) or not doc.get('latest_version'):
        raise ValueError('服务器响应缺 latest_version')
    return doc


def _ota_install_impl():
    """OTA 安装实现（/api/ota/install 与 /api/update/install 共用，单一实现点）。

    2026-09-18 定案 §2.2/D03：web（User=ttbox）无 sudoers/polkit，不再用 systemd-run；
    改为**写任务文件**到入口的 OTA_JOBS_DIR（root:ttbox 0770），由 ttbox-ota.path 监听并拉起
    root 更新器。前置校验失败**返错**，绝不返回 ok:true（消灭 D03 假成功）。
    """
    if not _license_block().get('capabilities', {}).get('ota'):
        return jsonify({'ok': False, 'error': "feature 'ota' not licensed"}), 403
    body = request.get_json(silent=True) or {}
    url = str(body.get('url') or '').strip()
    key_id = str(body.get('key_id') or _ota_default_key_id()).strip()
    # ★ key_id/version 会进 root 更新器的路径拼装（<keys_dir>/<key_id>.pub、
    #   releases/<ver>.ota.staging），与更新器 _check_safe_id 同一白名单，
    #   源头就拒掉路径穿越（"../../tmp/evil" 可让 root 验签加载攻击者公钥）。
    _OTA_ID_RE = re.compile(r'^[A-Za-z0-9][A-Za-z0-9._-]*$')
    if not _OTA_ID_RE.match(key_id) or '..' in key_id:
        return jsonify({'ok': False, 'error': '非法 key_id'}), 400
    if not url:
        return jsonify({'ok': False, 'error': 'url is required'}), 400
    if not url.startswith('https://'):
        return jsonify({'ok': False, 'error': '仅接受 https 更新源'}), 400
    # 前置校验（D03）：更新器在、任务目录在且可写 —— 任何一条不满足都是环境故障，
    # 必须当场报错而不是假装调度成功（旧版假成功 = 装了也白装）。
    if not os.path.isfile(_ota_updater_path()):
        return jsonify({'ok': False, 'error': f'更新器缺失: {_ota_updater_path()}'}), 500
    if not os.path.isdir(_ota_jobs_dir()):
        return jsonify({'ok': False, 'error': f'任务目录缺失: {_ota_jobs_dir()}'}), 500
    try:
        probe = os.path.join(_ota_jobs_dir(), '.probe-%d' % int(time.time()))
        with open(probe, 'w', encoding='utf-8') as f:
            f.write('{}')
        os.remove(probe)
    except Exception as e:
        return jsonify({'ok': False, 'error': f'任务目录不可写: {e!r}'}), 500
    job = {'url': url, 'key_id': key_id, 'enqueued_at': int(time.time())}
    if str(body.get('version') or '').strip():
        job['version'] = str(body['version']).strip()
        if not _OTA_ID_RE.match(job['version']) or '..' in job['version']:
            return jsonify({'ok': False, 'error': '非法 version'}), 400
    name = 'job-%d.json' % int(time.time() * 1000)
    try:
        with open(os.path.join(_ota_jobs_dir(), name), 'w', encoding='utf-8') as f:
            json.dump(job, f, ensure_ascii=False, sort_keys=True)
    except Exception as e:
        return jsonify({'ok': False, 'error': f'写任务文件失败: {e!r}'}), 500
    return jsonify({'ok': True, 'data': {'scheduled': name, 'message': '更新任务已受理'}})
