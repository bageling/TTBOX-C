# -*- coding: utf-8 -*-
"""V1.0.23「只瞄上半身」配置透传与面板开关测试。

★ 与 test_web_aim_head_box.py 的差别：那份只做**源码级**断言（"这段文本存在"），
  这份**真调** profile_to_web / web_body_to_profile 两个函数验往返 ——
  「面板勾了能不能存进 core 配置、存进去再读出来还在」才是真契约。
  源码级只保留面板 DOM/JS 那三处（它们本来就没有可调函数，只能查文本）。

运行：python -m pytest plugins/web/tests/test_web_upper_body.py -v
"""
from __future__ import annotations

import importlib.util
import json
import os
import pathlib
import sys
import tempfile

import pytest

from plugins.web.lib.paths import repo_root as _ttbox_repo_root  # noqa: E402
REPO_ROOT = pathlib.Path(_ttbox_repo_root())
WEB_SRC = REPO_ROOT / 'plugins' / 'web' / 'bin' / 'ttbox-web.py'

_load_seq = 0


def _load():
    global _load_seq
    _load_seq += 1
    name = 'ttbox_web_upper_%d_%d' % (os.getpid(), _load_seq)
    spec = importlib.util.spec_from_file_location(name, WEB_SRC)
    mod = importlib.util.module_from_spec(spec)
    sys.modules[name] = mod
    spec.loader.exec_module(mod)
    return mod


@pytest.fixture()
def web_mod(monkeypatch):
    """加载 ttbox-web.py，运行期路径全指到临时目录（不碰开发机真实 FHS）。"""
    tmp = tempfile.mkdtemp(prefix='ttbox_upper_%d_' % os.getpid())
    monkeypatch.setenv('TTBOX_PREFIX', tmp)
    monkeypatch.setenv('TTBOX_PRESETS_DIR', os.path.join(tmp, 'presets'))
    monkeypatch.setenv('TTBOX_CONFIG_DIR', os.path.join(tmp, 'config'))
    monkeypatch.setenv('TTBOX_MODELS_ROOT', os.path.join(tmp, 'models'))
    monkeypatch.setenv('TTBOX_MOTION_PROFILES_DIR', os.path.join(tmp, 'config', 'motion-profiles'))
    mod = _load()
    monkeypatch.setattr(mod, '_get_runtime_profile', lambda: _base_profile())
    return mod


def _base_profile():
    return {
        'capture': {'width': 640, 'height': 640},
        'inference': {'confidence': 0.5, 'iou': 0.45, 'class_filter': []},
        'mouse': {'kp_x': 0.5, 'kp_y': 0.5, 'sensitivity': 1.25,
                  'upper_body_enabled': True, 'upper_body_ratio': 0.55},
        'fov': {'shape': 0, 'center_x': 0.5, 'center_y': 0.5, 'enabled': True, 'radius': 0.4},
        'model_id': 'model-A',
    }


# ---------------------------------------------------------------- core → 面板
def test_profile_to_web_exposes_both_keys(web_mod):
    web = web_mod.profile_to_web(json.loads(json.dumps(_base_profile())))
    ctrl = (web.get('ai') or {}).get('controller') or {}
    assert ctrl.get('upper_body_enabled') is True, (
        '面板读不到 upper_body_enabled ⇒ 开关永远显示未勾选（勾了也会被刷掉）：%r'
        % ctrl.get('upper_body_enabled'))
    assert ctrl.get('upper_body_ratio') == pytest.approx(0.55)


def test_profile_to_web_defaults_are_off(web_mod):
    """老配置没有这两个键时，面板必须显示「关 + 0.5」——
    默认开 = 升级即改现网所有用户的瞄准行为。"""
    prof = _base_profile()
    prof['mouse'].pop('upper_body_enabled')
    prof['mouse'].pop('upper_body_ratio')
    web = web_mod.profile_to_web(json.loads(json.dumps(prof)))
    ctrl = (web.get('ai') or {}).get('controller') or {}
    assert ctrl.get('upper_body_enabled') is False
    assert ctrl.get('upper_body_ratio') == pytest.approx(0.5)


# ---------------------------------------------------------------- 面板 → core
def test_web_body_to_profile_writes_both_keys(web_mod):
    out = web_mod.web_body_to_profile(
        {'ai': {'controller': {'upper_body_enabled': True, 'upper_body_ratio': 0.45}}})
    mouse = out['mouse']
    assert mouse.get('upper_body_enabled') is True
    assert mouse.get('upper_body_ratio') == pytest.approx(0.45)


def test_panel_false_does_not_leak_true(web_mod):
    """★ 反向陷阱：面板取消勾选时若后端"只在为真时写"，
    老配置里的 true 会永远留着 ⇒ 用户关不掉这个开关。"""
    prof = json.loads(json.dumps(_base_profile()))  # mouse.upper_body_enabled = True
    prof = web_mod.web_body_to_profile(
        {'ai': {'controller': {'upper_body_enabled': False}}}, prev_profile=prof)
    assert prof['mouse'].get('upper_body_enabled') is False, (
        '取消勾选必须能写回 false（"为真才写"的实现会让旧 true 赖着不走）')


def test_round_trip_keeps_value(web_mod):
    """core → 面板 → core 一圈，值必须原样回来。"""
    web = web_mod.profile_to_web(json.loads(json.dumps(_base_profile())))
    ctrl = (web.get('ai') or {}).get('controller') or {}
    body = {'ai': {'controller': {k: ctrl[k] for k in
                                  ('upper_body_enabled', 'upper_body_ratio') if k in ctrl}}}
    back = web_mod.web_body_to_profile(body)
    assert back['mouse']['upper_body_enabled'] is True
    assert back['mouse']['upper_body_ratio'] == pytest.approx(0.55)


# ---------------------------------------------------------------- 面板 DOM/JS
def _panel_text() -> str:
    root = REPO_ROOT / 'plugins' / 'web'
    parts = [root / 'templates' / 'index.html', root / 'static' / 'panel.css',
             root / 'static' / 'panel' / '00-const.js',
             root / 'static' / 'panel' / '10-flow.js']
    return '\n'.join(p.read_text(encoding='utf-8') for p in parts)


def test_panel_has_switch():
    text = _panel_text()
    assert 'id="controller_upper_body_enabled"' in text
    idx = text.index('id="controller_upper_body_enabled"')
    assert 'data-config' in text[idx:idx + 120] and 'type="checkbox"' in text[idx:idx + 120]


def test_panel_render_and_collect_wired():
    text = _panel_text()
    assert 'setCheckbox("controller_upper_body_enabled", controller.upper_body_enabled' in text, (
        '渲染时没把后端值灌进开关 ⇒ 打开面板永远显示未勾选')
    assert 'upper_body_enabled: getCheckbox("controller_upper_body_enabled")' in text, (
        '保存时没读开关 ⇒ 勾了也存不进去')


def test_panel_default_is_off():
    text = _panel_text()
    assert 'upper_body_enabled: false' in text
    assert 'upper_body_ratio: 0.5' in text
