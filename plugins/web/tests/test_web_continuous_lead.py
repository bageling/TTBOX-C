# test_web_continuous_lead.py — 持续提前量（continuous_lead）Web ↔ Core 通路单测
#
# 背景（M2：面板对齐 yu）：
#   全仓复核发现 continuous_lead 是**算法在、管线断**——
#     · 算法 + 单测在：core/src/mouse/ContinuousLead.hpp、core/tests/test_mouse.cpp
#     · 结构体在：MouseTypes.hpp ContinuousLeadConfig
#     · 但 RuntimeProfile.mouse 无该成员、AimThread 从未调用、Web 层只有
#       static/legacy/index-preview.html 里一句"持续提前量参数配置占位"
#       （该遗留模板 2026-09-24 面板收敛时已删除，归档见 .workbuddy/artifacts/archive/）
#       ⇒ 该功能**永远无法从面板开启**。
#   本次把四层补齐，本文件锁死其中 Web 那两层（参数翻译 + 反向投影）。
#
# 字段命名依据 yu config.json 的 ai.controller.continuous_lead_*：
#   continuous_lead_enabled / _enter_distance / _scale / _fade_in_ms / _fade_out_ms
#   / _near_disable_ratio
#
# ★ 有效性契约（必须与 core/src/mouse/ContinuousLead.hpp 的 apply() 保持一致）：
#   只有 enabled / enter_distance / scale 参与算法；fade_in_ms / fade_out_ms /
#   near_disable_ratio 是 Core 侧标"保留字段"、算法尚未消费的预留项。
#   本文件**不**断言这 3 个"生效"，只断言它们能无损往返（面板标为"预留·调节无效"）。
#
# 运行：python -m pytest plugins/web/tests/test_web_continuous_lead.py -v
import importlib.util
import os
import pathlib
import sys

WEB_SRC = pathlib.Path(__file__).resolve().parents[3] / 'plugins' / 'web' / 'bin' / 'ttbox-web.py'

_load_seq = 0

# 有效字段（core 侧 apply() 真正消费）
EFFECTIVE = ('enabled', 'enter_distance', 'scale')
# 预留字段（core 侧结构体已声明、算法未消费）
RESERVED = ('fade_in_ms', 'fade_out_ms', 'near_disable_ratio')


def _load():
    global _load_seq
    _load_seq += 1
    name = 'ttbox_web_lead_%d_%d' % (os.getpid(), _load_seq)
    spec = importlib.util.spec_from_file_location(name, WEB_SRC)
    mod = importlib.util.module_from_spec(spec)
    sys.modules[name] = mod
    spec.loader.exec_module(mod)
    return mod


def _body(**ctrl):
    return {'ai': {'controller': dict(ctrl)}}


# ---------------------------------------------------------------------------
# 1. Web body → RuntimeProfile
# ---------------------------------------------------------------------------

def test_body_maps_all_six_fields_into_mouse_continuous_lead():
    mod = _load()
    prof = mod.web_body_to_profile(_body(
        continuous_lead_enabled=True,
        continuous_lead_enter_distance=180,
        continuous_lead_scale=0.7,
        continuous_lead_fade_in_ms=250,
        continuous_lead_fade_out_ms=400,
        continuous_lead_near_disable_ratio=0.5,
    ))
    cl = prof['mouse']['continuous_lead']
    assert cl['enabled'] is True
    assert cl['enter_distance'] == 180
    assert cl['scale'] == 0.7
    assert cl['fade_in_ms'] == 250
    assert cl['fade_out_ms'] == 400
    assert cl['near_disable_ratio'] == 0.5


def test_body_absent_fields_do_not_create_the_key():
    """面板未提交该组字段时**不得**凭空造 key —— 核心侧据此保留自己的默认值（关）。"""
    mod = _load()
    prof = mod.web_body_to_profile(_body(kp_x=0.5))
    assert 'continuous_lead' not in (prof.get('mouse') or {})


def test_body_partial_fields_only_carry_what_was_sent():
    mod = _load()
    prof = mod.web_body_to_profile(_body(continuous_lead_scale=1.2))
    cl = prof['mouse']['continuous_lead']
    assert cl == {'scale': 1.2}
    assert 'enabled' not in cl        # 未提交 ⇒ 不写，由 core 默认（false）兜底


def test_body_enabled_is_coerced_to_real_boolean():
    """表单勾选框可能回传 0/1/字符串，必须归一为 bool（否则 core 侧 obj_bool 语义分叉）。"""
    mod = _load()
    for raw, expected in ((0, False), (1, True), (False, False), (True, True)):
        prof = mod.web_body_to_profile(_body(continuous_lead_enabled=raw))
        assert prof['mouse']['continuous_lead']['enabled'] is expected, raw


# ---------------------------------------------------------------------------
# 2. RuntimeProfile → Web（反向投影）
# ---------------------------------------------------------------------------

def test_profile_projects_into_ai_controller():
    mod = _load()
    web = mod.profile_to_web({'mouse': {'continuous_lead': {
        'enabled': True, 'enter_distance': 200, 'scale': 0.9,
        'fade_in_ms': 120, 'fade_out_ms': 220, 'near_disable_ratio': 0.4,
    }}})
    ctrl = web['ai']['controller']
    assert ctrl['continuous_lead_enabled'] is True
    assert ctrl['continuous_lead_enter_distance'] == 200
    assert ctrl['continuous_lead_scale'] == 0.9
    assert ctrl['continuous_lead_fade_in_ms'] == 120
    assert ctrl['continuous_lead_fade_out_ms'] == 220
    assert ctrl['continuous_lead_near_disable_ratio'] == 0.4


def test_profile_absent_yields_safe_defaults():
    """缺该块 ⇒ 面板必须显示"关"，且默认值与 Core 侧结构体默认值一致。

    这是安全默认：Core 侧没开就绝不动输出链；面板若显示"开"会误导用户以为在生效。
    """
    mod = _load()
    ctrl = mod.profile_to_web({'mouse': {}})['ai']['controller']
    assert ctrl['continuous_lead_enabled'] is False          # 必须关
    assert ctrl['continuous_lead_enter_distance'] == 150     # 与 MouseTypes.hpp 默认一致
    assert ctrl['continuous_lead_scale'] == 0.5
    assert ctrl['continuous_lead_fade_in_ms'] == 300
    assert ctrl['continuous_lead_fade_out_ms'] == 300
    assert ctrl['continuous_lead_near_disable_ratio'] == 0.66


# ---------------------------------------------------------------------------
# 3. 往返一致性（面板保存→读取不得丢值）
# ---------------------------------------------------------------------------

def test_roundtrip_is_lossless_across_all_six_fields():
    mod = _load()
    body = _body(continuous_lead_enabled=True,
                 continuous_lead_enter_distance=321,
                 continuous_lead_scale=1.25,
                 continuous_lead_fade_in_ms=111,
                 continuous_lead_fade_out_ms=222,
                 continuous_lead_near_disable_ratio=0.33)
    prof = mod.web_body_to_profile(body)
    ctrl = mod.profile_to_web(prof)['ai']['controller']
    assert ctrl['continuous_lead_enabled'] is True
    assert ctrl['continuous_lead_enter_distance'] == 321
    assert ctrl['continuous_lead_scale'] == 1.25
    assert ctrl['continuous_lead_fade_in_ms'] == 111
    assert ctrl['continuous_lead_fade_out_ms'] == 222
    assert ctrl['continuous_lead_near_disable_ratio'] == 0.33


# ---------------------------------------------------------------------------
# 4. 契约锁：字段名本身（面板/模板/yu 对齐都靠它）
# ---------------------------------------------------------------------------

def test_contract_key_names_are_exactly_yu_parity():
    """键名一旦改动，模板 id / app.js 采集 / yu 配置迁移会三方同时失配。"""
    mod = _load()
    ctrl = mod.profile_to_web({'mouse': {'continuous_lead': {}}})['ai']['controller']
    for field in EFFECTIVE + RESERVED:
        assert 'continuous_lead_' + field in ctrl, field

    prof = mod.web_body_to_profile(_body(**{
        'continuous_lead_' + f: 1 for f in EFFECTIVE + RESERVED}))
    cl = prof['mouse']['continuous_lead']
    for field in EFFECTIVE + RESERVED:
        assert field in cl, field
    assert 'continuous_lead_' not in cl        # 内层不得残留 Web 前缀


def test_no_extra_unexpected_keys_leak_into_profile_block():
    """只映射契约内的 6 个字段，其它 controller_continuous_lead_* 不得混入。"""
    mod = _load()
    prof = mod.web_body_to_profile(_body(
        continuous_lead_legacy_knob=42,      # 不在契约内的键
        continuous_lead_scale=0.8,
    ))
    cl = prof['mouse']['continuous_lead']
    assert 'legacy_knob' not in cl and 'continuous_lead_legacy_knob' not in cl
    assert cl == {'scale': 0.8}
