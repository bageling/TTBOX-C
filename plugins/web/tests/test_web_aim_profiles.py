# test_web_aim_profiles.py — 回归锁：面板「热键与类别」多档位 ↔ core mouse.aim_profiles
#
# 背景（2026-09-24）：面板每张热键卡 = 一个档位，提交体是 aim_profiles[] 数组；
# core 侧 MouseProfile.aim_profiles 是热键的唯一真源（老平铺 aim_hotkey/aim_hotkey2/
# aim_hotkey_mode 已从结构体删除）。修复前后端只读 aim_profiles[0] ⇒ 第 2 张卡起
# 保存时静默丢弃、刷新后连卡都消失。本文件钉死下面这些点，防回退。
#
# 业主裁定的档位规则：
#   · 禁止重叠，保存报错 —— 任意两档的键位并集（主 ∪ 副）必须互斥（按位与 == 0）。
#   · ★ 但它**不保证**选档唯一：同时按下两档各自的键（左键+右键）会两档都命中，
#     物理上禁不掉。core 侧兜底 = 取面板顺序里第一个命中的档（aim_profile_match）。
#
# 运行：python -m pytest plugins/web/tests/test_web_aim_profiles.py -v
import importlib.util
import json
import os
import pathlib
import re
import sys
import tempfile

import pytest

from plugins.web.lib.paths import repo_root as _ttbox_repo_root  # noqa: E402  路径单点真源（A-PATH-3）
REPO_ROOT = pathlib.Path(_ttbox_repo_root())
WEB_SRC = REPO_ROOT / 'plugins' / 'web' / 'bin' / 'ttbox-web.py'
TEMPLATE = REPO_ROOT / 'plugins' / 'web' / 'templates' / 'index.html'
from plugins.web.tests import panel_src  # noqa: E402  2026-10-03 面板外链：读整个面板

_load_seq = 0


def _load():
    global _load_seq
    _load_seq += 1
    name = 'ttbox_web_aimprof_%d_%d' % (os.getpid(), _load_seq)
    spec = importlib.util.spec_from_file_location(name, WEB_SRC)
    mod = importlib.util.module_from_spec(spec)
    sys.modules[name] = mod
    spec.loader.exec_module(mod)
    return mod


@pytest.fixture()
def web_mod(monkeypatch):
    """加载 ttbox-web.py，并把全部运行期路径指到临时目录（不碰开发机真实 FHS）。"""
    tmp = tempfile.mkdtemp(prefix='ttbox_aimprof_%d_' % os.getpid())
    monkeypatch.setenv('TTBOX_PREFIX', tmp)
    monkeypatch.setenv('TTBOX_PRESETS_DIR', os.path.join(tmp, 'presets'))
    monkeypatch.setenv('TTBOX_CONFIG_DIR', os.path.join(tmp, 'config'))
    monkeypatch.setenv('TTBOX_MODELS_ROOT', os.path.join(tmp, 'models'))
    monkeypatch.setenv('TTBOX_MOTION_PROFILES_DIR', os.path.join(tmp, 'config', 'motion-profiles'))
    return _load()


def _base_profile():
    return {
        'capture': {'width': 640, 'height': 640, 'offset_x': 0, 'offset_y': 0},
        'inference': {'confidence': 0.5, 'iou': 0.45, 'class_filter': []},
        # 老配置形态：没有 aim_profiles（core 会合成单档）
        'mouse': {'kp_x': 0.5, 'kp_y': 0.5, 'sensitivity': 1.25},
        'fov': {'shape': 0, 'center_x': 0.5, 'center_y': 0.5, 'enabled': True, 'radius': 0.4},
        'model_id': 'model-A',
    }


def _client(web_mod, monkeypatch, profile=None, recorder=None):
    def fake_ipc(req_type, params=None, timeout=5):
        if recorder is not None:
            recorder['req_type'] = req_type
            recorder['params'] = params
        return {'status': 0, 'data': {}}

    monkeypatch.setattr(web_mod, 'ipc_request', fake_ipc)
    monkeypatch.setattr(web_mod, '_get_runtime_profile',
                        lambda: json.loads(json.dumps(profile if profile is not None else _base_profile())))
    monkeypatch.setattr(web_mod, '_activation_ok', lambda: True)
    return web_mod.app.test_client()


def _two_cards():
    """两档、键位互斥（右键 / 左键）。"""
    return [
        {'hotkey': 'right', 'hotkey2': '', 'hotkey_mode': 'any', 'sensitivity': 1.2,
         'fov_scale': 0.5, 'offset_x': 0.4, 'offset_y': 0.3, 'class_filter_mask': (1 << 0) | (1 << 3)},
        {'hotkey': 'left', 'hotkey2': '', 'hotkey_mode': 'any', 'sensitivity': 0.8,
         'fov_scale': 0.8, 'offset_x': 0.6, 'offset_y': 0.7, 'class_filter_mask': (1 << 1)},
    ]


# ===========================================================================
# 1. 整表遍历：第 2 张卡起不能丢
# ===========================================================================

def test_second_card_is_saved_not_dropped(web_mod, monkeypatch):
    """★ 旧实现只取 aim_profiles[0]，第 2 张卡的全部字段在保存时静默丢弃。"""
    monkeypatch.setattr(web_mod, '_get_runtime_profile',
                        lambda: json.loads(json.dumps(_base_profile())))
    out = web_mod.web_body_to_profile({'aim_profiles': _two_cards()})
    profs = out['mouse']['aim_profiles']
    assert len(profs) == 2, '第 2 张卡必须一起存下去'
    assert profs[0]['hotkey'] == 2            # right
    assert profs[1]['hotkey'] == 1            # left
    assert profs[1]['fov_scale'] == pytest.approx(0.8)
    assert profs[1]['offset_y'] == pytest.approx(0.7)
    assert profs[1]['sensitivity'] == pytest.approx(0.8)
    assert profs[1]['class_filter'] == [1]


def test_every_card_field_maps_to_core_contract(web_mod, monkeypatch):
    """逐字段核对面板 → core 的映射（键位字符串转位掩码、类别掩码转列表）。"""
    monkeypatch.setattr(web_mod, '_get_runtime_profile',
                        lambda: json.loads(json.dumps(_base_profile())))
    out = web_mod.web_body_to_profile({'aim_profiles': [
        {'hotkey': 'forward', 'hotkey2': 'back', 'hotkey_mode': 'all', 'sensitivity': 1.0,
         'fov_scale': 1.0, 'offset_x': 0.25, 'offset_y': 0.75, 'class_filter_mask': (1 << 2)},
    ]})
    p = out['mouse']['aim_profiles'][0]
    assert p['hotkey'] == 16
    assert p['hotkey2'] == 8
    assert p['hotkey_mode'] == 'all'
    assert p['offset_x'] == pytest.approx(0.25)
    assert p['offset_y'] == pytest.approx(0.75)
    assert p['class_filter'] == [2]


def test_profile_order_is_preserved(web_mod, monkeypatch):
    """★ 顺序有语义：多键同按时 core 取数组里第一个命中的档，保存不能重排。"""
    monkeypatch.setattr(web_mod, '_get_runtime_profile',
                        lambda: json.loads(json.dumps(_base_profile())))
    out = web_mod.web_body_to_profile({'aim_profiles': _two_cards()})
    profs = out['mouse']['aim_profiles']
    assert [p['hotkey'] for p in profs] == [2, 1]


# ===========================================================================
# 2. 档位重叠：保存报错（业主裁定）
# ===========================================================================

def test_overlapping_main_keys_are_rejected(web_mod):
    """两档用同一个键 ⇒ 报错，且信息里点出是哪两张卡重复、重复的是哪个键。"""
    with pytest.raises(web_mod.ConfigValidationError) as ei:
        web_mod.validate_aim_profiles([
            {'hotkey': 'right', 'hotkey_mode': 'any'},
            {'hotkey': 'right', 'hotkey_mode': 'any'},
        ])
    msg = str(ei.value)
    assert '热键 1 与热键 2' in msg
    assert 'right' in msg


def test_side_key_colliding_with_someone_elses_main_key_is_rejected(web_mod):
    """副键撞别人的主键也算重叠 —— 单键按下会同时落进两档。"""
    with pytest.raises(web_mod.ConfigValidationError):
        web_mod.validate_aim_profiles([
            {'hotkey': 'right', 'hotkey2': '', 'hotkey_mode': 'any'},
            {'hotkey': 'middle', 'hotkey2': 'right', 'hotkey_mode': 'any'},
        ])


def test_disjoint_cards_pass_validation(web_mod):
    got = web_mod.validate_aim_profiles(_two_cards())
    assert len(got) == 2


def test_missing_main_key_is_rejected_not_defaulted(web_mod):
    """主键缺失必须报错：core 侧是整表替换，默认成右键 = 静默改配置。"""
    with pytest.raises(web_mod.ConfigValidationError) as ei:
        web_mod.validate_aim_profiles([{'hotkey': '', 'hotkey_mode': 'any'}])
    assert '主按键' in str(ei.value)


def test_side_key_equal_to_main_key_is_rejected(web_mod):
    with pytest.raises(web_mod.ConfigValidationError) as ei:
        web_mod.validate_aim_profiles([{'hotkey': 'right', 'hotkey2': 'right', 'hotkey_mode': 'any'}])
    assert '相同' in str(ei.value)


def test_all_mode_without_side_key_is_rejected(web_mod):
    """「同时按下」却没选副键 ⇒ 这档永远不会命中，属于配错，直接挡在保存前。"""
    with pytest.raises(web_mod.ConfigValidationError) as ei:
        web_mod.validate_aim_profiles([{'hotkey': 'right', 'hotkey2': '', 'hotkey_mode': 'all'}])
    assert '副按键' in str(ei.value)


def test_too_many_cards_are_rejected(web_mod):
    cards = [{'hotkey': k, 'hotkey_mode': 'any'} for k in ('left', 'right', 'middle', 'back', 'forward')]
    # 5 个内置键最多 5 档；上限常量若被改小，这里立刻红
    for extra in range(web_mod.AIM_PROFILE_MAX):
        cards.append({'hotkey': 'left', 'hotkey_mode': 'any'})
    with pytest.raises(web_mod.ConfigValidationError) as ei:
        web_mod.validate_aim_profiles(cards)
    assert '最多' in str(ei.value)


# ===========================================================================
# 3. 空 / 缺失 aim_profiles 不得误伤别的字段
# ===========================================================================

def test_absent_aim_profiles_leaves_mouse_profiles_untouched(web_mod, monkeypatch):
    """提交体不带 aim_profiles（只改别的字段）⇒ 不能写 mouse.aim_profiles。

    core 侧 aim_profiles 是**整表替换**，写一个空表进去等于把用户的档位全删了。
    """
    monkeypatch.setattr(web_mod, '_get_runtime_profile',
                        lambda: json.loads(json.dumps(_base_profile())))
    out = web_mod.web_body_to_profile({'sens': 1.5})
    assert 'aim_profiles' not in out['mouse']
    assert out['mouse']['sensitivity'] == pytest.approx(1.5)


def test_absent_aim_profiles_leaves_inference_class_filter_untouched(web_mod, monkeypatch):
    """类别的并集只在带了档位表时才算 —— 否则改个别的字段会把目标类别清空。"""
    monkeypatch.setattr(web_mod, '_get_runtime_profile',
                        lambda: json.loads(json.dumps(_base_profile())))
    out = web_mod.web_body_to_profile({'sens': 1.5})
    assert 'class_filter' not in out['inference']


def test_empty_aim_profiles_list_writes_nothing(web_mod, monkeypatch):
    monkeypatch.setattr(web_mod, '_get_runtime_profile',
                        lambda: json.loads(json.dumps(_base_profile())))
    out = web_mod.web_body_to_profile({'aim_profiles': []})
    assert 'aim_profiles' not in out['mouse']


def test_empty_aim_profiles_does_not_silently_clear_inference_class_filter(web_mod, monkeypatch):
    """★ 同一个空数组哨兵，两侧必须同解。

    2026-09-25 修：mouse 段把 `[]` 当"未提交"（保留旧档表），inference 段却挂的是
    `profiles is not None` ⇒ 当成"已提交"，把 class_filter 写成 []，而 core 侧
    `TargetSelector.cpp` 空 class_filter = **不过滤 = 全类别放行**。
    净效果：档表纹丝不动，用户已排除的类别却全都回来了。
    """
    monkeypatch.setattr(web_mod, '_get_runtime_profile',
                        lambda: json.loads(json.dumps(_base_profile())))
    out = web_mod.web_body_to_profile({'aim_profiles': []})
    assert 'aim_profiles' not in out['mouse'], 'mouse 段：空表不应覆盖旧档'
    # 推理侧同样必须"没提交"，不能偷偷写 class_filter
    assert 'class_filter' not in (out.get('inference') or {}), \
        'inference 段：空表不能把类别过滤清成 []（= core 侧全类别放行）'
    # 合并到既有配置后，旧的类别过滤必须原样保留
    merged = web_mod._deep_merge_profile(
        {'mouse': {'aim_profiles': [{'hotkey': 2, 'class_filter': [0, 1]}]},
         'inference': {'class_filter': [0, 1]}}, out)
    assert merged['inference']['class_filter'] == [0, 1]


# ===========================================================================
# 4. 目标类别：推理侧收全档并集（否则档 2 要用的类别在推理阶段就被丢）
# ===========================================================================

def test_inference_class_filter_is_the_union_of_all_cards(web_mod, monkeypatch):
    monkeypatch.setattr(web_mod, '_get_runtime_profile',
                        lambda: json.loads(json.dumps(_base_profile())))
    out = web_mod.web_body_to_profile({'aim_profiles': _two_cards()})
    # 卡1 类别 {0,3}，卡2 类别 {1} → 推理侧必须是 {0,1,3}
    assert out['inference']['class_filter'] == [0, 1, 3]


def test_single_card_union_equals_that_card(web_mod, monkeypatch):
    """单档时并集 == 本档类别 ⇒ 与"瞄准侧按档窄化"合起来行为不变。"""
    monkeypatch.setattr(web_mod, '_get_runtime_profile',
                        lambda: json.loads(json.dumps(_base_profile())))
    out = web_mod.web_body_to_profile({'aim_profiles': [
        {'hotkey': 'right', 'hotkey_mode': 'any', 'class_filter_mask': (1 << 5)},
    ]})
    assert out['inference']['class_filter'] == [5]
    assert out['mouse']['aim_profiles'][0]['class_filter'] == [5]


# ===========================================================================
# 5. 移动倍率：卡片值不得再写进全局（否则 core 里会乘两遍）
# ===========================================================================

def test_card_sensitivity_is_not_written_into_global_sensitivity(web_mod, monkeypatch):
    """★ core 侧 out = 全局 sensitivity × 当前档 sensitivity。

    旧实现把 card[0].sensitivity 覆盖到 mouse.sensitivity 上 ⇒ 既让总览「移动倍率」
    滑块变成死的，又会在加了按档倍率之后把倍率乘两遍。
    """
    monkeypatch.setattr(web_mod, '_get_runtime_profile',
                        lambda: json.loads(json.dumps(_base_profile())))
    out = web_mod.web_body_to_profile({'aim_profiles': _two_cards()})
    # 卡片倍率 1.2 / 0.8 都不能跑到全局去：body 没带 sens 时全局键**根本不出现**
    # （_deep_merge_profile 按键合并 ⇒ 不出现 = 不动库里现值 1.25）。
    # 旧实现把 card[0].sensitivity 写进全局，总览滑块就成了死的。
    assert 'sensitivity' not in out['mouse']
    assert out['mouse']['aim_profiles'][0]['sensitivity'] == pytest.approx(1.2)
    assert out['mouse']['aim_profiles'][1]['sensitivity'] == pytest.approx(0.8)


def test_body_sens_still_controls_the_global_sensitivity(web_mod, monkeypatch):
    monkeypatch.setattr(web_mod, '_get_runtime_profile',
                        lambda: json.loads(json.dumps(_base_profile())))
    out = web_mod.web_body_to_profile({'sens': 2.0, 'aim_profiles': _two_cards()})
    assert out['mouse']['sensitivity'] == pytest.approx(2.0)
    assert out['mouse']['aim_profiles'][1]['sensitivity'] == pytest.approx(0.8)


# ===========================================================================
# 6. 档 0 的瞄准点仍然写出平铺全局偏移（core 合成的老配置靠它）
# ===========================================================================

def test_card0_offset_is_mirrored_to_flat_global_offset(web_mod, monkeypatch):
    monkeypatch.setattr(web_mod, '_get_runtime_profile',
                        lambda: json.loads(json.dumps(_base_profile())))
    out = web_mod.web_body_to_profile({'aim_profiles': _two_cards()})
    assert out['mouse']['offset_x'] == pytest.approx(0.4)
    assert out['mouse']['offset_y'] == pytest.approx(0.3)


# ===========================================================================
# 7. HTTP 层：非法档位表必须 400 且不落盘
# ===========================================================================

def test_put_config_rejects_overlapping_profiles_with_400(web_mod, monkeypatch):
    rec = {}
    client = _client(web_mod, monkeypatch, recorder=rec)
    rv = client.put('/api/config', json={'aim_profiles': [
        {'hotkey': 'right', 'hotkey_mode': 'any'},
        {'hotkey': 'right', 'hotkey_mode': 'any'},
    ]})
    assert rv.status_code == 400
    body = rv.get_json()
    assert body['ok'] is False
    assert '热键 1 与热键 2' in body['error']
    assert 'req_type' not in rec, '校验失败时绝不能把配置发给 core'


def test_put_config_accepts_two_disjoint_profiles(web_mod, monkeypatch):
    rec = {}
    client = _client(web_mod, monkeypatch, recorder=rec)
    rv = client.put('/api/config', json={'aim_profiles': _two_cards()})
    assert rv.status_code == 200
    sent = rec['params']['profile']['mouse']['aim_profiles']
    assert len(sent) == 2
    assert sent[1]['hotkey'] == 1


# ===========================================================================
# 8. 跨层锁：面板前端必须有同一套重叠校验（后端是硬护栏，前端给即时提示）
# ===========================================================================

def _html() -> str:
    return panel_src.all_src()


def test_html_defines_overlap_check_before_submit():
    src = _html()
    assert 'function aimProfilesConflict' in src, '面板缺少重叠校验函数'
    # 定义了不调用等于没有。
    assert src.count('aimProfilesConflict(') >= 2, '重叠校验函数没有被调用'
    # 且必须在 applyConfigNow 里、collectConfig() **之前** —— 否则冲突会先提交出去，
    # 用户拿到的是延迟一轮的报错（后端 400 那一趟）。
    seg = src.split('async function applyConfigNow', 1)[1]
    call_at = seg.find('aimProfilesConflict()')
    collect_at = seg.find('collectConfig()')
    assert call_at != -1, 'applyConfigNow 里没有调用重叠校验'
    assert collect_at != -1, 'applyConfigNow 里找不到 collectConfig()'
    assert call_at < collect_at, '重叠校验必须在 collectConfig() 之前'


def test_html_validation_reads_every_card_not_just_the_first():
    """校验必须遍历全部卡片 —— 只查第一张的话，第 2/3 张撞键根本发现不了。"""
    src = _html()
    seg = src.split('function aimProfilesConflict', 1)[1].split('\n}', 1)[0]
    assert 'querySelectorAll' in seg
    assert 'aim-profile-card' in seg


def test_html_key_bits_table_matches_backend():
    """前端的键位表必须与后端 HOTKEY_BITS 逐项同值（否则校验口径漂移）。"""
    src = _html()
    m = re.search(r'const\s+AIM_HOTKEY_BITS\s*=\s*\{([^}]*)\}', src)
    assert m, 'index.html 缺少 AIM_HOTKEY_BITS 常量'
    pairs = dict(re.findall(r"(\w+)\s*:\s*(\d+)", m.group(1)))
    assert pairs == {'left': '1', 'right': '2', 'middle': '4', 'back': '8', 'forward': '16'}


# ===========================================================================
# 热键掩码必须做「域校验」，不能只判 == 0
#
# 2026-09-25 修：validate_aim_profiles 原来只判 `hk == 0`，于是 hotkey=-1/32/255
# 全都放行。core 侧 static_cast<uint8_t>(-1) = 255，而命中判据是
# `ap.hotkey != 0 && (buttons & ap.hotkey) != 0` ⇒ 255 对**任意**物理键成立
# ⇒ 按左键/右键/中键/侧键都会瞄准，热键闸门形同虚设（fail-open，比拒绝更危险）。
# 下面两条把「非法值必须被拒」和「core 侧不再绕回」都钉住。
# ===========================================================================


def test_hotkey_out_of_domain_values_are_rejected():
    """★ 反向锁：-1 / 32 / 255 这类越界掩码必须被拒，
    否则 core 侧会绕回成 255，命中判据对任意键成立。"""
    web = _load()
    for bad in (-1, 32, 255, 64, 0x20):
        with pytest.raises(web.ConfigValidationError):
            web.validate_aim_profiles([{'hotkey': bad, 'hotkey_mode': 'any'}])
        with pytest.raises(web.ConfigValidationError):
            web.validate_aim_profiles([{'hotkey': 'right', 'hotkey2': bad,
                                        'hotkey_mode': 'all'}])


def test_combined_hotkey_mask_is_rejected_because_panel_cannot_roundtrip_it():
    """组合掩码（3 = 左|右）core 能用，但面板 `_bits_to_hotkey(3)` 返回空串、
    会被 `or 'right'` 静默显示成右键 ⇒ 用户下一次保存就真的变成 2。
    "能存但不能显示"就是静默漂移，面板这一层直接拒掉，不给它进网。"""
    web = _load()
    with pytest.raises(web.ConfigValidationError):
        web.validate_aim_profiles([{'hotkey': 3, 'hotkey_mode': 'any'}])
    # 五个合法单键仍然全部可用
    for good in ('left', 'right', 'middle', 'back', 'forward'):
        out = web.validate_aim_profiles([{'hotkey': good, 'hotkey_mode': 'any'}])
        assert out[0]['hotkey'] == web.HOTKEY_BITS[good]


def test_core_dict_clamps_out_of_domain_hotkey():
    """写入 core 的那一层也要夹住（validate 之外的第二条防线）。"""
    web = _load()
    d = web._aim_profile_core_dict({'hotkey': -1, 'hotkey2': 999})
    assert d['hotkey'] in web.AIM_PROFILE_VALID_BITS
    assert d['hotkey2'] in web.AIM_PROFILE_VALID_BITS or d['hotkey2'] == 0


def test_core_runtime_profile_sanitizes_hotkey_bits():
    """core 侧 RuntimeProfile 必须把负数/越界热键夹成 0（永不命中），
    而不是靠 static_cast<uint8_t> 绕回成 255（fail-open）。"""
    src = pathlib.Path(REPO_ROOT / 'core' / 'src' / 'model' / 'RuntimeProfile.cpp').read_text(encoding='utf-8')
    assert 'sanitize_hotkey_bits' in src, 'core 缺少热键掩码消毒函数'
    # 三个解析点都必须走消毒，不能还留裸强转
    assert 'static_cast<uint8_t>(obj_int(j, "hotkey"' not in src
    assert 'static_cast<uint8_t>(obj_int(*m, "aim_hotkey"' not in src
    assert 'toggle_hotkey", 4) & 0x1F' not in src, 'hotkey_guard 仍在用 & 0x1F（-1 会掩成 31 = 任意键）'
