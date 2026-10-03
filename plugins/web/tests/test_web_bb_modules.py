# test_web_bb_modules.py — 辅助功能模块的 Web ↔ Core 通路单测
#
# 背景：把选靶/压枪/扳机机制重写进 core/src/mouse/ 后，还必须让**面板能开、能调、能存**，
# 否则就是"算法在、管线断"。本文件锁死 Web 这一层：参数翻译 + 反向投影。
#
# 覆盖：
#   · 压枪速率引擎 recoil_*（2026-09-30 对照 yu 重做：拉力/速度/累计上限/松手渐出/门控）
#   · 自动扳机 trigger2（2.0）—— 含键位字符串 ↔ 位掩码
#   · 选靶四项 selector_*（Core 侧是 mouse 顶层扁平键，不是子对象）
#
# 契约（改面板或改后端前先读这三条）：
#   1. 面板元素 id 即提交键名，全都在 body['ai']['controller'] 这一层。
#      老字段用 "controller_" 前缀（history 遗留）；模块用 "<模块前缀>_<字段>"，
#      例如 recoil_strength / selector_lock_hold_ms。
#      别名一律不要：同一功能的老界面 retired 后，键名也随之下线。
#   1b. ★ 压枪的参数来自**两处**：开关/热键在 body['recoil']，算法参数在
#       body['ai']['controller'] 的 recoil_* 扁平键里。两边必须合并进同一个
#       mouse.recoil 子对象（谁覆盖谁都会静默丢配置）。
#   2. profile_to_web 缺字段必须补 **Core 结构体默认值**（表里的第三列）——
#      面板首次打开显示的就是它，对不上会让没存过配置的设备显示成另一套参数。
#   3. 数组字段形状不对时**整套跳过**（不能写坏配置）；键位字段 Core 侧是位掩码、
#      面板侧是 'left'/'right'/'middle'/'back'/'forward' 或 ''。
#
# 运行：python -m pytest plugins/web/tests/test_web_bb_modules.py -v
import importlib.util
import os
import pathlib
import sys

from plugins.web.lib.paths import repo_root as _ttbox_repo_root  # noqa: E402  路径单点真源（A-PATH-3）
WEB_SRC = pathlib.Path(_ttbox_repo_root()) / 'plugins' / 'web' / 'bin' / 'ttbox-web.py'

_load_seq = 0


def _load():
    global _load_seq
    _load_seq += 1
    name = 'ttbox_web_bb_%d_%d' % (os.getpid(), _load_seq)
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


def test_recoil_rate_engine_fields_map():
    """压枪速率引擎（2026-09-30 对照 yu 重做）：面板 recoil_* → mouse.recoil.*。"""
    mod = _load()
    prof = mod.web_body_to_profile(_body(
        recoil_only_when_target_visible=False, recoil_target_lost_release_ms=500,
        recoil_trigger_delay_enabled=True, recoil_trigger_delay_ms=200,
        recoil_strength=180, recoil_speed=1.4, recoil_curve_strength=0.25, recoil_roi_h=450,
    ))
    rc = prof['mouse']['recoil']
    assert rc['only_when_target_visible'] is False
    assert abs(rc['target_lost_release_ms'] - 500.0) < 1e-9
    assert rc['trigger_delay_enabled'] is True
    assert abs(rc['trigger_delay_ms'] - 200.0) < 1e-9
    assert abs(rc['strength'] - 180.0) < 1e-9
    assert abs(rc['speed'] - 1.4) < 1e-9
    assert abs(rc['curve_strength'] - 0.25) < 1e-9
    assert abs(rc['roi_h'] - 450.0) < 1e-9


# 2026-09-29：v7.26 扳机的键位映射用例已删除（`trigger` 段整体下线）。
#   位掩码映射本身仍由 test_trigger2_keys_use_bitmask 覆盖。


def test_trigger2_keys_use_bitmask():
    mod = _load()
    prof = mod.web_body_to_profile(_body(
        trigger2_enabled=True, trigger2_key1='forward', trigger2_fire_button='left',
        trigger2_with_simple_recoil=True, trigger2_precision_frames=8,
    ))
    tg2 = prof['mouse']['trigger2']
    assert tg2['key1'] == 16
    assert tg2['fire_button'] == 1
    assert tg2['with_simple_recoil'] is True
    assert tg2['precision_frames'] == 8
    # 随枪压枪只剩一路（2026-09-30 收敛：简易/进阶/准星三路 → 只留 simple）
    assert 'with_' 'adv_recoil' not in tg2 and 'with_' 'crosshair' not in tg2


def test_selector_fields_land_on_mouse_top_level():
    mod = _load()
    prof = mod.web_body_to_profile(_body(
        selector_lock_hold_ms=1500, selector_priority_scoring=True,
        selector_weight_size=0.45, selector_head_body_stable=True, selector_hb_body2=3,
    ))
    m = prof['mouse']
    assert m['lock_hold_ms'] == 1500.0
    assert m['priority_scoring'] is True
    assert abs(m['weight_size'] - 0.45) < 1e-9
    assert m['head_body_stable'] is True
    assert m['hb_body2'] == 3
    # 面板前缀不得带进 Core
    assert not any(k.startswith('selector_') for k in m)


def test_selector_switch_damping_maps_both_ways():
    """1.5.46 的切靶防抖两条：Core 与 AimThread 早就在用，面板一直没有界面。

    这次「选靶」分区补上，所以必须锁死双向通路 —— 只做单向的话会出现
    "面板能调但存不下去"，或者"存下去了但面板打开显示 0"。
    """
    mod = _load()
    prof = mod.web_body_to_profile(_body(
        selector_switch_hysteresis=0.35, selector_switch_cooldown_ms=450))
    assert abs(prof['mouse']['switch_hysteresis'] - 0.35) < 1e-9
    assert abs(prof['mouse']['switch_cooldown_ms'] - 450.0) < 1e-9

    c = mod.profile_to_web({'mouse': {'switch_hysteresis': 0.2,
                                      'switch_cooldown_ms': 300.0}})['ai']['controller']
    assert abs(c['selector_switch_hysteresis'] - 0.2) < 1e-9
    assert abs(c['selector_switch_cooldown_ms'] - 300.0) < 1e-9

    # 空 profile ⇒ 落回 Core 结构体默认（0.5 / 600），不能是 None，
    # 否则面板首次打开会显示成另一套参数。
    d = mod.profile_to_web({})['ai']['controller']
    assert d['selector_switch_hysteresis'] == 0.5
    assert d['selector_switch_cooldown_ms'] == 600.0


# ---------------------------------------------------------------------------
# 1b. 压枪：两个来源必须合并进同一个 mouse.recoil
# ---------------------------------------------------------------------------

def test_recoil_switch_and_rate_params_merge_into_one_object():
    """开关/热键（body.recoil）与算法参数（body.ai.controller 的 recoil_*）
    是**同一个** mouse.recoil 子对象，谁都不能把对方覆盖掉。

    这两处曾经各有各的引擎，所以后端是"整块覆盖"写法；现在合成一套，
    覆盖式写入会表现为"开关开了但参数没了"或"参数在但开关被抹掉"，
    两种都不报错。
    """
    mod = _load()
    prof = mod.web_body_to_profile({
        'ai': {'controller': {'recoil_strength': 180.0, 'recoil_speed': 1.2}},
        'recoil': {'enabled': True, 'hotkey': 'right', 'hotkey2': '', 'hotkey_mode': 'any'},
    })
    rc = prof['mouse']['recoil']
    assert rc['enabled'] is True
    assert rc['hotkey'] == 2
    assert abs(rc['strength'] - 180.0) < 1e-9
    assert abs(rc['speed'] - 1.2) < 1e-9


def test_recoil_params_without_switch_block_still_land():
    """只调参数、没动开关（body 里没有 recoil 块）时，参数照样要落下去。"""
    mod = _load()
    prof = mod.web_body_to_profile(_body(recoil_strength=250.0))
    assert abs(prof['mouse']['recoil']['strength'] - 250.0) < 1e-9
    # 面板没提交的字段不凭空写入（Core 保留原值）
    assert 'enabled' not in prof['mouse']['recoil']


def test_recoil_absent_everywhere_does_not_invent_object():
    """面板没提交压枪块时，不得凭空造出 mouse.recoil（Core 会保留原值）。"""
    mod = _load()
    prof = mod.web_body_to_profile(_body(trigger2_enabled=True))
    assert 'recoil' not in prof['mouse']


def test_profile_to_web_recoil_switch_reads_recoil_enabled():
    """回填只认 recoil.enabled —— 2026-09-30 起压枪只剩一套引擎。"""
    mod = _load()
    off = mod.profile_to_web({'mouse': {'recoil': {'enabled': False}}})['recoil']
    assert off['enabled'] is False
    on = mod.profile_to_web({'mouse': {'recoil': {'enabled': True}}})['recoil']
    assert on['enabled'] is True
    # 算法参数走 ai.controller 的 recoil_* 键，不该混进 body['recoil'] 块
    assert 'strength' not in on


# ---------------------------------------------------------------------------
# 2. RuntimeProfile → Web（回填）
# ---------------------------------------------------------------------------

def test_profile_to_web_fills_core_defaults():
    """空 profile ⇒ 面板键必须等于 Core 结构体默认值（首次打开显示的就是它）。"""
    mod = _load()
    c = mod.profile_to_web({})['ai']['controller']
    # 压枪速率引擎：默认值必须与 MouseTypes.hpp::RecoilConfig 一致
    assert c['recoil_only_when_target_visible'] is True
    assert c['recoil_target_lost_release_ms'] == 300.0
    assert c['recoil_trigger_delay_enabled'] is False and c['recoil_trigger_delay_ms'] == 120.0
    assert c['recoil_strength'] == 100.0 and c['recoil_speed'] == 1.0
    assert abs(c['recoil_curve_strength'] - 0.6) < 1e-9 and c['recoil_roi_h'] == 300.0
    # v7.26 扳机、一代提前量、三段查表 / 垂直修正 / 闭环的面板键已随实现下线
    assert 'trigger_enabled' not in c and 'lead1_enabled' not in c
    assert 'vc' '_enabled' not in c and 'recoil' '_cl_enabled' not in c
    # 以 recoil 开头的键只能是速率引擎这 8 个（其余引擎的键必须彻底消失）
    _rate_keys = {'recoil_only_when_target_visible', 'recoil_target_lost_release_ms',
                  'recoil_trigger_delay_enabled', 'recoil_trigger_delay_ms',
                  'recoil_strength', 'recoil_speed', 'recoil_curve_strength',
                  'recoil_roi_h'}
    assert {k for k in c if k.startswith('recoil_')} == _rate_keys
    assert c['trigger2_key1'] == 'forward' and c['trigger2_fire_button'] == 'left'
    assert c['selector_lock_hold_ms'] == 0.0
    assert c['selector_priority_scoring'] is False
    assert c['selector_hb_head1'] == 1 and c['selector_hb_body2'] == -1


def test_profile_to_web_reads_existing_values():
    mod = _load()
    prof = {'mouse': {
        'recoil': {'enabled': True, 'strength': 210.0, 'roi_h': 0.0},
        'trigger2': {'enabled': True, 'key1': 1},
        'lock_hold_ms': 800.0, 'priority_scoring': True,
    }}
    c = mod.profile_to_web(prof)['ai']['controller']
    assert c['recoil_strength'] == 210.0
    assert c['recoil_roi_h'] == 0.0                    # 0 = 不限，不能被当成"缺字段"
    # 表里没给的字段落回 Core 结构体默认值，而不是 None
    assert abs(c['recoil_curve_strength'] - 0.6) < 1e-9
    assert c['trigger2_enabled'] is True and c['trigger2_key1'] == 'left'
    assert c['selector_lock_hold_ms'] == 800.0
    assert c['selector_priority_scoring'] is True


def test_web_roundtrip_is_lossless():
    """面板键 → Core → 面板键，全部原样回来（含键位字符串）。"""
    mod = _load()
    ctrl_in = {
        'recoil_only_when_target_visible': False, 'recoil_strength': 140.0,
        'recoil_curve_strength': 0.35, 'recoil_roi_h': 500.0,
        'trigger2_enabled': True, 'trigger2_key1': 'forward', 'trigger2_first_err': 44.0,
        'selector_lock_hold_ms': 1200.0, 'selector_head_body_stable': True,
        'selector_hb_body1': 2, 'selector_hb_head1': 3,
    }
    prof = mod.web_body_to_profile(_body(**ctrl_in))
    out = mod.profile_to_web(prof)['ai']['controller']
    for key, want in ctrl_in.items():
        got = out[key]
        if isinstance(want, list):
            assert got == want, key
        elif isinstance(want, float):
            assert abs(got - want) < 1e-9, key
        else:
            assert got == want, key


def test_disabled_modules_do_not_leak_cross_keys():
    """只开一个模块时，不得在别的模块对象里塞键（避免面板互串）。"""
    mod = _load()
    prof = mod.web_body_to_profile(_body(trigger2_enabled=True))
    m = prof['mouse']
    assert 'trigger2' in m
    for other in ('recoil',):
        assert other not in m, other
