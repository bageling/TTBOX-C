"""profile_translate.py — Web 请求体 ↔ RuntimeProfile 双向翻译（2026-10-02 web 换写法 S5）。

从 ``plugins/web/bin/ttbox-web.py`` 整块**原样搬出**（实现一行未改），
入口按老名字 re-export —— 482 个测试的语义一行未动。

职责：面板提交体 → core RuntimeProfile（``web_body_to_profile``）、
      core RuntimeProfile → 面板可读体（``profile_to_web``），
      含 aim_profiles[] 卡组校验（``validate_aim_profiles``）与热键位掩码换算。

## 依赖接缝（照 lib/hub.py 的约定）

* ``lib/hotkeys`` / ``lib/controller_params`` / ``lib/capture_geometry`` **直接 import**：
  纯函数，测试 0 处 monkeypatch。
* ``_get_runtime_profile`` 是**测试 monkeypatch 锚点（29 处）**，必须留在入口 ⇒
  本模块只在**调用时**经 ``hub.call()`` 向入口取，不在 import 期快照。
* 本模块不 import 入口（hub 由入口末尾 bind），**无循环 import**。
"""
from __future__ import annotations

from plugins.web.lib import hub
from plugins.web.lib.capture_geometry import (
    _fov_factor_clamp,
    _fov_radius_to_factor,
    normalize_capture_crop_size,
)
from plugins.web.lib.controller_params import (
    CONTROLLER_BOOLS,
    CONTROLLER_NUMS,
    CTRL_BLOCKS,
    CTRL_SELECTOR_FIELDS,
    _coerce_ctrl_value,
    _ctrl_read_block,
    _ctrl_write_block,
)
from plugins.web.lib.hotkeys import (
    AIM_PROFILE_VALID_BITS,
    HOTKEY_BITS,
    _bits_to_hotkey,
    _hotkey_guard_to_web,
    _hotkey_to_bits,
)


def _get_runtime_profile(*args, **kwargs):
    """留在入口（补丁锚点 ×29），此处仅转发 —— 调用时取，补丁因此在任何调用点生效。"""
    return hub.call('_get_runtime_profile', *args, **kwargs)
AIM_PROFILE_MAX = 8   # 档位数上限（面板能加卡，这里是硬护栏）
AIM_PROFILE_MIN = 1   # 至少一档：core 侧 aim_profiles 非空是不变量


class ConfigValidationError(ValueError):
    """提交体语义非法（JSON 没坏，是参数配错了）。调用方转成 400 + 可读原因。"""


def _aim_profile_core_dict(p: dict) -> dict:
    """单张热键卡 → core aim_profiles[] 的一项。缺字段不写（让 core 吃结构体默认）。"""
    p = p if isinstance(p, dict) else {}
    out: dict = {}
    # 域内才写，越界/负数落回默认值（面板这一层不让非法掩码出网；
    # core 侧另有 sanitize_hotkey_bits 兜底，双保险）
    _hk = _hotkey_to_bits(p.get('hotkey'), 2)
    out['hotkey'] = _hk if _hk in AIM_PROFILE_VALID_BITS else 2
    _hk2 = _hotkey_to_bits(p.get('hotkey2'), 0)
    out['hotkey2'] = _hk2 if _hk2 in AIM_PROFILE_VALID_BITS else 0
    out['hotkey_mode'] = _hotkey_mode_to_web(p.get('hotkey_mode'))
    if p.get('offset_x') is not None:
        out['offset_x'] = p['offset_x']
    if p.get('offset_y') is not None:
        out['offset_y'] = p['offset_y']
    if p.get('class_offsets'):
        out['class_offsets'] = p['class_offsets']
    if p.get('sensitivity') is not None:
        out['sensitivity'] = p['sensitivity']
    if p.get('fov_scale') is not None:
        out['fov_scale'] = _fov_factor_clamp(p['fov_scale'])
    # V1.0.12（2026-09-30）：zoom_scale / gain_px_per_count 不再透传给 core（不区分倍镜）。
    mask = p.get('class_filter_mask')
    if mask is not None:
        m = int(mask or 0)
        out['class_filter'] = [i for i in range(32) if m & (1 << i)]
    return out


def _aim_profile_key_bits(p: dict) -> int:
    """一张卡的键位并集（主 ∪ 副）。与 core 的 aim_profiles_overlap 同一口径。"""
    p = p if isinstance(p, dict) else {}
    hk = _hotkey_to_bits(p.get('hotkey'), 0)
    hk2 = _hotkey_to_bits(p.get('hotkey2'), 0)
    return hk | hk2


def validate_aim_profiles(profiles) -> list:
    """校验并归一化 aim_profiles。非法时抛 ConfigValidationError（带人话原因）。

    规则与 core/src/mouse/MouseTypes.hpp 的注释同源：
      · 至少 1 档、最多 AIM_PROFILE_MAX 档；
      · 档内：主键必须有效；副键不能与主键撞（撞了等于没按副键）；
        「同时按下」模式副键必须非空；
      · 档间：**任意两档的键位并集必须互斥**（按位与 == 0），即"同一个键不能给两档"。
        这条挡的是"单个键按下时判不出该用哪档"的配错。

    ★ 它**不保证**选档唯一，这点别搞混：同时按下两档各自的键（左键开火 + 右键瞄准）
      仍然会两档都命中，物理上禁不掉。core 侧此时取**面板顺序里第一个命中的档**
      （aim_profile_match 命中即返回）。所以"顺序优先"是必需兜底，不是可选优化。

    提交体里的每张卡都必须带 hotkey：core 侧 mouse.aim_profiles 是**整表替换**，
    少一个 hotkey 就会把那一档的键位重置成默认值（右键），属于静默改配置。
    """
    if not isinstance(profiles, list) or not profiles:
        return []
    if len(profiles) > AIM_PROFILE_MAX:
        raise ConfigValidationError(f'热键最多 {AIM_PROFILE_MAX} 组，当前提交了 {len(profiles)} 组')
    key_names = {v: k for k, v in HOTKEY_BITS.items()}
    masks: list = []
    for i, p in enumerate(profiles, start=1):
        p = p if isinstance(p, dict) else {}
        hk = _hotkey_to_bits(p.get('hotkey'), 0)
        hk2 = _hotkey_to_bits(p.get('hotkey2'), 0)
        mode = _hotkey_mode_to_web(p.get('hotkey_mode'))
        if hk == 0:
            raise ConfigValidationError(f'热键 {i}：请选择主按键')
        # ★ 域校验：必须是五个合法键位之一。放行了 -1/32/255 会让 core 侧绕回成
        #   255，命中判据对任意键成立 ⇒ 热键闸门形同虚设。
        if hk not in AIM_PROFILE_VALID_BITS:
            raise ConfigValidationError(
                f'热键 {i}：主按键值非法（{hk}），只能是 左键/右键/中键/侧键1/侧键2 之一')
        if hk2 != 0 and hk2 not in AIM_PROFILE_VALID_BITS:
            raise ConfigValidationError(
                f'热键 {i}：副按键值非法（{hk2}），只能是 左键/右键/中键/侧键1/侧键2 之一')
        if hk & hk2:
            raise ConfigValidationError(f'热键 {i}：副按键不能与主按键相同（同一个键等于没按）')
        if mode == 'all' and hk2 == 0:
            raise ConfigValidationError(f'热键 {i}：触发方式选了「同时按下」，必须再选一个副按键')
        masks.append(hk | hk2)
    for i in range(len(masks)):
        for j in range(i + 1, len(masks)):
            dup = masks[i] & masks[j]
            if dup:
                names = '、'.join(key_names.get(1 << b, str(1 << b))
                                  for b in range(5) if dup & (1 << b))
                raise ConfigValidationError(
                    f'热键 {i + 1} 与热键 {j + 1} 不能共用按键（重复：{names}）。'
                    '一个按键只能归一组 —— 请换一个键，或删掉其中一组')
    return [_aim_profile_core_dict(p) for p in profiles]


def web_body_to_profile(body: dict, prev_profile: dict | None = None) -> dict:
    """Web 前端保存的配置格式（collectConfig 扁平结构）→ RuntimeProfile。

    prev_profile：调用方**已读到的**当前 RuntimeProfile（merge base）。
    ★ 旧实现在函数内部二次 GET_CONFIG 且失败时裸吞异常回落出厂默认 —— 那会把
      真实 FOV 等字段静默重置。现在：调用方传了 prev 就直接用（消灭 TOCTOU）；
      不传（纯函数场景，如测试）用中性默认值，绝不碰 Core。"""
    ctrl = (body.get('ai') or {}).get('controller') or {}
    mouse: dict = {}

    # 1) controller 数值/布尔直通
    for yk, tk in CONTROLLER_NUMS.items():
        if ctrl.get(yk) is not None:
            mouse[tk] = ctrl[yk]
    for yk, tk in CONTROLLER_BOOLS.items():
        if ctrl.get(yk) is not None:
            if tk.startswith('_'):
                continue  # 嵌套结构开关，下面统一处理
            mouse[tk] = bool(ctrl[yk])

    # 2) 插件结构（pull_curve / personal_trajectory / lock_confirm / head_aim / personal_motion）
    pull_curve: dict = {}
    if ctrl.get('pull_curve_enabled') is not None:
        pull_curve['enabled'] = bool(ctrl['pull_curve_enabled'])
    for yk, tk in [('pull_curve_strength', 'strength'),
                   ('pull_curve_min_distance', 'min_distance')]:
        if ctrl.get(yk) is not None:
            pull_curve[tk] = ctrl[yk]
    if pull_curve:
        mouse['pull_curve'] = pull_curve

    # 持续提前量（continuous_lead）—— mouse.continuous_lead.*
    # 字段名与 yu config.json 的 controller.continuous_lead_* 一一对应，便于对标迁移。
    # ★ 有效性说明（勿在面板上误导用户）：core/src/mouse/ContinuousLead.hpp 的
    #   apply() 当前只消费 enabled / enter_distance / scale 三个字段；
    #   fade_in_ms / fade_out_ms / near_disable_ratio 在 Core 侧结构体里标"保留字段"、
    #   算法尚未使用（渐入用的是固定一阶系数、空闲复位用的是固定 300ms）。
    #   所以这 3 个字段**照常存取**（保证与 yu 配置的往返一致、留给后续实现），
    #   但面板上必须显示为"预留"，不能让用户以为调了就有用。
    continuous_lead: dict = {}
    if ctrl.get('continuous_lead_enabled') is not None:
        continuous_lead['enabled'] = bool(ctrl['continuous_lead_enabled'])
    for yk, tk in [('continuous_lead_enter_distance', 'enter_distance'),
                   ('continuous_lead_scale', 'scale'),
                   ('continuous_lead_fade_in_ms', 'fade_in_ms'),
                   ('continuous_lead_fade_out_ms', 'fade_out_ms'),
                   ('continuous_lead_near_disable_ratio', 'near_disable_ratio')]:
        if ctrl.get(yk) is not None:
            continuous_lead[tk] = ctrl[yk]
    if continuous_lead:
        mouse['continuous_lead'] = continuous_lead

    # 拟人化整形（第1项）—— mouse.personal_trajectory.*
    personal_traj = {}
    if ctrl.get('personal_trajectory_enabled') is not None:
        personal_traj['enabled'] = bool(ctrl['personal_trajectory_enabled'])
    for yk, tk in [('personal_trajectory_speed_scale', 'speed_scale'),
                   ('personal_trajectory_stability_scale', 'stability_scale'),
                   ('personal_trajectory_variation_scale', 'variation_scale'),
                   ('personal_trajectory_jitter_amp_px', 'jitter_amp_px'),
                   ('personal_trajectory_fitts_intercept_ms', 'fitts_intercept_ms'),
                   ('personal_trajectory_fitts_slope_ms_per_bit', 'fitts_slope_ms_per_bit')]:
        if ctrl.get(yk) is not None:
            personal_traj[tk] = ctrl[yk]
    if personal_traj:
        mouse['personal_trajectory'] = personal_traj

    # 目标锁定确认（第2项）—— mouse.lock_confirm.*
    lock_confirm = {}
    if ctrl.get('lock_confirm_instant_enter_enabled') is not None:
        lock_confirm['instant_enter_enabled'] = bool(ctrl['lock_confirm_instant_enter_enabled'])
    for yk, tk in [('lock_confirm_confirmation_frames', 'confirmation_frames'),
                   ('lock_confirm_enter_conf', 'enter_conf'),
                   ('lock_confirm_hold_conf', 'hold_conf'),
                   ('lock_confirm_instant_enter_dist', 'instant_enter_dist'),
                   ('lock_confirm_instant_enter_conf', 'instant_enter_conf')]:
        if ctrl.get(yk) is not None:
            lock_confirm[tk] = ctrl[yk]
    if lock_confirm:
        mouse['lock_confirm'] = lock_confirm

    # 压枪（recoil）—— mouse.recoil.*（压枪 12 参数语义，基于 TTBOX 输出链）
    # Web 前端提交在 body 顶层 recoil 块；hotkey 为字符串（'left'/'right'/''）→ 位掩码，
    # hotkey_mode：'all'/'any' → 2/1
    rk = body.get('recoil') or {}
    recoil = {}
    if rk.get('enabled') is not None:
        recoil['enabled'] = bool(rk['enabled'])
    if rk.get('hotkey') is not None:
        recoil['hotkey'] = _hotkey_to_bits(rk['hotkey'], 1) or 1
    if rk.get('hotkey2') is not None:
        recoil['hotkey2'] = _hotkey_to_bits(rk['hotkey2'], 0)
    if rk.get('hotkey_mode') is not None:
        recoil['hotkey_mode'] = 2 if str(rk['hotkey_mode']) == 'all' else 1
    # 速率/门控参数改由下面的 CTRL_BLOCKS['recoil'] 统一搬运，这里只收开关与热键。
    if recoil:
        mouse['recoil'] = recoil

    # ---- BB 对标新模块（2026-09-24）----
    # 表驱动搬运：CTRL_BLOCKS 每个 (前缀, 子对象) 收成 mouse[子对象]。
    # 面板只传它渲染出来的字段 ⇒ 这里"见键就收"，不补默认值（补默认是 Core from_json 的职责）。
    #   ★ 压枪这一块要与上面 body['recoil'] 收进来的开关/热键**合并**（同一个子对象），
    #     直接覆盖会把 enabled/hotkey 一起抹掉。
    for prefix, obj_key, fields in CTRL_BLOCKS:
        blk = _ctrl_read_block(ctrl, prefix, fields)
        if blk:
            cur = mouse.get(obj_key)
            mouse[obj_key] = dict(cur, **blk) if isinstance(cur, dict) else blk

    # 选靶四项机制：Core 侧是 mouse 顶层扁平键
    for web_key, core_key, kind, _dflt in CTRL_SELECTOR_FIELDS:
        v = ctrl.get(web_key)
        if v is None:
            continue
        cv = _coerce_ctrl_value(v, kind)
        if cv is not None:
            mouse[core_key] = cv

    # 热键保护（hotkey_guard）—— mouse.hotkey_guard.*
    # 语义：按一次 toggle_hotkey 在「热键生效 / 全部挂起」之间切换。挂起状态是 Core
    # 运行期状态，不落盘；面板通过 /api/state 的 state.aim.hotkeys_suspended 回读。
    guard = {}
    hg = body.get('hotkey_guard') or {}
    if hg.get('enabled') is not None:
        guard['enabled'] = bool(hg['enabled'])
    if hg.get('toggle_hotkey') is not None:
        # 空字符串/未识别 → 回落 Core 默认的 middle（4），不写 0（0 = 没有切换键 = 永不挂起）
        guard['toggle_hotkey'] = _hotkey_to_bits(hg['toggle_hotkey'], 4) or 4
    if guard:
        mouse['hotkey_guard'] = guard

    # 头部瞄准约束（第3项）—— mouse.head_aim.*
    head_aim = {}
    if ctrl.get('head_aim_enabled') is not None:
        head_aim['enabled'] = bool(ctrl['head_aim_enabled'])
    for yk, tk in [('head_aim_head_offset_top_fraction', 'head_offset_top_fraction'),
                   ('head_aim_head_height_fraction', 'head_height_fraction'),
                   ('head_aim_safe_inset_fraction', 'safe_inset_fraction'),
                   ('head_aim_max_lag_px', 'max_lag_px')]:
        if ctrl.get(yk) is not None:
            head_aim[tk] = ctrl[yk]
    if head_aim:
        mouse['head_aim'] = head_aim

    # 3) 个人移动曲线：TTBOX 自己的 RuntimeProfile 结构
    personal_motion = {}
    for key in ('personal_motion_enabled', 'personal_motion_curve_blend',
                'personal_motion_speed_blend', 'personal_motion_reaction_blend',
                'personal_motion_max_reaction_delay_ms'):
        if ctrl.get(key) is not None:
            target = {
                'personal_motion_enabled': 'enabled',
                'personal_motion_curve_blend': 'curve_blend',
                'personal_motion_speed_blend': 'speed_blend',
                'personal_motion_reaction_blend': 'reaction_blend',
                'personal_motion_max_reaction_delay_ms': 'max_reaction_delay_ms',
            }[key]
            personal_motion[target] = ctrl[key]
    if personal_motion:
        mouse['personal_motion'] = personal_motion

    # 4) 目标选择
    if ctrl.get('selector_lost_grace_ms') is not None:
        mouse['lost_grace_ms'] = ctrl['selector_lost_grace_ms']

    # 5) 瞄准档位 aim_profiles[]：热键 / 瞄准点 / 移动倍率 / FOV 倍率 / 目标类别
    #    ★ 必须整表遍历。旧实现只取 [0]，面板「新增热键」加出来的第 2 张卡起，
    #      所有字段在保存时静默丢弃、刷新后连卡本身都消失 —— 那不是"少读一行"，
    #      是面板长了个 core 没有的功能（面板多卡 UI 来自 e874b2c 照抄上游 YU 面板）。
    #    提交体带 aim_profiles 时整表替换（core 侧数组就是一个整体，没有按键级合并）；
    #    不带时整段不动，避免"改个灵敏度把热键顺手重置"。
    profiles = body.get('aim_profiles')
    core_profiles = validate_aim_profiles(profiles) if profiles is not None else []
    if core_profiles:
        mouse['aim_profiles'] = core_profiles
    p0 = core_profiles[0] if core_profiles else {}
    # 目标类别：推理侧（DecodeNMS）逐帧解码、没有"哪个热键"的概念 ⇒ 只能收**全档并集**，
    # 否则档 2 要用的类别会在推理阶段就被丢掉。瞄准侧再按当前档窄化（AimThread 的
    # scfg.class_filter），单档时两侧相同 ⇒ 行为与加档位之前逐位一致。
    # 提交体不带 aim_profiles 时不动这一项（None 哨兵），免得"改个别处把类别清空"。
    # ★ 2026-09-25 修：哨兵必须与 mouse 段**同解**。原先这里挂的是 `profiles is not None`，
    #   mouse 段挂的是 `if core_profiles` ⇒ `aim_profiles: []` 被当成两种意思：
    #   mouse 段当"未提交"（保留旧档表），inference 段当"已提交"（并集 0 ⇒ class_filter
    #   写 []），而 core 侧 `TargetSelector.cpp` 空 class_filter = **不过滤 = 全类别放行**。
    #   净效果：档表纹丝不动，用户已排除的类别却全都回来了。两边必须挂同一个条件。
    class_union_mask = None
    if core_profiles:
        class_union_mask = 0
        for p in (profiles or []):
            class_union_mask |= int((p or {}).get('class_filter_mask') or 0)

    # 5b) 全局量：sens / pos
    #    sens → sensitivity（输出全局缩放）；pos → offset_y（瞄准高度）。
    #    ★ 卡片的「热键移动倍率」**不再**写进全局 sensitivity：core 侧现在是
    #      out = 全局 sensitivity × 当前档 sensitivity，写进全局就会把倍率乘两遍。
    #      旧实现把 card[0].sensitivity 覆盖到 mouse.sensitivity 上 ⇒ 总览「移动倍率」
    #      那个滑块其实一直是死的（被卡片顶掉），这次一并修掉。
    if body.get('sens') is not None:
        mouse['sensitivity'] = body['sens']
    aim_point_vals: dict = {}
    if p0.get('offset_x') is not None:
        aim_point_vals['offset_x'] = p0['offset_x']
    if p0.get('offset_y') is not None:
        aim_point_vals['offset_y'] = p0['offset_y']
    elif body.get('pos') is not None:
        aim_point_vals['offset_y'] = body['pos']
    if p0.get('class_offsets'):
        mouse['class_offsets'] = p0['class_offsets']
    # RuntimeProfile::from_json 读平铺的 offset_x/offset_y（mouse.aim_point 是内部结构，
    # JSON 层平铺为 mouse.offset_x/mouse.offset_y），此处按 Core 契约平铺写入。
    # 这两个是"全局基底瞄准点"，core 侧再被当前档的 offset 覆盖。
    for k, v in aim_point_vals.items():
        mouse[k] = v

    # 6) 推理参数
    inference: dict = {}
    if body.get('video_detection_confidence') is not None:
        inference['confidence'] = body['video_detection_confidence']
    if body.get('video_detection_iou') is not None:
        inference['iou'] = body['video_detection_iou']
    if class_union_mask is not None:
        # 全档类别并集（见第 5 段的说明）：推理侧不能按档过滤，瞄准侧才按档窄化。
        inference['class_filter'] = [i for i in range(32) if class_union_mask & (1 << i)]

    # 7) 采集
    capture: dict = {}
    cap = body.get('capture') or {}
    if cap.get('crop_size') is not None:
        # ★ 必须归一化：面板下界 1 落在 Core 合法域之外（见 normalize_capture_crop_size 注释）。
        crop = normalize_capture_crop_size(cap['crop_size'])
        capture['width'] = crop
        capture['height'] = crop
    # ★ V1.0.13：crop_offset_x/y → capture.offset_x/y 的映射已删（core 侧字段已删，
    #   裁剪区恒以画面中心为心）。面板也不再发这两个键。

    # 8) FOV 基准半径：瞄准范围 = 截取尺寸内划最大的圆形（业主口径）
    #    半径基准 = 内接圆半径 × range_factor（总览「FOV 半径」）。
    #    ★ 档位倍率（热键卡的「热键 FOV 缩放」）**不在这里乘**：总览半径是全局的、
    #      倍率是按档的，乘法必须在 core 侧按当前档做 ——
    #      core AimThread 取 fov_range = fov.radius × aim_profiles[active].fov_scale。
    #    ★★ V1.0.34 对齐 core 新口径：V1.0.31 起 core 已删掉「fov_range = fov.radius × 2」
    #      （AimThread.cpp:182 现为 fov.radius × fov_scale），fov.radius 本身就是**内接圆比例**：
    #      range_factor=1.0 ⇒ radius=1.0 ⇒ 内接圆（search_radius_px 全额）。故这里直写
    #      radius = range_factor，**不再 /2**。旧的「radius = k/2」是配 core 旧「×2」用的，
    #      core 删 ×2 后这套没同步 ⇒ 整体半径少一半（板端 1.0 只剩 160px，而非内接圆 320px），
    #      预览圈（cropSize × k 直径）因此比真实选靶圈大 2 倍，这就是「预览跟主页不同步」的根因。
    #    旧实现另两处历史问题（已修，勿回退）：enabled 非单调（`enabled = range_factor < 1.0`）、
    #    从不读 fov_scale（回填恒写 1.0）。
    fov: dict = {}
    # ★ 沿用值来源优先级：调用方传入的 merge base（update_config / load_preset 都已
    #   读过 Core，绝不在翻译层里二次 GET —— 那既构成 TOCTOU，也曾把"读失败"静默
    #   回落成出厂默认，保存一次就把真实 FOV 重置）> 直接 GET_CONFIG（测试经
    #   monkeypatch 注入）> 读失败时的中性默认（仅限未传 prev 的纯函数场景）。
    if prev_profile is not None:
        prev_fov = (prev_profile or {}).get('fov') or {}
    else:
        try:
            prev_fov = (_get_runtime_profile() or {}).get('fov') or {}
        except Exception:
            prev_fov = {}
    fov['shape'] = prev_fov.get('shape', 0)
    fov['center_x'] = prev_fov.get('center_x', 0.5)
    fov['center_y'] = prev_fov.get('center_y', 0.5)
    if body.get('range_factor') is None:
        # 没带总览倍率 ⇒ 沿用 Core 现值，本项不参与本次保存
        fov['enabled'] = prev_fov.get('enabled', False)
        fov['radius'] = prev_fov.get('radius', 0.5)
    else:
        fov['enabled'] = True
        fov['radius'] = round(_fov_factor_clamp(body['range_factor']), 6)

    # 9) 预览帧率
    preview: dict = {}
    lat = body.get('latency') or {}
    if lat.get('preview_interval_ms') is not None:
        iv = int(lat['preview_interval_ms'])
        if iv > 0:
            preview['fps'] = max(1, min(60, int(1000 / iv)))

    prof: dict = {
        'mouse': mouse,
        'inference': inference,
        'capture': capture,
        'fov': fov,
    }
    if preview:
        prof['preview'] = preview
    if body.get('model_id') is not None:
        prof['model_id'] = body['model_id']

    return prof


def _hotkey_mode_to_web(v) -> str:
    """core 的触发方式 → 面板选单值（'all' / 'any'）。

    ★ core 侧 aim_profiles[].hotkey_mode 序列化成**字符串** "all"/"any"
      （mouse_hotkey_mode_name），而压枪那一路（recoil.hotkey_mode）是数字 1/2。
      旧回填代码一律写 `mouse.get('aim_hotkey_mode') == 1`，拿字符串比整数 ⇒
      永远判成 'any'：「同时按下」在面板上从来看不到，而且用户下一次保存就把它
      真改成「任一按键」。这里两种形式都认，不再依赖序列化类型。
    """
    if isinstance(v, bool):
        return 'all' if v else 'any'
    if isinstance(v, (int, float)):
        return 'all' if int(v) == 1 else 'any'
    return 'all' if str(v).strip().lower() == 'all' else 'any'


def _aim_profiles_to_web(mouse: dict, inf: dict) -> list:
    """core 的 mouse.aim_profiles → 面板热键卡数组（populate 回填）。

    真实路径：core 序列化出的 mouse.aim_profiles[]（每档含 hotkey/hotkey2/hotkey_mode/
    offset_x/offset_y/class_offsets/class_filter/sensitivity/fov_scale）。
    老配置路径：core 没写数组（1.5.50 及更早的配置，或数组为空）⇒ 用平铺
    aim_hotkey/aim_hotkey2/aim_hotkey_mode + mouse.offset_* + inference.class_filter
    合成一张卡，保证"面板显示的就是实际生效的"，不会因为回填少一项而下次保存写错。

    ★ 档位顺序 = 面板顺序 = core 数组顺序。多键同按（左键+右键）时 core 取数组里
      第一个命中的档，所以这个顺序是有语义的，回填不能重排。
    """
    raw = mouse.get('aim_profiles')
    if isinstance(raw, list) and raw:
        cards = []
        for j in raw:
            j = j if isinstance(j, dict) else {}
            cf = j.get('class_filter')
            mask = 0
            if isinstance(cf, list):
                for i in cf:
                    try:
                        iv = int(i)
                    except (TypeError, ValueError):
                        continue
                    if iv >= 0:
                        mask |= 1 << iv
            ox = j.get('offset_x', 0.5)
            oy = j.get('offset_y', 0.5)
            cards.append({
                'hotkey': _bits_to_hotkey(j.get('hotkey', 2)) or 'right',
                'hotkey2': _bits_to_hotkey(j.get('hotkey2', 0)),
                'hotkey_mode': _hotkey_mode_to_web(j.get('hotkey_mode', 'any')),
                'sensitivity': j.get('sensitivity', 1.0),
                'fov_scale': j.get('fov_scale', 1.0),
                'offset_x': ox,
                'offset_y': oy,
                'alternate_offset_x': ox,
                'alternate_offset_y': oy,
                'class_filter_mask': mask,
                'class_offsets': j.get('class_offsets', []),
                'offset_switch_enabled': False, 'offset_switch_hotkey': '',
            })
        return cards
    # 老配置回退：合成单卡（与 core 侧 at RuntimeProfile 的合成口径一致：
    # sensitivity / fov_scale 都是 1.0 = 不额外缩放；类别继承全局 class_filter）
    return [{
        'hotkey': _bits_to_hotkey(mouse.get('aim_hotkey', 2)) or 'right',
        'hotkey2': _bits_to_hotkey(mouse.get('aim_hotkey2', 0)),
        'hotkey_mode': _hotkey_mode_to_web(mouse.get('aim_hotkey_mode', 'any')),
        'sensitivity': 1.0,
        'fov_scale': 1.0,
        'offset_x': mouse.get('offset_x', 0.5),
        'offset_y': mouse.get('offset_y', 0.5),
        'alternate_offset_x': mouse.get('offset_x', 0.5),
        'alternate_offset_y': mouse.get('offset_y', 0.5),
        'class_filter_mask': sum(1 << int(i) for i in inf.get('class_filter', [])
                                 if int(i) >= 0) if isinstance(inf.get('class_filter'), list) else 0,
        'class_offsets': mouse.get('class_offsets', []),
        'offset_switch_enabled': False, 'offset_switch_hotkey': '',
    }]


def profile_to_web(prof: dict) -> dict:
    """RuntimeProfile → Web 前端需要的格式（populate 回读完整字段）。"""
    mouse = prof.get('mouse') or {}
    # aim_point 在 Core JSON 层是平铺的 mouse.offset_x/mouse.offset_y
    # （RuntimeProfile::to_json 平铺输出，from_json 平铺读取）；
    # mouse.aim_point 子对象只在 C++ 结构体内部存在，JSON 层没有。
    ap = {
        'offset_x': mouse.get('offset_x', 0.5),
        'offset_y': mouse.get('offset_y', 0.5),
    }
    pc = mouse.get('pull_curve') or {}
    lead = mouse.get('continuous_lead') or {}
    fov_p = prof.get('fov') or {}
    prev_p = prof.get('preview') or {}
    inf = prof.get('inference') or {}
    cap = prof.get('capture') or {}
    # 回填「FOV 半径」倍率。上限夹到 1.0：旧配置可能存在 radius=1 & enabled=true
    # （= 2× 内接圆，圆已超出截取区、无实际意义），夹回 1.0。
    fov_factor_web = _fov_radius_to_factor(fov_p.get('radius', 0.5), fov_p.get('enabled'))

    personal_motion = mouse.get('personal_motion') or {}
    personal_traj = mouse.get('personal_trajectory') or {}
    lock_confirm = mouse.get('lock_confirm') or {}
    head_aim = mouse.get('head_aim') or {}
    recoil = mouse.get('recoil') or {}
    ctrl = {
        'kp_x': mouse.get('kp_x'), 'kp_y': mouse.get('kp_y'),
        'kd_x': mouse.get('kd_x'), 'kd_y': mouse.get('kd_y'),
        'predict_x': mouse.get('predict_x'), 'predict_y': mouse.get('predict_y'),
        'rate_x': mouse.get('rate_x'), 'rate_y': mouse.get('rate_y'),
        # ★ V1.0.38：smooth 回归（pid1 完全移植，core 侧 soft-limit 又活了）。
        #   不加这两行 ⇒ 面板能填但存不进 core，且回填永远拿不到值。
        'smooth_x': mouse.get('smooth_x'), 'smooth_y': mouse.get('smooth_y'),
        'output_deadzone': mouse.get('output_deadzone'),
        'selector_lost_grace_ms': mouse.get('lost_grace_ms'),
        'pull_curve_enabled': pc.get('enabled', True),
        'pull_curve_strength': pc.get('strength', 0.8),
        'pull_curve_min_distance': pc.get('min_distance', 80),
        # 持续提前量：默认一律"关"与 Core 侧安全默认一致（未显式开启 ⇒ 输出链不变）。
        'continuous_lead_enabled': lead.get('enabled', False),
        'continuous_lead_enter_distance': lead.get('enter_distance', 150),
        'continuous_lead_scale': lead.get('scale', 0.5),
        'continuous_lead_fade_in_ms': lead.get('fade_in_ms', 300),
        'continuous_lead_fade_out_ms': lead.get('fade_out_ms', 300),
        'continuous_lead_near_disable_ratio': lead.get('near_disable_ratio', 0.66),
        'personal_trajectory_enabled': personal_traj.get('enabled', False),
        'personal_trajectory_speed_scale': personal_traj.get('speed_scale', 1.0),
        'personal_trajectory_stability_scale': personal_traj.get('stability_scale', 1.0),
        'personal_trajectory_variation_scale': personal_traj.get('variation_scale', 1.0),
        'personal_trajectory_jitter_amp_px': personal_traj.get('jitter_amp_px', 0.20),
        'personal_trajectory_fitts_intercept_ms': personal_traj.get('fitts_intercept_ms', 120),
        'personal_trajectory_fitts_slope_ms_per_bit': personal_traj.get('fitts_slope_ms_per_bit', 85),
        'lock_confirm_confirmation_frames': lock_confirm.get('confirmation_frames', 1),
        'lock_confirm_enter_conf': lock_confirm.get('enter_conf', 0.0),
        'lock_confirm_hold_conf': lock_confirm.get('hold_conf', 0.0),
        'lock_confirm_instant_enter_enabled': lock_confirm.get('instant_enter_enabled', True),
        'lock_confirm_instant_enter_dist': lock_confirm.get('instant_enter_dist', 105.0),
        'lock_confirm_instant_enter_conf': lock_confirm.get('instant_enter_conf', 0.5),
        'head_aim_enabled': head_aim.get('enabled', False),
        'head_aim_head_offset_top_fraction': head_aim.get('head_offset_top_fraction', 0.04),
        'head_aim_head_height_fraction': head_aim.get('head_height_fraction', 0.28),
        'head_aim_safe_inset_fraction': head_aim.get('safe_inset_fraction', 0.12),
        'head_aim_max_lag_px': head_aim.get('max_lag_px', 1.25),
        # personal_motion 改由 CTRL_BLOCKS 表驱动搬运（键名规则一致：前缀_字段），
        # 这里不再手写（手写与表并存会互相覆盖，且容易漏同步）。
    }
    # ---- BB 对标新模块（2026-09-24）：Core 子对象 → 面板扁平键 ----
    # 缺字段补 Core 结构体默认值（表里的第三列），保证面板首次打开显示的就是 Core 的实际值。
    for prefix, obj_key, fields in CTRL_BLOCKS:
        _ctrl_write_block(ctrl, prefix, fields, mouse.get(obj_key))
    for web_key, core_key, _kind, dflt in CTRL_SELECTOR_FIELDS:
        ctrl[web_key] = mouse.get(core_key, dflt)

    lat = {}
    if prev_p.get('fps') not in (None, 0):
        try:
            lat['preview_interval_ms'] = max(1, int(1000 / int(prev_p['fps'])))
        except (TypeError, ValueError, ZeroDivisionError):
            lat['preview_interval_ms'] = 66
    else:
        lat['preview_interval_ms'] = 66

    return {
        'model_id': prof.get('model_id', ''),
        'video_detection_confidence': inf.get('confidence'),
        'video_detection_iou': inf.get('iou'),
        'capture': {
            'device': '/dev/video0',
            'crop_size': cap.get('width'),
        },
        'range_factor': fov_factor_web,
        'sens': mouse.get('sensitivity', 1.0),
        'pos': ap.get('offset_y', 0.5),
        'ai': {'controller': ctrl},
        'aim_profiles': _aim_profiles_to_web(mouse, inf),
        'recoil': {
            'enabled': bool(recoil.get('enabled', False)),
            'hotkey': _bits_to_hotkey(recoil.get('hotkey', 1)) or 'left',
            'hotkey2': _bits_to_hotkey(recoil.get('hotkey2', 0)),
            'hotkey_mode': 'all' if recoil.get('hotkey_mode') == 2 else 'any',
        }, 'hotkey_guard': _hotkey_guard_to_web(mouse.get('hotkey_guard')),
        'mouse_output': {'mode': 'full_passthrough'},
        'latency': lat, 'fan_control': {}, 'loopout_overlay': {},
    }
