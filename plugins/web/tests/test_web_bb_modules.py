# test_web_bb_modules.py — BB 对标新模块的 Web ↔ Core 通路单测
#
# 背景（2026-09-24 批）：把 BB_927 的选靶/压枪/提前量/拟人化/扳机机制用 C++ 重写进
# core/src/mouse/ 后，还必须让**面板能开、能调、能存**，否则就是"算法在、管线断"
# （ContinuousLead 当年就是这么废掉的）。本文件锁死 Web 这一层：参数翻译 + 反向投影。
#
# 覆盖：
#   · 提前量两代 lead1 / lead2
#   · 拟人化链 humanize（含 speed_fluctuation / accuracy_sim 两个附加项）
#   · 抗过冲 anti_overshoot / 速度自适应 Kp speed_adaptive_kp / 全局正弦 global_wave
#   · 压枪三段查表 recoil_bb（标量 + preset_total_time_ms / preset_vert / preset_horiz）
#   · 垂直修正 vc
#   · 自动扳机 trigger（v7.26）/ trigger2（2.0）—— 含键位字符串 ↔ 位掩码
#   · 选靶四项 selector_*（Core 侧是 mouse 顶层扁平键，不是子对象）
#
# 契约（改面板或改后端前先读这三条）：
#   1. 面板元素 id 即提交键名，全都在 body['ai']['controller'] 这一层。
#      老字段用 "controller_" 前缀（history 遗留）；BB 新模块用 "<模块前缀>_<字段>"，
#      例如 lead1_frames / recoil_bb_preset / selector_lock_hold_ms。
#      别名一律不要：同一功能的老界面 retired 后，键名也随之下线。
#   1b. ★ 压枪面板只剩一个总开关（业主 2026-09-24 裁定「新版替老版，界面只留一套」）：
#       recoil_bb_enabled 由后端从 recoil.enabled 镜像而来，面板**不发**这个键。
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

WEB_SRC = pathlib.Path(__file__).resolve().parents[3] / 'plugins' / 'web' / 'bin' / 'ttbox-web.py'

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

def test_lead_blocks_map_into_nested_objects():
    mod = _load()
    prof = mod.web_body_to_profile(_body(
        lead1_enabled=True, lead1_frames=12, lead1_strength=0.7,
        lead2_enabled=True, lead2_gain=0.075,
    ))
    l1 = prof['mouse']['lead1']
    assert l1['enabled'] is True
    assert l1['frames'] == 12
    assert abs(l1['strength'] - 0.7) < 1e-9
    # 面板没渲染的字段不凭空写入（Core from_json 会保留原值/默认值）
    assert 'direction_ratio' not in l1
    l2 = prof['mouse']['lead2']
    assert l2['enabled'] is True
    assert abs(l2['gain'] - 0.075) < 1e-12


def test_humanize_and_small_modules_map():
    mod = _load()
    prof = mod.web_body_to_profile(_body(
        humanize_enabled=True, humanize_smooth_factor=0.35,
        accuracy_sim_enabled=True, accuracy_sim_direction=2,
        speed_fluctuation_enabled=True, speed_fluctuation_intensity=0.25,
        anti_overshoot_enabled=True, anti_overshoot_inner_frames=4,
        speed_adaptive_kp_enabled=True, speed_adaptive_kp_move_mult=1.8,
        global_wave_enabled=True, global_wave_freq=1.7,
    ))
    m = prof['mouse']
    assert m['humanize']['enabled'] is True
    assert abs(m['humanize']['smooth_factor'] - 0.35) < 1e-9
    # ★ 2026-09-29：speed_fluctuation / accuracy_sim 已拆成**独立段**（键名不再带 humanize_
    #   前缀），搬运目标也从 mouse.humanize.* 改成 mouse.speed_fluctuation.* /
    #   mouse.accuracy_sim.*。此前那 8 个扁平键 Core 一个都不认 ⇒ 面板上「速度波动」
    #   「精度模拟」两组控件是假开关（调了存不到 core 读的位置）。
    assert m['accuracy_sim']['enabled'] is True
    assert m['accuracy_sim']['direction'] == 2
    assert m['speed_fluctuation']['enabled'] is True
    assert abs(m['speed_fluctuation']['intensity'] - 0.25) < 1e-9
    assert m['anti_overshoot']['inner_frames'] == 4
    assert abs(m['speed_adaptive_kp']['move_mult'] - 1.8) < 1e-9
    assert abs(m['global_wave']['freq'] - 1.7) < 1e-9


def test_vertical_correction_ramps_map():
    mod = _load()
    prof = mod.web_body_to_profile(_body(
        vc_enabled=True, vc_ramp1_enabled=True, vc_ramp1_start=1.25,
        vc_ramp1_duration_ms=900, vc_ramp3_enabled=True, vc_ramp3_end=0.05,
    ))
    vc = prof['mouse']['vertical_correction']
    assert vc['enabled'] is True
    assert vc['ramp1_enabled'] is True
    assert abs(vc['ramp1_start'] - 1.25) < 1e-9
    assert abs(vc['ramp1_duration_ms'] - 900.0) < 1e-9
    assert abs(vc['ramp3_end'] - 0.05) < 1e-9


def test_recoil_bb_scalars_and_tables_map():
    mod = _load()
    vt = [[1, 2, 3], [4, 5, 6], [7, 8, 9]]
    hz = [[0, 0, 0], [-1, -1, -1], [0, 0, 0]]
    prof = mod.web_body_to_profile(_body(
        recoil_bb_enabled=True, recoil_bb_preset=2, recoil_bb_global_vert=0.8,
        recoil_bb_preset_total_time_ms=[100, 200, 300],
        recoil_bb_preset_vert=vt, recoil_bb_preset_horiz=hz,
    ))
    rb = prof['mouse']['recoil_bb']
    assert rb['enabled'] is True
    assert rb['preset'] == 2
    assert abs(rb['global_vert'] - 0.8) < 1e-9
    assert rb['preset_total_time_ms'] == [100.0, 200.0, 300.0]
    assert rb['preset_vert'][2][1] == 8.0
    assert rb['preset_horiz'][1][0] == -1.0


def test_recoil_bb_bad_table_shape_is_skipped():
    mod = _load()
    prof = mod.web_body_to_profile(_body(
        recoil_bb_enabled=True,
        recoil_bb_preset_vert=[[1, 2], [3, 4]],          # 2×2，形状不对
        recoil_bb_preset_total_time_ms=[1, 2],           # 长度不对
    ))
    rb = prof['mouse']['recoil_bb']
    # 标量照收
    assert rb['enabled'] is True
    # 形状不对的数组整套不传，避免把 Core 的表写坏
    assert 'preset_vert' not in rb
    assert 'preset_total_time_ms' not in rb


def test_trigger_keys_use_bitmask():
    mod = _load()
    prof = mod.web_body_to_profile(_body(
        trigger_enabled=True, trigger_key1='middle', trigger_key2='', trigger_key3='forward',
        trigger_click_key='left', trigger_rifle_mode=False, trigger_click_count=7,
    ))
    tg = prof['mouse']['trigger']
    assert tg['key1'] == 4          # middle
    assert tg['key2'] == 0          # '' = 不使用
    assert tg['key3'] == 16         # forward
    assert tg['click_key'] == 1     # left
    assert tg['rifle_mode'] is False
    assert tg['click_count'] == 7


def test_trigger2_keys_use_bitmask():
    mod = _load()
    prof = mod.web_body_to_profile(_body(
        trigger2_enabled=True, trigger2_key1='forward', trigger2_fire_button='left',
        trigger2_with_adv_recoil=True, trigger2_precision_frames=8,
    ))
    tg2 = prof['mouse']['trigger2']
    assert tg2['key1'] == 16
    assert tg2['fire_button'] == 1
    assert tg2['with_adv_recoil'] is True
    assert tg2['precision_frames'] == 8


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
# 1b. 压枪总开关 → BB 引擎（面板只留一套的落地点）
# ---------------------------------------------------------------------------

def test_recoil_master_switch_drives_bb_engine():
    """压枪开关一按，BB 三段查表引擎就得跟着开。

    否则就是"无声失效"：面板显示已开启，实际内核走的是老速率模型
    （strength 默认 0 ⇒ 一点压枪都没有），用户完全看不出来。
    """
    mod = _load()
    prof = mod.web_body_to_profile({
        'ai': {'controller': {'recoil_bb_preset': 2}},
        'recoil': {'enabled': True, 'hotkey': 'left'},
    })
    assert prof['mouse']['recoil']['enabled'] is True
    assert prof['mouse']['recoil_bb']['enabled'] is True
    assert prof['mouse']['recoil_bb']['preset'] == 2


def test_recoil_master_switch_off_turns_bb_engine_off():
    mod = _load()
    prof = mod.web_body_to_profile({
        'ai': {'controller': {}},
        'recoil': {'enabled': False},
    })
    assert prof['mouse']['recoil_bb']['enabled'] is False


def test_recoil_switch_absent_does_not_invent_bb_block():
    """面板没提交压枪块时，不得凭空造出 recoil_bb（Core 会保留原值）。"""
    mod = _load()
    prof = mod.web_body_to_profile(_body(lead1_enabled=True))
    assert 'recoil_bb' not in prof['mouse']


def test_profile_to_web_recoil_switch_sees_bb_engine():
    """回填：recoil_bb.enabled=true 而 recoil.enabled=false ⇒ 面板必须显示「开」。

    只认 recoil.enabled 的话，这种设备面板显示"关、实际在压枪"，
    用户再点一下"开"反而会关掉它。
    """
    mod = _load()
    off = mod.profile_to_web({'mouse': {'recoil': {'enabled': False}}})['recoil']
    assert off['enabled'] is False
    on = mod.profile_to_web({'mouse': {'recoil': {'enabled': False},
                                       'recoil_bb': {'enabled': True}}})['recoil']
    assert on['enabled'] is True
    # 面板不再提交的老压枪参数不该再出现在回填里（避免"幽灵字段"被前端拿去用）
    assert 'strength' not in on
    assert 'humanize_curve_strength' not in on


# ---------------------------------------------------------------------------
# 2. RuntimeProfile → Web（回填）
# ---------------------------------------------------------------------------

def test_profile_to_web_fills_core_defaults():
    """空 profile ⇒ 面板键必须等于 Core 结构体默认值（首次打开显示的就是它）。"""
    mod = _load()
    c = mod.profile_to_web({})['ai']['controller']
    assert c['lead1_enabled'] is False and c['lead1_frames'] == 10
    assert abs(c['lead1_direction_ratio'] - 70.0) < 1e-9
    assert c['lead2_enabled'] is False and c['lead2_y_suppress_enabled'] is True
    assert abs(c['humanize_noise_sigma'] - 0.2) < 1e-9
    assert c['humanize_enabled'] is False
    assert c['anti_overshoot_outer_frames'] == 11 and c['anti_overshoot_inner_frames'] == 6
    assert abs(c['speed_adaptive_kp_move_mult'] - 1.5) < 1e-9
    assert abs(c['global_wave_freq'] - 1.0) < 1e-9
    assert c['vc_enabled'] is True                      # BB 默认 true（外层压枪不开则整段不跑）
    assert c['trigger_enabled'] is False and c['trigger_key1'] == 'middle'
    assert c['trigger2_key1'] == 'forward' and c['trigger2_fire_button'] == 'left'
    assert c['recoil_bb_preset_vert'][2][2] == 1.6      # 第3预设第3段垂直
    assert c['recoil_bb_preset_horiz'][1][0] == -2.0
    assert c['recoil_bb_preset_total_time_ms'] == [1500.0, 1500.0, 1500.0]
    assert c['selector_lock_hold_ms'] == 0.0
    assert c['selector_priority_scoring'] is False
    assert c['selector_hb_head1'] == 1 and c['selector_hb_body2'] == -1


def test_profile_to_web_reads_existing_values():
    mod = _load()
    prof = {'mouse': {
        'lead1': {'enabled': True, 'frames': 15},
        'humanize': {'enabled': True, 'noise_sigma': 0.9},
        'recoil_bb': {'enabled': True, 'preset': 3,
                      'preset_vert': [[1, 1, 1], [2, 2, 2], [3, 3, 3]]},
        'trigger': {'enabled': True, 'key1': 1, 'click_count': 9},
        'lock_hold_ms': 800.0, 'priority_scoring': True,
    }}
    c = mod.profile_to_web(prof)['ai']['controller']
    assert c['lead1_enabled'] is True and c['lead1_frames'] == 15
    assert abs(c['humanize_noise_sigma'] - 0.9) < 1e-9
    assert c['recoil_bb_preset'] == 3
    assert c['recoil_bb_preset_vert'][2][0] == 3
    # 表里没给的第二个/第三个数组要落回默认，而不是 None
    assert c['recoil_bb_preset_horiz'][1][0] == -2.0
    assert c['trigger_enabled'] is True and c['trigger_key1'] == 'left'
    assert c['trigger_click_count'] == 9
    assert c['selector_lock_hold_ms'] == 800.0
    assert c['selector_priority_scoring'] is True


def test_web_roundtrip_is_lossless():
    """面板键 → Core → 面板键，全部原样回来（含键位与数组）。"""
    mod = _load()
    ctrl_in = {
        'lead1_enabled': True, 'lead1_frames': 9, 'lead1_displacement_max': 55.0,
        'lead2_enabled': True, 'lead2_max_offset': 30.0,
        'humanize_enabled': True, 'humanize_overshoot': 0.3, 'humanize_brake_distance': 40.0,
        'anti_overshoot_enabled': True, 'anti_overshoot_outer_distance': 25.0,
        'speed_adaptive_kp_enabled': True, 'speed_adaptive_kp_threshold': 4.0,
        'global_wave_enabled': True, 'global_wave_amp_x': 0.2,
        'vc_enabled': True, 'vc_ramp2_enabled': True, 'vc_ramp2_middle': 0.7,
        'recoil_bb_enabled': True, 'recoil_bb_preset': 2,
        'recoil_bb_preset_total_time_ms': [1200.0, 1300.0, 1400.0],
        'recoil_bb_preset_vert': [[1.1, 1.2, 1.3], [2.1, 2.2, 2.3], [3.1, 3.2, 3.3]],
        'recoil_bb_preset_horiz': [[0.0, 0.0, 0.0], [-2.0, -2.0, -2.0], [0.0, 0.0, 0.0]],
        'trigger_enabled': True, 'trigger_key1': 'back', 'trigger_click_count': 33,
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
    prof = mod.web_body_to_profile(_body(lead1_enabled=True))
    m = prof['mouse']
    assert 'lead1' in m
    for other in ('lead2', 'humanize', 'anti_overshoot', 'speed_adaptive_kp',
                  'global_wave', 'recoil_bb', 'trigger', 'trigger2',
                  'vertical_correction'):
        assert other not in m, other
