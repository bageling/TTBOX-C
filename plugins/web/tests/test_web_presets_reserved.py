# test_web_presets_reserved.py — 预设目录里的「自动产物」不能被当成预设（2026-09-23 线上 500）
#
# 事故：切换模型后前端自动保存"当前预设"，面板报「预设保存失败」+ HTTP 500。
# 链路：
#   ① scripts/ttbox_dtb_fix.sh（root 跑）把 DTB 诊断报告写到 `/opt/ttbox/presets/_dtbfix.json`
#      （故意放预设目录，好让 `/api/presets/_dtbfix/export` 能读回）；文件 root:root 644，
#      目录 ttbox:ttbox ⇒ **面板（ttbox）可读不可写**。
#   ② 三处枚举都是裸 `glob('*.json')` ⇒ `_dtbfix` 被当成预设列进列表；
#      目录里恰好只有它一个 json ⇒ 前端把它当"当前预设"。
#   ③ 自动保存 POST /api/presets name=_dtbfix ⇒ write_text PermissionError ⇒ 未捕获 ⇒ 500。
#
# 本文件锁死修复口径：
#   ① `_` 开头是保留名：**不进**预设列表（/api/presets 与 /api/state、模型切换结果三处一致）；
#   ② 写入/改名保留名 ⇒ 明确错误 + ok:false，**绝不能是 500**；
#   ③ 保留名仍然**可读**（export 照旧工作）。
#
# 运行：python -m pytest plugins/web/tests/test_web_presets_reserved.py -v
from __future__ import annotations

import importlib.util
import json
import os
import pathlib
import sys
import tempfile

import pytest

from plugins.web.lib.paths import repo_root as _ttbox_repo_root  # noqa: E402  路径单点真源（A-PATH-3）
REPO_ROOT = pathlib.Path(_ttbox_repo_root())
WEB_SRC = REPO_ROOT / 'plugins' / 'web' / 'bin' / 'ttbox-web.py'

_load_seq = 0


def _load():
    global _load_seq
    _load_seq += 1
    name = 'ttbox_web_presets_reserved_%d_%d' % (os.getpid(), _load_seq)
    spec = importlib.util.spec_from_file_location(name, WEB_SRC)
    mod = importlib.util.module_from_spec(spec)
    sys.modules[name] = mod
    spec.loader.exec_module(mod)
    return mod


@pytest.fixture()
def web_mod(monkeypatch):
    tmp = tempfile.mkdtemp(prefix='ttbox_presets_res_%d_' % os.getpid())
    monkeypatch.setenv('TTBOX_PREFIX', tmp)
    monkeypatch.setenv('TTBOX_PRESETS_DIR', os.path.join(tmp, 'presets'))
    monkeypatch.setenv('TTBOX_CONFIG_DIR', os.path.join(tmp, 'config'))
    monkeypatch.setenv('TTBOX_MODELS_ROOT', os.path.join(tmp, 'models'))
    monkeypatch.delenv('TTBOX_ALLOW_ONNX', raising=False)
    mod = _load()
    pathlib.Path(mod.PRESETS_DIR).mkdir(parents=True, exist_ok=True)
    return mod


def _client(web_mod, monkeypatch):
    monkeypatch.setattr(web_mod, '_activation_ok', lambda: True)
    monkeypatch.setattr(web_mod, '_get_runtime_profile', lambda: {})
    return web_mod.app.test_client()


def _write_preset(web_mod, stem: str, body: dict) -> pathlib.Path:
    p = pathlib.Path(web_mod.PRESETS_DIR) / (stem + '.json')
    p.parent.mkdir(parents=True, exist_ok=True)
    p.write_text(json.dumps(body, ensure_ascii=False), encoding='utf-8')
    return p


# 复刻 scripts/ttbox_dtb_fix.sh 落的那份报告（非预设）
DTBFIX_REPORT = {
    'name': '_dtbfix',
    'note': 'TTBOX DTB 修复诊断报告（自动生成，非预设）',
    'good_sha': '277d9de8',
}


# ── ① 保留名不进预设列表 ────────────────────────────────────────────────

def test_list_presets_skips_reserved_names(web_mod, monkeypatch):
    _write_preset(web_mod, '_dtbfix', DTBFIX_REPORT)
    _write_preset(web_mod, 'mine', {'name': 'mine'})
    c = _client(web_mod, monkeypatch)

    resp = c.get('/api/presets')
    assert resp.status_code == 200
    assert json.loads(resp.data.decode('utf-8'))['data']['presets'] == ['mine']


def test_preset_names_helper_is_the_single_source(web_mod):
    """三处枚举必须都走 _preset_names()，不能再出现裸 glob('*.json')。"""
    src = WEB_SRC.read_text(encoding='utf-8')
    assert "Path(PRESETS_DIR).glob('*.json')" not in src


# ── ② 写保留名：明确错误，绝不能 500 ────────────────────────────────────

def test_save_reserved_name_returns_clear_error_not_500(web_mod, monkeypatch):
    _write_preset(web_mod, '_dtbfix', DTBFIX_REPORT)
    c = _client(web_mod, monkeypatch)

    resp = c.post('/api/presets', json={'name': '_dtbfix', 'config': {'a': 1}})
    assert resp.status_code == 200          # 不是 500：异常不得冒泡
    body = json.loads(resp.data.decode('utf-8'))
    assert body['ok'] is False
    assert body.get('error')
    # 原报告内容没被写坏
    still = json.loads((pathlib.Path(web_mod.PRESETS_DIR) / '_dtbfix.json').read_text(encoding='utf-8'))
    assert still == DTBFIX_REPORT


def test_rename_to_reserved_name_rejected(web_mod, monkeypatch):
    _write_preset(web_mod, 'mine', {'name': 'mine'})
    c = _client(web_mod, monkeypatch)

    resp = c.post('/api/presets', json={'name': 'mine', 'action': 'rename', 'new_name': '_dtbfix'})
    assert resp.status_code == 200
    body = json.loads(resp.data.decode('utf-8'))
    assert body['ok'] is False


def test_delete_reserved_name_rejected(web_mod, monkeypatch):
    _write_preset(web_mod, '_dtbfix', DTBFIX_REPORT)
    c = _client(web_mod, monkeypatch)

    resp = c.post('/api/presets', json={'name': '_dtbfix', 'action': 'delete'})
    assert resp.status_code == 200
    assert json.loads(resp.data.decode('utf-8'))['ok'] is False
    assert (pathlib.Path(web_mod.PRESETS_DIR) / '_dtbfix.json').exists()


# ── ③ 保留名仍然只读可用 ────────────────────────────────────────────────

def test_reserved_name_still_exportable(web_mod, monkeypatch):
    _write_preset(web_mod, '_dtbfix', DTBFIX_REPORT)
    c = _client(web_mod, monkeypatch)

    resp = c.get('/api/presets/_dtbfix/export')
    assert resp.status_code == 200
    assert json.loads(resp.data.decode('utf-8')) == DTBFIX_REPORT


# ── ④ 正常预设不受影响 ──────────────────────────────────────────────────

def test_unwritable_preset_file_reports_error_instead_of_500(web_mod, monkeypatch):
    """复刻线上情形的另一半：名字合法，但文件不可写（root 拥有、面板 ttbox 无写权限）。

    以前 write_text 抛 PermissionError 冒泡成 500，前端只有一句"预设保存失败"。
    """
    p = _write_preset(web_mod, 'locked', {'name': 'locked'})
    os.chmod(str(p), 0o444)
    c = _client(web_mod, monkeypatch)
    try:
        resp = c.post('/api/presets', json={'name': 'locked', 'config': {'a': 1}})
    finally:
        os.chmod(str(p), 0o644)
    assert resp.status_code == 200          # 不是 500
    body = json.loads(resp.data.decode('utf-8'))
    assert body['ok'] is False
    assert 'locked' in (body.get('error') or '')


def test_normal_preset_roundtrip_still_works(web_mod, monkeypatch):
    c = _client(web_mod, monkeypatch)
    resp = c.post('/api/presets', json={'name': 'cfg1', 'config': {'x': 1}})
    assert resp.status_code == 200
    assert json.loads(resp.data.decode('utf-8'))['ok'] is True
    assert json.loads(c.get('/api/presets').data.decode('utf-8'))['data']['presets'] == ['cfg1']
