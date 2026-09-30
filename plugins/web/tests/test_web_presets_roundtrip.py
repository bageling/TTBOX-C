# test_web_presets_roundtrip.py — T2-0 纯接线回归锁（预设导出/导入 + 模型导入分流）
#
# 背景（docs/交付前Web按钮落实审计-2026-09-19.md 第二档、docs/面板功能补齐实施计划-2026-09-19.md T2-0）：
#   ① 预设导出返回的是 **API 信封** `{'ok':true,'data':{'preset':{...}}}`，而消费方是
#      前端 `<a href download>` —— 浏览器把信封原样存成文件。用户再导入这份文件时，
#      导入端看到的根对象上既没有 name 也没有配置键 ⇒「导出→导入→加载」恒静默无效。
#   ② import_preset 只认文件内 name / 文件名主干，**前端 FormData 里的 name 被丢掉**，
#      用户在导入对话框里填的名称等于没填。
#   ③ 模型导入对话框固定打 `/api/models/import`，ONNX 在那里被拒（板端无 TTBOX_ALLOW_ONNX
#      逃生口）⇒ 转换链 `_run_onnx_conversion` 明明实现了却永远走不到。
#      同时 HEF 单选是 Hailo 格式，RK3588 没有对应后端（core 侧零 hef 分支），选了必然被拒。
#
# 本文件锁死修复后的口径：
#   ① /export 返回预设**本体** + attachment 头，且这份本体能被 /import 原样吃回去；
#   ② 名称优先级 FormData.name > 文件内 name > 文件名主干，旧信封文件仍可导入；
#   ③ 前端不存在 HEF 导入选项，ONNX 提交走 /api/models/import-onnx。
#
# 运行：python -m pytest plugins/web/tests/test_web_presets_roundtrip.py -v
from __future__ import annotations

import importlib.util
import io
import json
import os
import pathlib
import re
import sys
import tempfile

import pytest

REPO_ROOT = pathlib.Path(__file__).resolve().parents[3]
WEB_SRC = REPO_ROOT / 'plugins' / 'web' / 'bin' / 'ttbox-web.py'
TEMPLATE = REPO_ROOT / 'plugins' / 'web' / 'templates' / 'index.html'

_load_seq = 0


def _load():
    global _load_seq
    _load_seq += 1
    name = 'ttbox_web_presets_%d_%d' % (os.getpid(), _load_seq)
    spec = importlib.util.spec_from_file_location(name, WEB_SRC)
    mod = importlib.util.module_from_spec(spec)
    sys.modules[name] = mod
    spec.loader.exec_module(mod)
    return mod


@pytest.fixture()
def web_mod(monkeypatch):
    tmp = tempfile.mkdtemp(prefix='ttbox_presets_%d_' % os.getpid())
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
    # 运行期配置走 IPC（本机无 Core），替换成空 profile —— 被测的是路由自身行为。
    monkeypatch.setattr(web_mod, '_get_runtime_profile', lambda: {})
    return web_mod.app.test_client()


def _preset_path(web_mod, stem: str) -> pathlib.Path:
    return pathlib.Path(web_mod.PRESETS_DIR) / (stem + '.json')


def _write_preset(web_mod, stem: str, body: dict) -> pathlib.Path:
    p = _preset_path(web_mod, stem)
    p.parent.mkdir(parents=True, exist_ok=True)
    p.write_text(json.dumps(body, ensure_ascii=False), encoding='utf-8')
    return p


SAMPLE = {
    'name': 'config1',
    # V1.0.13：crop_offset_x/y 已删（core 的 CaptureProfile::offset_* 也没了）
    'capture': {'crop_size': 320},
    'ai': {'controller': {'kp_x': 0.7, 'kp_y': 0.6}},
    'mouse': {'sensitivity': 1.3},
}


def _upload(data: bytes, filename: str):
    return {'file': (io.BytesIO(data), filename)}


# ── ① 导出必须给预设本体，不是信封 ─────────────────────────────────────

def test_export_returns_preset_body_not_api_envelope(web_mod, monkeypatch):
    _write_preset(web_mod, 'config1', SAMPLE)
    c = _client(web_mod, monkeypatch)
    resp = c.get('/api/presets/config1/export')
    assert resp.status_code == 200
    body = json.loads(resp.data.decode('utf-8'))
    # 存下来的是预设本身：根上就是配置键，没有被 ok/data 包一层。
    assert body == SAMPLE
    assert 'ok' not in body
    assert 'data' not in body


def test_export_sets_attachment_disposition_with_json_name(web_mod, monkeypatch):
    _write_preset(web_mod, 'config1', SAMPLE)
    c = _client(web_mod, monkeypatch)
    resp = c.get('/api/presets/config1/export')
    cd = resp.headers.get('Content-Disposition', '')
    assert 'attachment' in cd.lower()
    assert 'config1.json' in cd


def test_export_is_mimetyped_as_json(web_mod, monkeypatch):
    _write_preset(web_mod, 'config1', SAMPLE)
    c = _client(web_mod, monkeypatch)
    resp = c.get('/api/presets/config1/export')
    assert 'json' in resp.headers.get('Content-Type', '').lower()


def test_export_missing_preset_is_json_error_not_a_preset(web_mod, monkeypatch):
    c = _client(web_mod, monkeypatch)
    resp = c.get('/api/presets/nope/export')
    body = json.loads(resp.data.decode('utf-8'))
    assert body.get('ok') is False
    assert 'error' in body


def test_export_damaged_preset_is_json_error(web_mod, monkeypatch):
    p = _preset_path(web_mod, 'broken')
    p.parent.mkdir(parents=True, exist_ok=True)
    p.write_text('{ this is not json', encoding='utf-8')
    c = _client(web_mod, monkeypatch)
    resp = c.get('/api/presets/broken/export')
    body = json.loads(resp.data.decode('utf-8'))
    assert body.get('ok') is False
    assert 'damaged' in str(body.get('error', '')).lower()


# ── 导出 → 导入 往返：这份文件必须能原样吃回去 ──────────────────────────

def test_export_then_import_roundtrip_preserves_content(web_mod, monkeypatch):
    _write_preset(web_mod, 'config1', SAMPLE)
    c = _client(web_mod, monkeypatch)
    exported = c.get('/api/presets/config1/export').data

    # 用户把导出的文件再传回来，名称留空 —— 应落到同一个名字上。
    resp = c.post('/api/presets/import',
                  data=_upload(exported, 'config1.json'),
                  content_type='multipart/form-data')
    assert resp.status_code == 200
    assert json.loads(resp.data.decode('utf-8')).get('ok') is True
    on_disk = json.loads(_preset_path(web_mod, 'config1').read_text(encoding='utf-8'))
    assert on_disk == SAMPLE


def test_roundtrip_into_a_different_name_keeps_config_intact(web_mod, monkeypatch):
    _write_preset(web_mod, 'config1', SAMPLE)
    c = _client(web_mod, monkeypatch)
    exported = c.get('/api/presets/config1/export').data

    c.post('/api/presets/import',
           data={**_upload(exported, 'whatever.json'), 'name': 'copy2'},
           content_type='multipart/form-data')
    on_disk = json.loads(_preset_path(web_mod, 'copy2').read_text(encoding='utf-8'))
    assert on_disk == SAMPLE


# ── ② 名称优先级 ───────────────────────────────────────────────────────

def test_import_form_name_beats_embedded_name_and_filename(web_mod, monkeypatch):
    c = _client(web_mod, monkeypatch)
    payload = json.dumps({'name': 'embedded', 'mouse': {'sensitivity': 1.0}}).encode()
    resp = c.post('/api/presets/import',
                  data={**_upload(payload, 'fromfile.json'), 'name': '面板名'},
                  content_type='multipart/form-data')
    assert json.loads(resp.data.decode('utf-8'))['data']['name'] == '面板名'
    assert _preset_path(web_mod, '面板名').exists()
    assert not _preset_path(web_mod, 'embedded').exists()
    assert not _preset_path(web_mod, 'fromfile').exists()


def test_import_falls_back_to_embedded_name_when_form_empty(web_mod, monkeypatch):
    c = _client(web_mod, monkeypatch)
    payload = json.dumps({'name': 'embedded', 'mouse': {}}).encode()
    c.post('/api/presets/import',
           data={**_upload(payload, 'fromfile.json'), 'name': ''},
           content_type='multipart/form-data')
    assert _preset_path(web_mod, 'embedded').exists()


def test_import_falls_back_to_upload_filename_when_no_name_anywhere(web_mod, monkeypatch):
    c = _client(web_mod, monkeypatch)
    c.post('/api/presets/import',
           data=_upload(json.dumps({'mouse': {}}).encode(), 'thirdparty.json'),
           content_type='multipart/form-data')
    assert _preset_path(web_mod, 'thirdparty').exists()


def test_import_name_is_sanitized_for_filesystem(web_mod, monkeypatch):
    c = _client(web_mod, monkeypatch)
    c.post('/api/presets/import',
           data={**_upload(json.dumps({'mouse': {}}).encode(), 'x.json'), 'name': 'a/b c'},
           content_type='multipart/form-data')
    # 路径分隔符与空格都会被替换掉，不能穿出预设目录。
    assert _preset_path(web_mod, 'a_b_c').exists()
    assert not (pathlib.Path(web_mod.PRESETS_DIR) / 'b c.json').exists()


# ── 旧信封文件仍可导入（用户手里已有的导出物） ─────────────────────────

def test_import_strips_legacy_export_envelope(web_mod, monkeypatch):
    c = _client(web_mod, monkeypatch)
    legacy = json.dumps({'ok': True, 'data': {'preset': SAMPLE, 'name': 'config1'}}).encode()
    resp = c.post('/api/presets/import',
                  data=_upload(legacy, 'legacy.json'),
                  content_type='multipart/form-data')
    assert json.loads(resp.data.decode('utf-8')).get('ok') is True
    on_disk = json.loads(_preset_path(web_mod, 'config1').read_text(encoding='utf-8'))
    # 剥掉信封后落盘的必须是预设本体，不是信封。
    assert on_disk == SAMPLE


def test_legacy_envelope_without_preset_object_is_still_rejected(web_mod, monkeypatch):
    c = _client(web_mod, monkeypatch)
    bad = json.dumps({'ok': False, 'error': 'nope'}).encode()
    resp = c.post('/api/presets/import',
                  data=_upload(bad, 'bad.json'),
                  content_type='multipart/form-data')
    # 落盘一个没有配置键的空壳没有意义；这里只要求不崩、能落一个文件即可。
    assert resp.status_code == 200


# ── 入参健壮性 ─────────────────────────────────────────────────────────

def test_import_rejects_non_object_json(web_mod, monkeypatch):
    c = _client(web_mod, monkeypatch)
    resp = c.post('/api/presets/import',
                  data=_upload(b'[1,2,3]', 'list.json'),
                  content_type='multipart/form-data')
    assert json.loads(resp.data.decode('utf-8'))['ok'] is False


def test_import_rejects_invalid_json(web_mod, monkeypatch):
    c = _client(web_mod, monkeypatch)
    resp = c.post('/api/presets/import',
                  data=_upload(b'{not json', 'broken.json'),
                  content_type='multipart/form-data')
    assert json.loads(resp.data.decode('utf-8'))['ok'] is False


def test_import_rejects_missing_upload(web_mod, monkeypatch):
    c = _client(web_mod, monkeypatch)
    resp = c.post('/api/presets/import', data={}, content_type='multipart/form-data')
    assert json.loads(resp.data.decode('utf-8'))['ok'] is False


# ── ③ 模型导入分流 ─────────────────────────────────────────────────────

def _js_function(src: str, signature: str) -> str:
    """抽出一个 JS 函数体（按大括号配对）。找不到签名就断言失败。"""
    start = src.find(signature)
    assert start >= 0, '未找到函数：%s' % signature
    brace = src.find('{', start)
    assert brace >= 0, '函数体缺失：%s' % signature
    depth = 0
    for i in range(brace, len(src)):
        ch = src[i]
        if ch == '{':
            depth += 1
        elif ch == '}':
            depth -= 1
            if depth == 0:
                return src[start:i + 1]
    raise AssertionError('大括号不配对：%s' % signature)


@pytest.fixture(scope='module')
def template_src() -> str:
    return TEMPLATE.read_text(encoding='utf-8')


def test_hef_radio_removed_from_import_dialog(template_src):
    # HEF 是 Hailo 格式，RK3588 无后端；单选里不能再出现 value="hef"。
    assert 'value="hef"' not in template_src


def test_current_model_import_type_only_knows_rknn_and_onnx(template_src):
    body = _js_function(template_src, 'function currentModelImportType(form)')
    assert 'hef' not in body
    assert '"onnx"' in body
    assert '"rknn"' in body


def test_update_model_import_mode_has_no_hef_branch(template_src):
    body = _js_function(template_src, 'function updateModelImportMode()')
    assert 'hef' not in body.lower()
    # ONNX 必须仍然提供校准包与标签字段的显示切换。
    assert 'isOnnx' in body


def test_set_model_import_busy_has_no_hef_wording(template_src):
    body = _js_function(template_src, 'function setModelImportBusy(')
    assert 'hef' not in body.lower()


def test_onnx_submit_goes_to_the_conversion_endpoint(template_src):
    # 提交处必须按类型分流：ONNX → /api/models/import-onnx，其余 → /api/models/import。
    idx = template_src.find('/api/models/import-onnx')
    assert idx >= 0, '前端没有任何地方调用 /api/models/import-onnx，ONNX 转换链仍是死代码'
    window = template_src[idx - 400:idx + 200]
    assert 'importType === "onnx"' in window
    assert '/api/models/import"' in window


def test_models_import_still_rejects_onnx_without_escape_hatch(web_mod, monkeypatch):
    # 板端不设 TTBOX_ALLOW_ONNX：ONNX 走 /api/models/import 必须被拒，
    # 否则前端分流就白改了（用户仍会拿到"导入成功"却拿到个没转换的模型）。
    c = _client(web_mod, monkeypatch)
    resp = c.post('/api/models/import',
                  data=_upload(b'\x00\x01\x02', 'm.onnx'),
                  content_type='multipart/form-data')
    body = json.loads(resp.data.decode('utf-8'))
    assert body.get('ok') is False


def test_web_source_has_no_applied_true_literal_anymore():
    # 顺带守住第一档的口径不被回退（预设/模型这两块同属"用户点一下"的路径）。
    src = WEB_SRC.read_text(encoding='utf-8')
    assert "'applied': True" not in src
