# test_web_config_crop_normalize.py — P0-1 回归锁：crop_size 归一化 + /api/config 错误分流
#
# 板端实测（2026-09-19，见 docs/交付前Web实测报告-2026-09-19.md P0-1）：
#   面板"保存配置"全部失败（HTTP 503，文案"Core 未运行"）。真实链路：
#     · 面板"截取尺寸"输入框 min=1，而 profile 里的全帧值 0 回填后被夹到 1
#       ⇒ 提交 capture.crop_size=1 ⇒ Web 译成 capture.width=1；
#     · Core 的 RuntimeProfile::validate（core/src/app/Application.cpp）只接受
#       {0（全帧）} ∪ [64, 3840]，1 落在域外 ⇒ 拒收**整份** SET_CONFIG
#       （原话："profile 校验失败: capture.width 非法（0=全帧，或需在 64~3840 之间）"）；
#     · Web 把 IPC status=1（参数错）误写成"Core 未运行"，真实原因被吞 ⇒ 排查跑偏。
#   本文件锁死两层修复：
#     ① 值域归一化（<=0 → 0；1~63 → 64；>3840 → 3840）——提交给 Core 的 profile 永远合法；
#     ② 错误分流（status==3 ⇒ 503 Core 离线；status∈{1,2,4} ⇒ 400 透传 Core 原话），
#        绝不再把"配置被 Core 拒绝"说成"Core 未运行"。
#
# 归一化规则的前后端同值约定：本文件 ↔ index.html::normalizeCropSize。
# 后端**故意**不用前端的下界 1：那是"夹出来"的非法值，正是它触发了整份拒收。
#
# 运行：python -m pytest plugins/web/tests/test_web_config_crop_normalize.py -v
import importlib.util
import json
import os
import pathlib
import sys
import tempfile

import pytest

REPO_ROOT = pathlib.Path(__file__).resolve().parents[3]
WEB_SRC = REPO_ROOT / 'plugins' / 'web' / 'bin' / 'ttbox-web.py'

_load_seq = 0

# Core 侧合法域（core/src/app/Application.cpp::RuntimeProfile::validate）
CORE_MIN_OCCUPIED = 64
CORE_MAX_OCCUPIED = 3840


def _load():
    global _load_seq
    _load_seq += 1
    name = 'ttbox_web_cropnorm_%d_%d' % (os.getpid(), _load_seq)
    spec = importlib.util.spec_from_file_location(name, WEB_SRC)
    mod = importlib.util.module_from_spec(spec)
    sys.modules[name] = mod
    spec.loader.exec_module(mod)
    return mod


@pytest.fixture()
def web_mod(monkeypatch):
    """加载 ttbox-web.py，并把全部运行期路径指到临时目录（不碰开发机真实 FHS）。"""
    tmp = tempfile.mkdtemp(prefix='ttbox_cropnorm_%d_' % os.getpid())
    monkeypatch.setenv('TTBOX_PREFIX', tmp)
    monkeypatch.setenv('TTBOX_PRESETS_DIR', os.path.join(tmp, 'presets'))
    monkeypatch.setenv('TTBOX_CONFIG_DIR', os.path.join(tmp, 'config'))
    monkeypatch.setenv('TTBOX_MODELS_ROOT', os.path.join(tmp, 'models'))
    monkeypatch.setenv('TTBOX_MOTION_PROFILES_DIR', os.path.join(tmp, 'config', 'motion-profiles'))
    return _load()


def _base_profile(**capture_overrides):
    """一份"Core 当前运行配置"的替身：capture 默认全帧（0）。"""
    cap = {'width': 0, 'height': 0, 'offset_x': 0, 'offset_y': 0}
    cap.update(capture_overrides)
    return {
        'capture': cap,
        'inference': {'confidence': 0.5, 'iou': 0.45},
        'mouse': {'kp_x': 0.5, 'kp_y': 0.5},
        'fov': {'shape': 0, 'center_x': 0.5, 'center_y': 0.5, 'enabled': False, 'radius': 0.5},
        'model_id': 'model-A',
    }


def _client(web_mod, monkeypatch, profile=None, ipc_result=None, recorder=None):
    """装配一个把 IPC 与"当前配置"都替换掉的 test client；recorder 收下 SET_CONFIG 实参。

    同时放行入口 gate（`_enforce_gate` 要求已激活才可达 /api/config、/api/presets/*）：
    这里替换的是**激活态判定**这一个模块边界，被执行的仍是产品里的 update_config /
    load_preset 本体，与板端"已激活"时走的代码路径一致。
    """
    def fake_ipc(req_type, params=None, timeout=5):
        if recorder is not None:
            recorder['req_type'] = req_type
            recorder['params'] = params
        return dict(ipc_result or {'status': 0, 'data': {}})

    monkeypatch.setattr(web_mod, 'ipc_request', fake_ipc)
    monkeypatch.setattr(web_mod, '_get_runtime_profile',
                        lambda: json.loads(json.dumps(profile if profile is not None else _base_profile())))
    monkeypatch.setattr(web_mod, '_activation_ok', lambda: True)
    return web_mod.app.test_client()


# ===========================================================================
# 1. 纯函数：值域归一化（最底层契约）
# ===========================================================================

@pytest.mark.parametrize('raw,expected', [
    # 全帧 / 非法输入一律 0
    (0, 0), (-1, 0), (-1920, 0), (0.4, 0), (None, 0), ('', 0), ('abc', 0), ([], 0), ({}, 0),
    # 1~63 是"夹出来"的非法值区，一律抬到 Core 下界
    (1, 64), (2, 64), (63, 64), (63.4, 64),
    # 合法域内原样保留
    (64, 64), (65, 65), (192, 192), (320, 320), ('416', 416), (1080, 1080), (3840, 3840),
    # 超上限一律压到 Core 上界
    (3841, 3840), (4000, 3840), (10 ** 9, 3840),
])
def test_normalize_capture_crop_size_never_emits_illegal_value(web_mod, raw, expected):
    got = web_mod.normalize_capture_crop_size(raw)
    assert got == expected, f'raw={raw!r}'
    # 输出必须恒在 Core 合法域内：要么 0（全帧），要么 [64, 3840]
    assert got == 0 or CORE_MIN_OCCUPIED <= got <= CORE_MAX_OCCUPIED, f'raw={raw!r} → {got}'


def test_normalize_capture_crop_size_is_idempotent(web_mod):
    for raw in (0, 1, 63, 64, 320, 4000, None):
        once = web_mod.normalize_capture_crop_size(raw)
        assert web_mod.normalize_capture_crop_size(once) == once


def test_normalize_profile_pulls_both_dims_into_legal_domain(web_mod):
    """存量 Core 配置里若已残留 width=height=1，保存别的字段时也必须把它带回合法域。"""
    p = web_mod.normalize_profile_capture_size({'capture': {'width': 1, 'height': 1}})
    assert p['capture'] == {'width': 64, 'height': 64}


def test_normalize_profile_keeps_zero_as_full_frame(web_mod):
    p = web_mod.normalize_profile_capture_size({'capture': {'width': 0, 'height': 0}})
    assert p['capture'] == {'width': 0, 'height': 0}


def test_normalize_profile_does_not_invent_missing_keys(web_mod):
    """只归一化**已存在**的键；凭空补 height 会把"未裁剪"语义改写成一次裁剪。"""
    p = web_mod.normalize_profile_capture_size({'capture': {'width': 1}})
    assert p['capture'] == {'width': 64}


def test_normalize_profile_tolerates_shapeless_input(web_mod):
    assert web_mod.normalize_profile_capture_size(None) is None
    assert web_mod.normalize_profile_capture_size({'capture': None}) == {'capture': None}
    assert web_mod.normalize_profile_capture_size({'mouse': {}}) == {'mouse': {}}
    assert web_mod.normalize_profile_capture_size({}) == {}


# ===========================================================================
# 2. 翻译层：面板体 → RuntimeProfile
# ===========================================================================

def test_body_crop_size_1_becomes_64_and_removed_offsets_are_ignored(web_mod, monkeypatch):
    """面板提交 1（全帧被前端夹出来的值）⇒ 必须落成 64。

    ★ V1.0.13（2026-09-30）：crop_offset_x/y 已删（core 的 CaptureProfile::offset_* 也没了，
    裁剪区恒以画面中心为心）。老页面 / 老预设里若还带这两个键，翻译层必须**静默忽略**
    —— 写进 profile 就是"看起来生效、其实没人读"的死接线。
    """
    monkeypatch.setattr(web_mod, '_get_runtime_profile', lambda: _base_profile())
    prof = web_mod.web_body_to_profile(
        {'capture': {'crop_size': 1, 'crop_offset_x': 10, 'crop_offset_y': 20}})
    assert prof['capture']['width'] == 64
    assert prof['capture']['height'] == 64
    assert 'offset_x' not in prof['capture']
    assert 'offset_y' not in prof['capture']


def test_body_crop_size_0_stays_full_frame(web_mod, monkeypatch):
    monkeypatch.setattr(web_mod, '_get_runtime_profile', lambda: _base_profile())
    prof = web_mod.web_body_to_profile({'capture': {'crop_size': 0}})
    assert prof['capture'] == {'width': 0, 'height': 0}


def test_body_absent_crop_size_leaves_capture_block_empty(web_mod, monkeypatch):
    """未提交 crop_size ⇒ capture 子对象为空（深合并时不会顶掉 Core 现有裁切设置）。"""
    monkeypatch.setattr(web_mod, '_get_runtime_profile', lambda: _base_profile())
    prof = web_mod.web_body_to_profile({'ai': {'controller': {'kp_x': 0.5}}})
    assert prof['capture'] == {}


# ===========================================================================
# 3. PUT /api/config：真正写给 Core 的那一份 profile
# ===========================================================================

def test_put_config_saves_full_frame_without_error(web_mod, monkeypatch):
    """P0-1 的正题：全帧（0）保存必须成功。板端历史表现是恒 503。"""
    rec = {}
    client = _client(web_mod, monkeypatch, recorder=rec)
    rv = client.put('/api/config', json={'capture': {'crop_size': 0}})
    assert rv.status_code == 200
    body = rv.get_json()
    assert body['ok'] is True
    assert rec['req_type'] == 'SET_CONFIG'
    assert rec['params']['profile']['capture']['width'] == 0


def test_put_config_normalizes_panel_value_before_set_config(web_mod, monkeypatch):
    """面板把全帧带成 1 ⇒ 发给 Core 的必须是 64（否则整份配置被拒、连带丢字段）。"""
    rec = {}
    client = _client(web_mod, monkeypatch, recorder=rec)
    rv = client.put('/api/config', json={'capture': {'crop_size': 1}})
    assert rv.status_code == 200
    prof = rec['params']['profile']
    assert prof['capture']['width'] == 64
    assert prof['capture']['height'] == 64
    # 未提交的字段必须原样保留（深合并语义不能因归一化被破坏）
    assert prof['capture']['offset_x'] == 0
    assert prof['inference']['confidence'] == 0.5


def test_put_config_normalizes_stale_core_profile_even_when_field_untouched(web_mod, monkeypatch):
    """Core 现存配置里已残留 width=1 时，用户"只改别的字段"也必须能存下去。"""
    rec = {}
    client = _client(web_mod, monkeypatch, profile=_base_profile(width=1, height=1), recorder=rec)
    rv = client.put('/api/config', json={'video_detection_confidence': 0.7})
    assert rv.status_code == 200
    prof = rec['params']['profile']
    assert prof['capture']['width'] == 64
    assert prof['capture']['height'] == 64
    assert prof['inference']['confidence'] == 0.7


def test_put_config_keeps_existing_model_id_when_body_omits_it(web_mod, monkeypatch):
    rec = {}
    client = _client(web_mod, monkeypatch, recorder=rec)
    client.put('/api/config', json={'capture': {'crop_size': 0}})
    assert rec['params']['profile']['model_id'] == 'model-A'


# ===========================================================================
# 4. 错误分流：status 语义（3=Core 不在；1/2/4=Core 拒收并带原话）
# ===========================================================================

CORE_REJECTION = 'profile 校验失败: capture.width 非法（0=全帧，或需在 64~3840 之间）'


@pytest.mark.parametrize('status,msg', [
    (1, CORE_REJECTION),
    (2, 'model not found'),
    (4, 'unsupported request'),
])
def test_put_config_surfaces_core_rejection_with_real_reason(web_mod, monkeypatch, status, msg):
    client = _client(web_mod, monkeypatch, ipc_result={'status': status, 'error': msg})
    rv = client.put('/api/config', json={'capture': {'crop_size': 192}})
    assert rv.status_code == 400
    body = rv.get_json()
    assert body['ok'] is False
    assert body['core_error'] == msg
    assert msg in body['error']
    # 关键回归：不得再把"被拒"谎报成"Core 未运行"
    assert body.get('core_offline') is not True
    assert 'Core 未运行' not in body['error']


def test_put_config_reports_core_offline_only_on_status_3(web_mod, monkeypatch):
    # 传输层原因的原文照带（此处刻意不写 socket 路径字面量：仓库门禁①要求路径单点真源）
    client = _client(web_mod, monkeypatch,
                     ipc_result={'status': 3, 'error': 'connect failed: no such file'})
    rv = client.put('/api/config', json={'capture': {'crop_size': 192}})
    assert rv.status_code == 503
    body = rv.get_json()
    assert body['ok'] is False
    assert body['core_offline'] is True
    assert 'Core 未运行' in body['error']
    # 传输层原因也要带出来，别只剩一句笼统文案
    assert 'no such file' in body['error']


def test_put_config_rejection_message_is_not_generic(web_mod, monkeypatch):
    """空 error 的拒收也要给兜底文案，不能把消息吞成空串。"""
    client = _client(web_mod, monkeypatch, ipc_result={'status': 1, 'error': ''})
    rv = client.put('/api/config', json={'capture': {'crop_size': 192}})
    assert rv.status_code == 400
    body = rv.get_json()
    assert body['core_error'] == '未知原因'
    assert '未知原因' in body['error']


# ===========================================================================
# 5. POST /api/presets/load：同一处修复的第二落点
# ===========================================================================

def test_presets_load_normalizes_legacy_profile_before_set_config(web_mod, monkeypatch, tmp_path):
    """旧版预设（RuntimeProfile 结构）里带 width=1 ⇒ 加载时同样要被带回合法域。"""
    pd = tmp_path / 'presets'
    pd.mkdir()
    (pd / 'legacy.json').write_text(json.dumps({
        'capture': {'width': 1, 'height': 1},
        'inference': {'confidence': 0.6},
    }, ensure_ascii=False), encoding='utf-8')
    monkeypatch.setattr(web_mod, 'PRESETS_DIR', str(pd))
    rec = {}
    client = _client(web_mod, monkeypatch, recorder=rec)
    rv = client.post('/api/presets/load', json={'name': 'legacy'})
    assert rv.status_code == 200
    assert rv.get_json()['ok'] is True
    assert rec['req_type'] == 'SET_CONFIG'
    prof = rec['params']['profile']
    assert prof['capture']['width'] == 64
    assert prof['capture']['height'] == 64
    assert prof['inference']['confidence'] == 0.6


def test_presets_load_surfaces_core_rejection(web_mod, monkeypatch, tmp_path):
    pd = tmp_path / 'presets'
    pd.mkdir()
    (pd / 'legacy.json').write_text(json.dumps({
        'capture': {'width': 1, 'height': 1},
    }, ensure_ascii=False), encoding='utf-8')
    monkeypatch.setattr(web_mod, 'PRESETS_DIR', str(pd))
    client = _client(web_mod, monkeypatch, ipc_result={'status': 1, 'error': CORE_REJECTION})
    rv = client.post('/api/presets/load', json={'name': 'legacy'})
    assert rv.status_code == 400
    body = rv.get_json()
    assert body['core_error'] == CORE_REJECTION
    assert '预设被 Core 拒绝' in body['error']
    assert body.get('core_offline') is not True
