# test_web_hotkey_guard.py — 热键保护（hotkey_guard）web 侧接线回归锁
#
# 背景（docs/交付前Web按钮落实审计-2026-09-19.md 第二档、docs/面板功能补齐实施计划-2026-09-19.md T2-0/T2-8）：
#   面板上「热键保护」的两个控件（开关 + 切换键）此前是**双向断链**：
#     - 保存：web_body_to_profile 里根本没有 hotkey_guard 这一块，前端提交的值被静默丢弃；
#     - 回读：profile_to_web 恒返 {'enabled': False, 'toggle_hotkey': 'middle'}，
#             不管 Core 里存的是什么；
#     - 状态：collect_web_state 的 state.aim.hotkeys_suspended 恒 False，
#             用户按了挂起键，面板徽标照样显示"未禁用"。
#
#   现在 Core 侧真的有了这个概念（MouseProfile.hotkey_guard + AimThread 的上升沿翻转 +
#   metrics.aim_hotkeys_suspended），web 侧必须把线接上：
#     保存 → mouse.hotkey_guard {enabled, toggle_hotkey(位掩码)}
#     回读 → 读 Core 真值，缺字段回落与 Core 结构体一致的默认值
#     状态 → state.aim.hotkeys_suspended 如实映射
#
# 运行：python -m pytest plugins/web/tests/test_web_hotkey_guard.py -v
from __future__ import annotations

import importlib.util
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
    name = 'ttbox_web_guard_%d_%d' % (os.getpid(), _load_seq)
    spec = importlib.util.spec_from_file_location(name, WEB_SRC)
    mod = importlib.util.module_from_spec(spec)
    sys.modules[name] = mod
    spec.loader.exec_module(mod)
    return mod


@pytest.fixture()
def web_mod(monkeypatch):
    tmp = tempfile.mkdtemp(prefix='ttbox_guard_%d_' % os.getpid())
    monkeypatch.setenv('TTBOX_PREFIX', tmp)
    monkeypatch.setenv('TTBOX_PRESETS_DIR', os.path.join(tmp, 'presets'))
    monkeypatch.setenv('TTBOX_CONFIG_DIR', os.path.join(tmp, 'config'))
    monkeypatch.setenv('TTBOX_MODELS_ROOT', os.path.join(tmp, 'models'))
    mod = _load()
    # 翻译层是纯函数，但 web_body_to_profile 会顺带读一次当前 profile 补 fov 字段。
    # 本机没有 Core，直接替换掉，避免测试挂在 IPC 超时上。
    monkeypatch.setattr(mod, '_activation_ok', lambda: True)
    monkeypatch.setattr(mod, '_get_runtime_profile', lambda: {})
    return mod


def _code_only() -> str:
    """去掉 `#` 注释尾巴 —— 注释里引用的旧字面量不算违规，代码里的才算。"""
    lines = []
    for ln in WEB_SRC.read_text(encoding='utf-8').split('\n'):
        idx = ln.find('#')
        lines.append(ln if idx < 0 else ln[:idx])
    return '\n'.join(lines)


# ── 保存方向：前端提交 → Core profile ─────────────────────────────────

def test_web_body_translates_hotkey_guard(web_mod):
    prof = web_mod.web_body_to_profile(
        {'hotkey_guard': {'enabled': True, 'toggle_hotkey': 'back'}})
    assert prof['mouse']['hotkey_guard'] == {'enabled': True, 'toggle_hotkey': 8}


def test_web_body_translates_disabled_guard(web_mod):
    prof = web_mod.web_body_to_profile(
        {'hotkey_guard': {'enabled': False, 'toggle_hotkey': 'middle'}})
    assert prof['mouse']['hotkey_guard'] == {'enabled': False, 'toggle_hotkey': 4}


def test_web_body_omits_hotkey_guard_when_not_submitted(web_mod):
    # 老前端/局部提交不带这一块时，不能凭空往 Core 塞一份默认值把用户配置覆盖掉。
    prof = web_mod.web_body_to_profile({'sens': 1.0})
    assert 'hotkey_guard' not in prof['mouse']


def test_web_body_toggle_hotkey_falls_back_to_middle_not_zero(web_mod):
    # 空字符串不能翻成位掩码 0：0 在 Core 里是"没有切换键"，等于把这功能悄悄关掉。
    prof = web_mod.web_body_to_profile({'hotkey_guard': {'toggle_hotkey': ''}})
    assert prof['mouse']['hotkey_guard']['toggle_hotkey'] == 4


def test_web_body_unknown_hotkey_name_falls_back_to_middle(web_mod):
    prof = web_mod.web_body_to_profile({'hotkey_guard': {'toggle_hotkey': 'side3'}})
    assert prof['mouse']['hotkey_guard']['toggle_hotkey'] == 4


def test_web_body_accepts_numeric_toggle_hotkey(web_mod):
    prof = web_mod.web_body_to_profile({'hotkey_guard': {'toggle_hotkey': 8}})
    assert prof['mouse']['hotkey_guard']['toggle_hotkey'] == 8


# ── 回读方向：Core profile → 前端控件 ─────────────────────────────────

def test_profile_to_web_reads_real_hotkey_guard(web_mod):
    prof = {'mouse': {'hotkey_guard': {'enabled': True, 'toggle_hotkey': 8}}}
    assert web_mod.profile_to_web(prof)['hotkey_guard'] == {
        'enabled': True, 'toggle_hotkey': 'back'}


def test_profile_to_web_defaults_match_core_struct_when_missing(web_mod):
    # 缺字段的默认值必须与 MouseTypes.hpp 的 HotkeyGuardConfig 一致，
    # 否则"面板显示的值"和"Core 实际用的值"又分家了。
    assert web_mod.profile_to_web({})['hotkey_guard'] == {
        'enabled': False, 'toggle_hotkey': 'middle'}


def test_profile_to_web_handles_null_hotkey_guard(web_mod):
    assert web_mod.profile_to_web({'mouse': {'hotkey_guard': None}})['hotkey_guard'] == {
        'enabled': False, 'toggle_hotkey': 'middle'}


def test_roundtrip_body_to_web_keeps_guard(web_mod):
    # 保存 → 回读 一圈，值必须原样回来（否则"改完保存、刷新又变回去"）。
    body = {'hotkey_guard': {'enabled': True, 'toggle_hotkey': 'forward'}}
    sent = web_mod.web_body_to_profile(body)['mouse']['hotkey_guard']
    back = web_mod.profile_to_web({'mouse': {'hotkey_guard': sent}})['hotkey_guard']
    assert back == {'enabled': True, 'toggle_hotkey': 'forward'}


# ── 状态方向：挂起真值必须如实上报 ───────────────────────────────────

def _collect(web_mod, monkeypatch, metrics: dict) -> dict:
    monkeypatch.setattr(web_mod, '_get_status', lambda: {
        'running': True, 'runtime_running': True, 'version': 'test', 'metrics': metrics})
    monkeypatch.setattr(web_mod, 'ipc_request', lambda *a, **k: {'status': 2, 'data': {}})
    monkeypatch.setattr(web_mod, '_license_block',
                        lambda: {'valid': False, 'ui_brand': None})
    return web_mod.collect_web_state()


def test_collect_web_state_reports_suspended_true(web_mod, monkeypatch):
    data = _collect(web_mod, monkeypatch, {'aim_hotkeys_suspended': True})
    assert data['data']['state']['aim']['hotkeys_suspended'] is True


def test_collect_web_state_reports_suspended_false(web_mod, monkeypatch):
    data = _collect(web_mod, monkeypatch, {'aim_hotkeys_suspended': False})
    assert data['data']['state']['aim']['hotkeys_suspended'] is False


def test_collect_web_state_suspended_defaults_false_when_core_silent(web_mod, monkeypatch):
    # 旧 Core 不上报该字段时，只能是 False（不能编造"已挂起"吓用户）。
    data = _collect(web_mod, monkeypatch, {})
    assert data['data']['state']['aim']['hotkeys_suspended'] is False


def test_collect_web_state_suspended_is_boolean_not_truthy_passthrough(web_mod, monkeypatch):
    data = _collect(web_mod, monkeypatch, {'aim_hotkeys_suspended': 1})
    assert data['data']['state']['aim']['hotkeys_suspended'] is True


# ── 源码级：旧撒谎口径不得回退 ───────────────────────────────────────

def test_web_source_no_longer_hardcodes_suspended_false():
    src = _code_only()
    assert "'hotkeys_suspended': False," not in src


def test_web_source_no_longer_hardcodes_hotkey_guard_defaults():
    src = _code_only()
    assert "'hotkey_guard': {'enabled': False, 'toggle_hotkey': 'middle'}" not in src
