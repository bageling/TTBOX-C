# test_web_license_caps.py — M2.03 前端接线：license 子块的 short_code + capabilities 投影
#
# 断链点（本轮之前）：
#   · core 已下发 license.capabilities.{capture,inference,aim,ota} 与 license.short_code，
#     但 Web 的 _license_block() **不透传** ⇒ 前端拿不到能力位 ⇒ 无法做功能可见性门控；
#   · /api/ota/install 无 feature 'ota' 门控 ⇒ 未授权卡也能调度 OTA（越权）。
#
# 本文件锁死的判据：
#   1 license 子块投影 short_code + capabilities（逐字段来自 IPC，Web 零推导）
#   2 core 不可达 ⇒ 诚实默认（short_code='' + 四项能力全 False，绝不"默认放行"）
#   3 IPC 的 capabilities 缺失/畸形 ⇒ fail-closed 全 False
#   4 /api/ota/install 未授权(ota=false) ⇒ 403 且**不调度** updater
#   5 /api/ota/install 已授权(ota=true) ⇒ 放行并调度
#   6 /api/state 与 /api/license 的短码/能力位同源（同一 _license_block 投影）
#
# 运行：python -m pytest plugins/web/tests/test_web_license_caps.py -v
# 说明：Python 侧用例，不并入 C++ ttbox_core_tests，不动三个 C++ 计数域。
import importlib.util
import os
import pathlib
import shutil
import sys
import tempfile

import pytest

from plugins.web.lib.paths import repo_root as _ttbox_repo_root  # noqa: E402  路径单点真源（A-PATH-3）
REPO_ROOT = pathlib.Path(_ttbox_repo_root())
WEB_SRC = REPO_ROOT / 'plugins' / 'web' / 'bin' / 'ttbox-web.py'

_ALL_CAPS = ('capture', 'inference', 'aim', 'ota')
_load_seq = 0


def _load_ttbox_web():
    global _load_seq
    _load_seq += 1
    name = 'ttbox_web_caps_%d_%d' % (os.getpid(), _load_seq)
    spec = importlib.util.spec_from_file_location(name, WEB_SRC)
    mod = importlib.util.module_from_spec(spec)
    sys.modules[name] = mod
    spec.loader.exec_module(mod)
    return mod


@pytest.fixture()
def web_mod():
    """每用例独立模块实例；凭据路径重定向到 PID 唯一临时目录（绝不碰 /etc/ttbox）。"""
    mod = _load_ttbox_web()
    cred_dir = tempfile.mkdtemp(prefix='ttbox_webcaps_%d_' % os.getpid())
    mod.CRED_PATH = os.path.join(cred_dir, 'web_credentials.json')
    yield mod
    shutil.rmtree(cred_dir, ignore_errors=True)


def _authed_client(mod):
    """M2.07（D1）：面板免密化后无鉴权层；激活 gate 判据 = _get_status().license.activated，
    由各用例对 _get_status 的打桩决定（activated=True 时放行到被测 handler）。"""
    mod.app.config['TESTING'] = False
    return mod.app.test_client()


def _license_status(**over):
    base = {'state': 'valid', 'activated': True, 'plan': 'subscription', 'is_pro': False,
            'features': ['capture', 'inference', 'aim', 'ota'], 'ui_brand': 'ttbox'}
    base.update(over)
    return {'license': base}


# ---------------------------------------------------------------------------
# 1. 投影
# ---------------------------------------------------------------------------
def test_license_block_projects_short_code_and_capabilities(web_mod, monkeypatch):
    monkeypatch.setattr(web_mod, '_get_status', lambda: _license_status(
        short_code='TTB-006Z-0C33',
        capabilities={'capture': True, 'inference': False, 'aim': False, 'ota': True}))
    blk = web_mod._license_block()
    assert blk['short_code'] == 'TTB-006Z-0C33'
    assert blk['capabilities'] == {'capture': True, 'inference': False, 'aim': False, 'ota': True}


# ---------------------------------------------------------------------------
# 2. core 不可达 ⇒ 诚实默认
# ---------------------------------------------------------------------------
def test_license_block_core_unreachable_is_honest_default(web_mod, monkeypatch):
    monkeypatch.setattr(web_mod, '_get_status', lambda: {})
    blk = web_mod._license_block()
    assert blk['short_code'] == ''
    assert blk['capabilities'] == {k: False for k in _ALL_CAPS}
    assert blk['activated'] is False


# ---------------------------------------------------------------------------
# 3. 畸形 IPC 能力位 ⇒ fail-closed
# ---------------------------------------------------------------------------
@pytest.mark.parametrize('bad_caps', ['nope', [], {}, None, 123])
def test_capabilities_malformed_is_fail_closed(web_mod, monkeypatch, bad_caps):
    monkeypatch.setattr(web_mod, '_get_status', lambda: _license_status(
        capabilities=bad_caps, short_code=None))
    blk = web_mod._license_block()
    assert blk['capabilities'] == {k: False for k in _ALL_CAPS}
    assert blk['short_code'] == ''


# ---------------------------------------------------------------------------
# 4 / 5. /api/ota/install 的 feature 'ota' 门控
# ---------------------------------------------------------------------------
def test_ota_install_403_when_not_licensed(web_mod, monkeypatch):
    client = _authed_client(web_mod)
    monkeypatch.setattr(web_mod, '_get_status', lambda: _license_status(
        capabilities={'capture': True, 'inference': True, 'aim': True, 'ota': False}))
    scheduled = []
    monkeypatch.setattr(web_mod.subprocess, 'Popen',
                        lambda *a, **k: scheduled.append(a))
    r = client.post('/api/ota/install', json={'url': 'https://example.com/u.bin'})
    assert r.status_code == 403
    assert "ota" in r.get_json().get('error', '')
    assert scheduled == [], '未授权卡不得调度 updater（否则越权升级）'


def test_ota_install_proceeds_when_licensed(web_mod, monkeypatch, tmp_path):
    client = _authed_client(web_mod)
    monkeypatch.setattr(web_mod, '_get_status', lambda: _license_status(
        capabilities={'capture': True, 'inference': True, 'aim': True, 'ota': True}))
    jobs = tmp_path / 'jobs'
    jobs.mkdir()
    monkeypatch.setattr(web_mod, 'OTA_JOBS_DIR', str(jobs))
    monkeypatch.setattr(web_mod, 'OTA_UPDATER_PATH', str(pathlib.Path(web_mod.__file__)))
    r = client.post('/api/ota/install', json={'url': 'https://example.com/u.bin'})
    assert r.status_code == 200
    assert r.get_json().get('ok') is True
    written = list(jobs.glob('job-*.json'))
    assert written, '已授权应落盘任务文件（写任务文件通道，非 Popen 调度）'


def test_ota_install_core_unreachable_is_403(web_mod, monkeypatch):
    """core 不可达 ⇒ capabilities 诚实默认全 False ⇒ fail-closed 403（不得放行）。"""
    client = _authed_client(web_mod)
    monkeypatch.setattr(web_mod, '_get_status', lambda: {})
    scheduled = []
    monkeypatch.setattr(web_mod.subprocess, 'Popen',
                        lambda *a, **k: scheduled.append(a))
    r = client.post('/api/ota/install', json={'url': 'https://example.com/u.bin'})
    assert r.status_code == 403
    assert scheduled == []


# ---------------------------------------------------------------------------
# 6. /api/state 与 /api/license 同源
# ---------------------------------------------------------------------------
def test_state_and_license_agree_on_short_code_and_caps(web_mod, monkeypatch):
    caps = {'capture': True, 'inference': False, 'aim': False, 'ota': False}
    monkeypatch.setattr(web_mod, '_get_status', lambda: _license_status(
        short_code='TTB-AAAA-BBBB', capabilities=caps))
    monkeypatch.setattr(web_mod, 'ipc_request', lambda *a, **k: {'status': 1, 'data': {}})
    monkeypatch.setattr(web_mod, '_get_runtime_profile', lambda: {})
    monkeypatch.setattr(web_mod, '_loopout_payload', lambda: {})

    lic = web_mod._license_payload()
    assert lic['license']['short_code'] == 'TTB-AAAA-BBBB'
    assert lic['license']['capabilities']['ota'] is False

    state = web_mod.collect_web_state()
    state_lic = state['data']['state']['license']
    assert state_lic['short_code'] == lic['license']['short_code']
    assert state_lic['capabilities'] == lic['license']['capabilities']


# ---------------------------------------------------------------------------
# 独立运行器（兼容既有 tests/ 风格）
# ---------------------------------------------------------------------------
if __name__ == '__main__':
    sys.exit(pytest.main([__file__, '-v']))
