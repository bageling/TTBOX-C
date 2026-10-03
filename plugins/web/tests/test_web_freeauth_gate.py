# test_web_freeauth_gate.py — M2.07 T01/T02 免密化 + 激活 gate 单测
#
# 覆盖（impl-spec T01 验收要点 + D1/D9/D10）：
#   ① 全端点免密直通（含 /api/ota/install、reboot/poweroff —— dry_run 不真关机）；
#   ② /setup、/login 页面下线（404）；grep 门禁：first-setup|_RL_ACTIVATE|scrypt 命中 0；
#   ③ web_credentials.json 启动退役改名 .retired；
#   ④ LAN 黑名单 403 执法保留（网络层功能，非鉴权）；
#   ⑤ 未激活：API 403 activation_required + 白名单 5 类端点可达；页面 302 /activate；
#   ⑥ /api/update/install|status 薄映射端点存在。
#
# 运行：python -m pytest plugins/web/tests/test_web_freeauth_gate.py -v（从仓库根）
from __future__ import annotations

import importlib.util
import json
import os
import pathlib
import shutil
import sys
import tempfile

import pytest

from plugins.web.lib.paths import repo_root as _ttbox_repo_root  # noqa: E402  路径单点真源（A-PATH-3）
REPO_ROOT = pathlib.Path(_ttbox_repo_root())
WEB_SRC = REPO_ROOT / 'plugins' / 'web' / 'bin' / 'ttbox-web.py'
# ★ P6：原先在这里 insert(0, REPO_ROOT/WEB_DIR) 替 ttbox-web.py 补 sys.path。
#   ttbox-web.py 现在**自带根锚 bootstrap**（A-PATH-3），测试不再替它铺路 ——
#   否则"它到底能不能在正常入口下加载"这件事永远验证不到。
_load_seq = 0


def _load_ttbox_web():
    """按唯一模块名加载 ttbox-web.py（避免与其它用例串味）。"""
    global _load_seq
    _load_seq += 1
    name = 'ttbox_web_freeauth_%d_%d' % (os.getpid(), _load_seq)
    spec = importlib.util.spec_from_file_location(name, WEB_SRC)
    mod = importlib.util.module_from_spec(spec)
    sys.modules[name] = mod
    spec.loader.exec_module(mod)
    return mod


ACTIVATED_LICENSE = {
    'activated': True, 'state': 'valid', 'plan': 'subscription', 'is_pro': False,
    'features': ['capture', 'inference', 'aim', 'ota'], 'expires_at': '',
    'grace_until': '', 'message': '', 'heartbeat_interval_s': 60,
    'status': 'valid', 'valid': True, 'ui_brand': 'ttbox', 'short_code': '',
    'capabilities': {'capture': True, 'inference': True, 'aim': True, 'ota': True},
}
UNACTIVATED_LICENSE = {
    'activated': False, 'state': 'unactivated', 'plan': 'none', 'is_pro': False,
    'features': [], 'expires_at': '', 'grace_until': '', 'message': '',
    'heartbeat_interval_s': 60, 'status': 'unactivated', 'valid': False,
    'ui_brand': 'ttbox', 'short_code': '',
    'capabilities': {'capture': False, 'inference': False, 'aim': False, 'ota': False},
}


@pytest.fixture(scope='module')
def web_mod():
    mod = _load_ttbox_web()
    tmp = tempfile.mkdtemp(prefix='ttbox_freeauth_%d_' % os.getpid())
    mod.WEB_CREDENTIALS_PATH = os.path.join(tmp, 'web_credentials.json')
    yield mod
    shutil.rmtree(tmp, ignore_errors=True)


@pytest.fixture(autouse=True)
def fresh_state(web_mod, monkeypatch):
    """每用例：重置激活缓存；默认 mock 成已激活 + core 可达。"""
    web_mod._ACTIVATION_CACHE['ts'] = 0.0
    monkeypatch.setattr(web_mod, '_license_block', lambda: dict(ACTIVATED_LICENSE))
    monkeypatch.setattr(web_mod, 'ipc_request',
                        lambda *a, **k: {'status': 0, 'data': {}})
    yield


@pytest.fixture
def client(web_mod):
    web_mod.app.config['TESTING'] = False
    return web_mod.app.test_client()


# ======================================================================
# ② grep 门禁：免密化拆除必须干净
# ======================================================================
def test_grep_gate_auth_machinery_removed():
    src = WEB_SRC.read_text(encoding='utf-8')
    for banned in ('first-setup', '_RL_ACTIVATE', 'scrypt', '_SESSION_COOKIE',
                   '_hash_password', 'api_auth_login'):
        assert banned not in src, f'免密化残留：{banned} 不应再出现在 ttbox-web.py'


def test_setup_login_pages_removed(client):
    assert client.get('/setup').status_code == 404
    assert client.get('/login').status_code == 404
    assert client.post('/api/auth/login', json={}).status_code == 404


# ======================================================================
# ① 免密直通矩阵（已激活态；无任何 cookie/凭据）
# ======================================================================
def test_apis_free_access_when_activated(client, web_mod, monkeypatch):
    # mock IPC：GET_STATUS/GET_CONFIG 返回骨架（api/state、/api/config 不依赖具体数据）
    monkeypatch.setattr(web_mod, 'ipc_request',
                        lambda *a, **k: {'status': 0, 'data': {'license': dict(ACTIVATED_LICENSE)}})
    assert client.get('/api/state').status_code == 200
    assert client.get('/api/config').status_code == 200
    assert client.get('/api/system').status_code == 200
    # 危险动作免密直通（dry_run 不真关机）
    assert client.post('/api/system/reboot', json={'dry_run': True}).status_code == 200
    assert client.post('/api/system/poweroff', json={'dry_run': True}).status_code == 200


def test_ota_install_free_access_with_capability(client, web_mod, monkeypatch, tmp_path):
    # capabilities.ota=true + mock 掉 OTA 环境（宿主机无 /var/lib/ttbox/ota/jobs）
    monkeypatch.setattr(web_mod, '_license_block', lambda: dict(ACTIVATED_LICENSE))
    jobs = tmp_path / 'jobs'
    jobs.mkdir()
    monkeypatch.setattr(web_mod, 'OTA_JOBS_DIR', str(jobs))
    monkeypatch.setattr(web_mod, 'OTA_UPDATER_PATH', WEB_SRC)
    r = client.post('/api/ota/install', json={'url': 'https://example.com/t.tar.gz'})
    assert r.status_code == 200
    assert r.get_json().get('ok') is True
    written = list(jobs.glob('job-*.json'))
    assert written, '已授权 + 环境齐备 ⇒ 任务文件必须落盘'
    job = json.loads(written[0].read_text(encoding='utf-8'))
    assert job['url'] == 'https://example.com/t.tar.gz'
    assert job['key_id'] == web_mod.OTA_DEFAULT_KEY_ID
    # 能力门控保留（M2.03 机制不动）：ota 未授权 ⇒ 403
    unlic = dict(ACTIVATED_LICENSE)
    unlic['capabilities'] = {'capture': True, 'inference': True, 'aim': True, 'ota': False}
    monkeypatch.setattr(web_mod, '_license_block', lambda: unlic)
    web_mod._ACTIVATION_CACHE['ts'] = 0.0
    assert client.post('/api/ota/install',
                       json={'url': 'https://example.com/t.tar.gz'}).status_code == 403


# ======================================================================
# ⑤ 未激活：API gate + 白名单 + 页面 302
# ======================================================================
def test_unactivated_api_gate_403_activation_required(client, web_mod, monkeypatch):
    monkeypatch.setattr(web_mod, '_license_block', lambda: dict(UNACTIVATED_LICENSE))
    web_mod._ACTIVATION_CACHE['ts'] = 0.0
    # 非白名单 API ⇒ 403 activation_required
    r = client.get('/api/state')
    assert r.status_code == 403
    assert r.get_json() == {'ok': False, 'error': 'activation_required'}
    assert client.get('/api/config').status_code == 403
    assert client.post('/api/system/reboot', json={'dry_run': True}).status_code == 403


def test_unactivated_whitelist_endpoints_reachable(client, web_mod, monkeypatch):
    monkeypatch.setattr(web_mod, '_license_block', lambda: dict(UNACTIVATED_LICENSE))
    web_mod._ACTIVATION_CACHE['ts'] = 0.0
    # D9 白名单（S1 减法后）：/api/license、/api/license/activate、/api/system
    # （/api/activation/network/prepare 与 /api/network/wifi* 已随网络页签移除而卸载）
    assert client.get('/api/license').status_code == 200
    assert client.get('/api/system').status_code == 200
    # 已卸载端点：未激活时 gate 在路由分发前拦截 ⇒ 403 activation_required
    # （关键是不可达 200；激活态下它们才是 404）
    r = client.post('/api/activation/network/prepare')
    assert r.status_code == 403 and r.get_json()['error'] == 'activation_required'
    assert client.get('/api/network/wifi').status_code == 403
    # 入参缺失 ⇒ 400（说明端点本身可达，未落 403 gate）
    assert client.post('/api/license/activate', json={}).status_code == 400


def test_unactivated_pages_redirect_to_activate(client, web_mod, monkeypatch):
    monkeypatch.setattr(web_mod, '_license_block', lambda: dict(UNACTIVATED_LICENSE))
    web_mod._ACTIVATION_CACHE['ts'] = 0.0
    for path in ('/', '/desktop', '/mobile'):
        r = client.get(path)
        assert r.status_code == 302, f'{path} 未激活应 302'
        assert r.headers['Location'].endswith('/activate')
    # 激活页本身可达（无凭据检查）
    assert client.get('/activate').status_code == 200


def test_activated_pages_render(client, web_mod, monkeypatch):
    assert client.get('/').status_code == 200
    assert client.get('/desktop').status_code == 200
    assert client.get('/mobile').status_code == 200


# ======================================================================
# ④ LAN 黑名单 403 执法保留
# ======================================================================
def test_blocklist_removed(client):
    """D04（2026-09-18 定案 §2.5）：局域网黑名单整块下线——端点必须 404。

    旧断言（blocked IP ⇒ 403）随功能删除失效：名单来源脚本从未进 payload，
    名单恒空 ⇒ fail-open，坏掉的安全功能比没有更危险。
    """
    r = client.get('/api/system/lan-blocklist')
    assert r.status_code == 404
    assert client.post('/api/system/lan-blocklist/scan').status_code == 404
    assert client.delete('/api/system/lan-blocklist').status_code == 404


# ======================================================================
# ③ web_credentials.json 退役（D10）
# ======================================================================
def test_credentials_retired_on_startup(web_mod):
    path = web_mod.WEB_CREDENTIALS_PATH
    retired = path + '.retired'
    for p in (path, retired):
        if os.path.exists(p):
            os.remove(p)
    with open(path, 'w', encoding='utf-8') as f:
        json.dump({'algo': 'scrypt'}, f)
    web_mod._retire_web_credentials()
    assert not os.path.exists(path)
    assert os.path.exists(retired)
    # 不存在时不炸、不产生副作用
    web_mod._retire_web_credentials()
    # 代码不再读取凭据（不存在任何 load/verify 调用）
    src = WEB_SRC.read_text(encoding='utf-8')
    assert '_load_credentials' not in src and '_verify_password' not in src


# ======================================================================
# ⑥ /api/update/* 薄映射
# ======================================================================
def test_update_thin_endpoints(client, web_mod, monkeypatch, tmp_path):
    monkeypatch.setattr(web_mod, '_license_block', lambda: dict(ACTIVATED_LICENSE))
    web_mod._ACTIVATION_CACHE['ts'] = 0.0
    jobs = tmp_path / 'jobs'
    jobs.mkdir()
    monkeypatch.setattr(web_mod, 'OTA_JOBS_DIR', str(jobs))
    monkeypatch.setattr(web_mod, 'OTA_UPDATER_PATH', WEB_SRC)
    r = client.post('/api/update/install', json={'url': 'https://example.com/t.tar.gz'})
    assert r.status_code == 200
    assert r.get_json().get('ok') is True
    assert list(jobs.glob('job-*.json')), 'install 薄映射应与 /api/ota/install 同义（写任务文件）'
    r2 = client.get('/api/update/status')
    assert r2.status_code == 200
    body = r2.get_json()
    assert body['ok'] is True and 'status' in body['data']
    # 2026-09-18 定案：versions 死端点已删（404）；check 仅 POST
    assert client.get('/api/update/versions').status_code == 404
    assert client.get('/api/update/check').status_code == 405
    # check：地址含 example.com（占位）⇒ 503 ota_server_not_configured（无假壳）。
    # 正式值（2026-09-19）已是真地址，这里 patch 回占位值验证 fail-closed，
    # 测试不碰真服务器。
    monkeypatch.setattr(web_mod, 'OTA_SERVER_URL', 'https://ota.ttbox.example.com')
    r3 = client.post('/api/update/check')
    assert r3.status_code == 503
    assert r3.get_json().get('error') == 'ota_server_not_configured'
    # 非 https scheme 同样 fail-closed（定案：更新器只认 https，不发起下载）
    monkeypatch.setattr(web_mod, 'OTA_SERVER_URL', 'http://cctv2.top:10086/ota')
    r4 = client.post('/api/update/check')
    assert r4.status_code == 503
    assert r4.get_json().get('error') == 'ota_server_not_configured'


def test_ota_server_url_two_places_in_sync():
    """单点纪律（2026-09-18 定案 §三 D 组）：OTA 地址两处写死、必须同值。

    ttbox.sh 与 ttbox-web.py 各写一份，板端由 `ttbox.sh doctor` 比对；
    Windows 侧先用这条钉住——改了一处漏另一处，本用例立刻红。
    同时要求：https、且不再是 example.com 占位值。
    """
    import re

    sh = (REPO_ROOT / 'scripts' / 'ttbox.sh').read_text(encoding='utf-8')
    web = WEB_SRC.read_text(encoding='utf-8')
    m_sh = re.search(r'^OTA_SERVER_URL="([^"]+)"', sh, re.M)
    m_web = re.search(r"^OTA_SERVER_URL = '([^']+)'", web, re.M)
    assert m_sh and m_web, '两处 OTA_SERVER_URL 定义都必须存在'
    assert m_sh.group(1) == m_web.group(1), (
        f'OTA 服务器地址漂移：ttbox.sh={m_sh.group(1)!r} ttbox-web.py={m_web.group(1)!r}')
    url = m_sh.group(1)
    assert url.startswith('https://'), f'必须是 https：{url}'
    assert 'example.com' not in url, '仍是占位值——正式包不允许'


# ======================================================================
# ⑦ 激活双保险（T03）：服务端 fail-closed 不变量 + /activate 卡片 1:1 + 面板兜底弹层
# ======================================================================
TEMPLATES_DIR = REPO_ROOT / 'plugins' / 'web' / 'templates'


def test_activation_gate_invariants(web_mod):
    """服务端执法不变量（语义级）：/ 必须在 _ACTIVATION_PAGES；/api/state 不得进白名单。

    这是「双保险」的根基 —— 无论前端弹层怎么改，服务端始终 fail-closed：
      页面 /、/desktop、/mobile ⇒ 302 /activate；非白名单 API ⇒ 403 activation_required。
    若未来有人把 / 从 _ACTIVATION_PAGES 移出、或把 /api/state 加白名单，本用例立刻变红。
    """
    assert '/' in web_mod._ACTIVATION_PAGES
    assert '/desktop' in web_mod._ACTIVATION_PAGES
    assert '/mobile' in web_mod._ACTIVATION_PAGES
    # /api/state 是面板主数据口，绝不因未激活而放行（否则整张面板对未授权客户端可达）
    assert ('GET', '/api/state') not in web_mod._ACTIVATION_WHITELIST
    assert not any(p.startswith('/api/state') for p in web_mod._ACTIVATION_WHITELIST_PREFIXES)


def test_activation_pages_literal_is_locked():
    """源码级锁：_ACTIVATION_PAGES 字面量必须逐字保留（防止被悄悄改写）。"""
    src = WEB_SRC.read_text(encoding='utf-8')
    assert "_ACTIVATION_PAGES = frozenset({'/', '/desktop', '/mobile'})" in src


def test_activate_page_replicates_license_gate_card(client, web_mod, monkeypatch):
    """激活页 = 参照物 #licenseGateOverlay 卡片 1:1（结构 + 文案 + 输入 + 双按钮 + 端点）。

    需先置为「未激活」——否则 /activate 会 302 回面板（已激活直进系统）。
    """
    monkeypatch.setattr(web_mod, '_license_block', lambda: dict(UNACTIVATED_LICENSE))
    web_mod._ACTIVATION_CACHE['ts'] = 0.0
    r = client.get('/activate')
    assert r.status_code == 200
    html = r.get_data(as_text=True)
    for marker in ('id="licenseGateOverlay"', '设备授权', '输入激活码后继续使用',
                   'id="licenseGateKeyInput"', 'id="licenseGateActivateButton"',
                   'id="licenseGateRefreshButton"', '/api/license/activate'):
        assert marker in html, f'激活页缺少 {marker}'


def test_panel_template_keeps_license_overlay_fallback():
    """面板保留上游 #licenseGateOverlay 标记 + 锁导航的JS（运行期掉线/被撤销时就地兜底）。

    ★ 2026-10-03 面板外链后：DOM 标记在 index.html，
      而 `setLicenseNavigationLock` 这个**函数**随脚本搬到了 static/panel.js。
      ⇒ 分开查，别再一个字符串里找两个文件的东西。
    """
    from plugins.web.tests import panel_src
    html = panel_src.html_src()
    js = panel_src.js_src()
    assert 'id="licenseGateOverlay"' in html
    assert 'setLicenseNavigationLock' in js


def test_unactivated_double_insurance_302_and_403(client, web_mod, monkeypatch):
    """双保险：未激活时页面 302、API 403 —— 与前端弹层无关（devtools 删弹层也绕不过）。"""
    monkeypatch.setattr(web_mod, '_license_block', lambda: dict(UNACTIVATED_LICENSE))
    web_mod._ACTIVATION_CACHE['ts'] = 0.0
    assert client.get('/').status_code == 302
    r = client.get('/api/state')
    assert r.status_code == 403 and r.get_json()['error'] == 'activation_required'
    # 激活页自身恒可达（服务端引导入口）
    assert client.get('/activate').status_code == 200
