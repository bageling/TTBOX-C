"""controller_params.py — 控制器参数块的表驱动读写（2026-10-02 web 换写法 S3）。

从 ``plugins/web/bin/ttbox-web.py`` 整块搬出。**行为逐字保留**。

``CTRL_BLOCKS`` / ``CTRL_SELECTOR_FIELDS`` 是"Web 表单字段 ↔ core 参数"的搬运表；
``_ctrl_read_block`` / ``_ctrl_write_block`` 按表读写，避免手写几十个字段的两向赋值。
"""
from __future__ import annotations

from plugins.web.lib.hotkeys import _bits_to_hotkey, _hotkey_to_bits


CONTROLLER_NUMS = {
    # ★★★ V1.0.43：控制器选择 + Fitts 四参（治 pid1「追着怪/停不住」，默认控制器）。
    #   controller_type 是**字符串**，不走本表（见下方 CTRL_STRINGS）；Fitts 四参是数值。
    'fitts_a_ms': 'fitts_a_ms',          # 时间常数 A（调大=更慢更稳）
    'fitts_b_ms': 'fitts_b_ms',          # 难度系数 B（调大=远处更卖力）
    'fitts_deadzone_px': 'fitts_deadzone_px',  # 死区像素（跟多紧）
    'fitts_ff_gain': 'fitts_ff_gain',    # 速度前馈（提前量强度）
    # ---- 老 pid 控制器（仅 controller_type=pid1 时被 core 消费）----
    'kp_x': 'kp_x', 'kp_y': 'kp_y',
    'kd_x': 'kd_x', 'kd_y': 'kd_y',
    'predict_x': 'predict_x', 'predict_y': 'predict_y',
    'rate_x': 'rate_x', 'rate_y': 'rate_y',
    # ★ V1.0.38：smooth 回归（pid1 完全移植，core 侧 soft-limit 又活了）。
    #   漏掉这两行 ⇒ 面板填的值到不了 core，且 GET 回填永远缺字段。
    'smooth_x': 'smooth_x', 'smooth_y': 'smooth_y',
    'output_deadzone': 'output_deadzone',
    'selector_lost_grace_ms': 'lost_grace_ms',
}

# controller 内的字符串字段（不走数值表；值域是枚举，不是连续量）。
# ★ V1.0.43：controller_type 决定 core 消费哪套参数（fitts 默认 / pid1 备选）。
#   白名单化是必须的：不在白名单的键会被 web 层静默丢弃 ⇒ 面板改了不生效。
CONTROLLER_STRINGS = {
    'controller_type': 'controller_type',   # 'fitts' | 'pid1'
}
# controller 内的布尔直通字段
CONTROLLER_BOOLS = {
    'pull_curve_enabled': '_pc_enabled',
    'personal_trajectory_enabled': '_pt_enabled',
    'lock_confirm_instant_enter_enabled': '_lc_inst_enter_enabled',
    'head_aim_enabled': '_ha_enabled',
}


# ---------------------------------------------------------------------------
# BB 对标新模块（2026-09-24）：面板扁平键 ↔ Core mouse 子对象
#
# 命名约定（与既有 pull_curve_* / continuous_lead_* 完全一致，别另立一套）：
#   面板元素 id = "controller_" + <数据键>；数据键放在 body['ai']['controller'] 这一层。
#   于是前端 collectConfig / populate 只跟扁平键打交道，后端负责折叠成 Core 的嵌套对象。
#
# 表项：(前缀, Core 子对象名, [(字段, 类型, Core 默认值), ...])
#   类型：'n'=浮点 'i'=整数 'b'=布尔 'key'=键位（'left'/'right'/'' ↔ 位掩码）
# ★ 默认值必须与 core/src/mouse/MouseTypes.hpp 的结构体默认值一字不差 ——
#   面板首次回填显示的就是它，对不上会让"没存过配置"的设备显示成另一套参数。
# ★ 本表只做搬运，不做校验；越界值由 Core 的 RuntimeProfile::validate 拦。
CTRL_BLOCKS = [
    # 个人动作曲线：core 一直在用（AimThread 的 personal_gain），但面板**从来没有控件**
    #   （此前只有几行手写搬运，UI 缺）⇒ 只能手改 json。本轮补成表驱动 + 卡片。
    #   只暴露 PersonalMotion 真正读的两个字段（enabled / curve_blend）；
    #   speed_blend / reaction_blend / max_reaction_delay_ms core 从不读，不进表。
    # 压枪速率引擎（2026-09-30 对照 yu 重做后）：只剩「速率 + 门控」两组参数。
    #   拉速 = 3 x strength x speed（px/s），与帧率无关；roi_h 是累计下压上限。
    #   ★ 默认值必须与 core/src/mouse/MouseTypes.hpp::RecoilConfig 一字不差。
    ('recoil', 'recoil', [
        ('only_when_target_visible', 'b', True),
        ('target_lost_release_ms', 'n', 300.0),
        ('trigger_delay_enabled', 'b', False),
        ('trigger_delay_ms', 'n', 120.0),
        ('strength', 'n', 100.0),
        ('speed', 'n', 1.0),
        ('curve_strength', 'n', 0.6),
        ('roi_h', 'n', 300.0),
    ]),
    # BB 扳机 2.0
    ('trigger2', 'trigger2', [
        ('enabled', 'b', False), ('key1', 'key', 16), ('key2', 'key', 0),
        ('fire_button', 'key', 1), ('with_aim', 'b', True),
        ('with_simple_recoil', 'b', False),
        ('confidence', 'n', 0.5), ('first_err', 'n', 30.0), ('first_delay', 'n', 0.0),
        ('fire_interval', 'n', 1.0), ('fire_random', 'n', 0.0), ('fire_count', 'i', 1),
        ('press_duration', 'n', 50.0), ('move_throttle_frames', 'i', 2),
        ('precision_enabled', 'b', False), ('precision_range', 'n', 10.0),
        ('precision_frames', 'i', 5), ('retarget_reset_ms', 'n', 1000.0),
        # ★ lite_mode（精简模式）已从面板撤掉：core 侧没有"跳过部分判定"的实现，
        #   属于空开关。配置键保留在结构体里，但不再搬运。
        ('stop_detect_enabled', 'b', False),
        ('stop_detect_color_id', 'i', 2), ('stop_detect_tolerance', 'n', 60.0),
        ('stop_detect_range', 'n', 80.0), ('stop_detect_interval', 'i', 10),
    ]),
]

# 选靶四项机制：Core 侧是 mouse 顶层扁平键（不是子对象），面板键统一加 selector_ 前缀区分
CTRL_SELECTOR_FIELDS = [
    ('selector_lock_hold_ms', 'lock_hold_ms', 'n', 0.0),
    # 1.5.46 的切靶防抖两条：Core 与 AimThread 早就在用，但面板一直没有界面
    #   （这次「选靶」分区补上）。默认值取 MouseProfile 结构体默认。
    ('selector_switch_hysteresis', 'switch_hysteresis', 'n', 0.5),
    ('selector_switch_cooldown_ms', 'switch_cooldown_ms', 'n', 600.0),
    ('selector_priority_scoring', 'priority_scoring', 'b', False),
    ('selector_weight_dist', 'weight_dist', 'n', 1.0),
    ('selector_weight_size', 'weight_size', 'n', 0.3),
    ('selector_stickiness', 'stickiness', 'n', 1.0),
    ('selector_switch_threshold_px', 'switch_threshold_px', 'n', 60.0),
    ('selector_head_body_stable', 'head_body_stable', 'b', False),
    ('selector_hb_body1', 'hb_body1', 'i', 0),
    ('selector_hb_head1', 'hb_head1', 'i', 1),
    ('selector_hb_body2', 'hb_body2', 'i', -1),
    ('selector_hb_head2', 'hb_head2', 'i', -1),
]


def _coerce_ctrl_value(v, kind):
    """面板值 → Core 侧类型。非法值返回 None（跳过该字段，不把配置写坏）。"""
    if kind == 'b':
        return bool(v)
    if kind == 'key':
        return _hotkey_to_bits(v, 0)
    try:
        return int(v) if kind == 'i' else float(v)
    except (TypeError, ValueError):
        return None


def _ctrl_read_block(ctrl: dict, prefix: str, fields) -> dict:
    """面板扁平键 → Core 子对象（只收面板真的传了的字段）。"""
    blk = {}
    for field, kind, _dflt in fields:
        v = ctrl.get(prefix + '_' + field)
        if v is None:
            continue
        cv = _coerce_ctrl_value(v, kind)
        if cv is not None:
            blk[field] = cv
    return blk


def _ctrl_write_block(ctrl: dict, prefix: str, fields, blk) -> None:
    """Core 子对象 → 面板扁平键（缺字段补 Core 默认值，保证首次回填正确）。"""
    blk = blk if isinstance(blk, dict) else {}
    for field, kind, dflt in fields:
        v = blk.get(field, dflt)
        ctrl[prefix + '_' + field] = (_bits_to_hotkey(v) if kind == 'key' else v)


def _ctrl_read_vec(ctrl: dict, key: str, n: int):
    """一维数组透传（长度不符返回 None，整套跳过）。"""
    v = ctrl.get(key)
    if not isinstance(v, list) or len(v) != n:
        return None
    out = []
    for item in v:
        cv = _coerce_ctrl_value(item, 'n')
        if cv is None:
            return None
        out.append(cv)
    return out


def _ctrl_read_table(ctrl: dict, key: str, rows: int, cols: int):
    """二维数组透传（形状不符返回 None，整套跳过）。"""
    v = ctrl.get(key)
    if not isinstance(v, list) or len(v) != rows:
        return None
    out = []
    for row in v:
        if not isinstance(row, list) or len(row) != cols:
            return None
        crow = []
        for item in row:
            cv = _coerce_ctrl_value(item, 'n')
            if cv is None:
                return None
            crow.append(cv)
        out.append(crow)
    return out


# ---- capture 截取尺寸：Core 合法域 = {0（全帧）} ∪ [64, 3840] ----
# Core 侧校验见 core/src/app/Application.cpp（RuntimeProfile::validate）：
#   "capture.width 非法（0=全帧，或需在 64~3840 之间）"
# Web 面板的"截取尺寸"输入框 min=1，而 profile 里的全帧值 0 回填后会被夹到 1 ⇒ 面板提交 1，
# 1 既不是 0 也不在 64~3840 之间 ⇒ Core 拒收整份 SET_CONFIG ⇒ **面板所有保存全部失败**
# （板端实测 2026-09-19：PUT /api/config 恒 503，报错文案还被误写成"Core 未运行"）。
# 这里做**归一化**（前后端同一套规则，前端见 index.html 的 normalizeCropSize）：
#   <=0 → 0（全帧）；1~63 → 64（Core 下界）；>3840 → 3840。
