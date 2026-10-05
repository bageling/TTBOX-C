"""标定（自动闭环）—— 2026-10-02 web 换写法 S4：从 bin/ttbox-web.py **原样搬出**，零逻辑改动。

搬运口径
========
① **留在入口**（没搬）：_calib_target（×3）/ _calib_out_counts（×3）/ _calib_sample_pair（×2）
   —— 都被 monkeypatch.setattr(web_mod, '<名>') 打过补丁，按 hub 规矩必须留在入口模块；
   尤其是 _calib_sample_pair：它被本模块的 _calib_wait_settled / _calib_worker 调用，
   若搬进来则补丁落空（2026-10-02 首次搬运时就是被这一条打回）。
   本模块对这三个用**同名转发**调用，调用时才向入口取 ⇒ 补丁落得实。
② **本模块内被"按源码切片"的测试依赖位置与顺序**：
   · test_web_calibration_amplitude_loop / _gain_source —— 取 `_calib_worker` 开头到
     `_calibration_payload` 开头之间的源码文本做断言；
   · test_web_calibration_apply —— 取 `_calib_apply_gain` 开头到 `_calib_worker` 开头
     之间的源码 exec 后直接调用，命名空间注入 _get_runtime_profile / ipc_request /
     _CFG_WRITE_LOCK 三个名字；
   · test_web_calibration_hot_reload —— 取 `_calibration_payload` 整个函数 exec，
     命名空间注入 _cal_lock / _cal / _read_calibration / _get_runtime_profile。
   ⇒ 三处必须保持**顶层顺序不变**；_calib_apply_gain 体内只许出现**裸名**
     _get_runtime_profile / ipc_request / _CFG_WRITE_LOCK（测试靠注入覆盖），
     不得改写成 hub.get(...)，否则 exec 版一调就 NameError。
③ **hub 转发层**（被 77 处补丁打的名字 + 留在入口的两个）全部集中在下方
   "hub 转发层"一节，定义在任何被切片的函数**之前**，避免混进切片区间。
"""
from __future__ import annotations

import json
import os
import threading
import time

from plugins.web.lib import hub
from plugins.web.lib import paths as ttbox_paths
from plugins.web.lib.locks import _CFG_WRITE_LOCK

from ttbox_motion.calibration import (
    CalibrationAxis,
    CalibrationObservation,
    derive_pid_params,
    fit_axis_measurements,
)


# ====================================================================
# hub 转发层 —— ★ 必须在所有被切片的函数之前定义
#   调用时才向入口模块取名字（import 期快照会让 monkeypatch 落空）。
# ====================================================================
def _get_runtime_profile(*args, **kwargs):
    return hub.call('_get_runtime_profile', *args, **kwargs)


def _get_status(*args, **kwargs):
    return hub.call('_get_status', *args, **kwargs)


def ipc_request(*args, **kwargs):
    return hub.call('ipc_request', *args, **kwargs)


def _calib_target(*args, **kwargs):
    """留在入口（补丁锚点 ×3），此处仅转发。"""
    return hub.call('_calib_target', *args, **kwargs)


def _calib_out_counts(*args, **kwargs):
    """留在入口（补丁锚点 ×3），此处仅转发。"""
    return hub.call('_calib_out_counts', *args, **kwargs)


def _calib_sample_pair(*args, **kwargs):
    """留在入口（补丁锚点 ×2：test_web_calibration_gain_source 的"等静止"两个用例），此处仅转发。"""
    return hub.call('_calib_sample_pair', *args, **kwargs)


# ====================================================================
# 以下为从 bin/ttbox-web.py 原样搬出的实现（行序未动）
# ====================================================================


CALIBRATION_FILE = ttbox_paths.join_path(ttbox_paths.config_dir(), 'calibration.json')
# 标定幅度表（单位 px 的参考点偏置）：**正负交替 + 分量程**。
# 为什么不是旧的全正 [8,16,24,32,40]：慢环下位移逐轮累加（轮间只清 bias、不等瞄点归位）
# ⇒ 比值散开 ⇒ 必挂 fit 的一致性门（MAD/|中位| > 0.35）。交替后相邻两步互相抵消，
# 同时每个幅度都覆盖到，够撑满 fit 的 min_samples=5。
# ★ 2026-09-30 指令三：上限从 ±32 提到 ±96（3 倍）。业主反馈「摆动幅度太小测不出最快
#   速度」——幅度小，闭环随便追得上，分不出系统能跟多快。等比 ×2（12/24/48/96）在同样
#   8 档里把动态范围拉满，大档位用来逼近「最快可追速度」上限（见 max_tracked_amp 闭环）。
CALIB_AMPLITUDES = (12.0, -12.0, 24.0, -24.0, 48.0, -48.0, 96.0, -96.0)
# 摆动幅度闭环（配合 CALIB_AMPLITUDES）：位移达到幅度 × 0.6 算「追上」，追得上就继续往
# 更大的档走；连续追不上 CALIB_AMP_MISS_LIMIT 轮 ⇒ 认为到了系统速度上限，提前收敛停摆
# （不再硬跑更大幅、避免把目标甩飞）。收敛产物 max_tracked_amp = 温和档下能追上的最大幅度。
CALIB_AMP_TRACK_RATIO = 0.6
CALIB_AMP_MISS_LIMIT = 2
# 标定期"温和档" PID：bias 是最高 ±32px 的阶跃，用实战参数在 ~50ms 采集回路延迟下
# 会打进持续振荡（实测 ±150px），把目标甩出画面 ⇒ 整轮 no_target 作废
# （2026-09-24 板上 A/B：kp0.10/kd0.30 十六轮全稳）。gain=Δpx/ΔΣcounts 是闭环恒等式、
# 与 PID 参数无关 ⇒ 压 PID 不影响测量。kd 按 3×kp 给阻尼。
#
# ★★★ V1.0.38（2026-10-05）：**口径随 PID 一起翻回名义值**（业主令 pid 完全移植 pid1）。
#   历史（V1.0.13~V1.0.37）：core 删掉 smooth、把"削弱 99%"折算进 kp，于是 kp 变成**生效值**，
#   温和档也从旧名义 10.0 换成 0.10。
#   现在 core 恢复 smooth（接回 Pid1Controller 第 5 参，AimThread.cpp:274-277）、
#   RuntimeProfile 里的折算整段删除 ⇒ kp 回到**名义值**域。
#   ⚠ 不跟着翻的后果（实测口径推演）：温和档写 0.10 名义 ⇒ 经 smooth=9900 实际只发挥
#     0.001，比板端现役 kp=25（生效 0.25）**弱 250 倍** ⇒ bias 阶跃推不动目标、
#     位移采样全落在噪声里 ⇒ gain 测不准 ⇒ 整轮标定得出错误的 kp（"标定成功但参数是错的"）。
#   ⇒ 换算：名义值 = 生效值 / (10000-smooth)/10000 = 0.10 / 0.01 = **10.0**（即回到 V1.0.13 之前）。
CALIB_PID_KP_MAX = 10.0
CALIB_PID_KD_RATIO = 3.0
# ★ V1.0.38：smooth 又活过来了，标定期**不覆盖**它（保持用户/出厂值 9900）。
#   V1.0.13 删过 CALIB_DEFAULT_SMOOTH_X / CALIB_MAX_SMOOTH_X / _calib_live_smooth()，
#   现在不需要复活那套 —— 标定只改 kp/kd/predict_x，smooth 由用户在面板「缓冲」项自管。
# 单个样本的最低信号门槛：count 太少 ⇒ 分母接近 0，比值被噪声主导；
# 位移太少 ⇒ 被检测噪声（实测静止抖动 ±0.05px）淹没。
CALIB_MIN_COUNTS = 6
CALIB_MIN_DELTA_PX = 0.5
# 当前激活模型标记：跟随 models_root()（V-04：unit 里 TTBOX_MODELS_ROOT=/var/lib/ttbox/models）。
# 以前这里写死过绝对路径 —— core 换根之后就指到不存在的文件去了。
ACTIVE_MODEL_FILE = ttbox_paths.join_path(ttbox_paths.models_root(), 'active_model.txt')
_cal = {
    'phase': 'idle',
    'status': 'idle',       # idle|running|success|failed|manual
    'state': 'idle',        # TTBOX CalibrationState 对外镜像
    'ready': False,
    'reason': 'not_running',  # 保持 Web 契约：未运行时 reason=not_running
    'total_rounds': 8,   # 每轴步数（= len(CALIB_AMPLITUDES)），两轴共 16 步
    'round': 0,
    'progress': 0.0,
    'current_axis': '',
    'round_gains': [],
    'axis_fits': {},
    'valid_sample_count': 0,
    'candidate_count': 0,
    'candidate_track_id': -1,
    'candidate_class_id': -1,
    'candidate_width': 0.0,
    'candidate_height': 0.0,
    'stable_frames': 0,
    'stable_ms': 0,
    'center_jitter_px': 0.0,
    'size_variation': 0.0,
    'thread': None,
    'elapsed_ms': 0,
    'amplitude_counts': 0,   # 旧键名：单位现为 px 偏置（下游无人消费，保留防契约断裂）
    'amplitude_px': 0.0,     # 本轮参考点偏置（px）
    'settle_ms': 0,          # 轮间等瞄点静止耗时
    'settled': False,        # 是否等到静止（未静止不判失败，交给 MAD 门过滤）
    'dropped_sample_count': 0,  # 被门槛丢掉的样本数（同目标/位移/count/落设备）
    'max_tracked_amp': 0.0,     # 闭环收敛产物：温和档下追得上的最大摆动幅度（px），= 最快可追速度的量度
}
_cal_lock = threading.Lock()




def _calib_set(**kw):
    with _cal_lock:
        _cal.update(kw)


def _read_calibration() -> dict:
    try:
        with open(CALIBRATION_FILE, encoding='utf-8') as f:
            return json.load(f)
    except Exception:
        return {}


def _write_calibration(data: dict) -> tuple[bool, str]:
    try:
        os.makedirs(os.path.dirname(CALIBRATION_FILE), exist_ok=True)
        tmp = CALIBRATION_FILE + '.tmp'
        with open(tmp, 'w', encoding='utf-8') as f:
            json.dump(data, f, ensure_ascii=False, indent=2)
        os.replace(tmp, CALIBRATION_FILE)
        return True, '标定参数已保存'
    except Exception as exc:
        return False, f'写入失败: {exc}'


def _clear_calibration() -> None:
    try:
        os.unlink(CALIBRATION_FILE)
    except FileNotFoundError:
        pass


def _read_active_model() -> str:
    try:
        return open(ACTIVE_MODEL_FILE, encoding='utf-8').read().strip()
    except Exception:
        return ''


def _calib_sample_observations(n: int = 3) -> list[dict]:
    observations = []
    for _ in range(n):
        target = _calib_target()
        if target is not None:
            observations.append(target)
        time.sleep(0.05)
    return observations


def _calib_sample_center(n: int = 3):
    observations = _calib_sample_observations(n)
    if not observations:
        return None
    return (
        sum(item['x'] for item in observations) / len(observations),
        sum(item['y'] for item in observations) / len(observations),
    )


def _calib_write_ok() -> int:
    """usbproxy 侧成功写出的包数。与 count 增量配对：不涨 = 注入没落到设备，本样本作废。"""
    st = _get_status()
    m = st.get('metrics', {}) if isinstance(st, dict) else {}
    try:
        return int(m.get('mouse_control_socket_write_ok') or 0)
    except (TypeError, ValueError):
        return 0


def _calib_apply_bias(axis, value: float) -> bool:
    """把参考点偏置写到指定轴（另一轴显式归零），并保持 calibrating=true。

    偏置进的是**控制误差域**（AimThread.cpp:360），所以它是一个"让闭环把瞄点拉到
    参考点 ±value px"的命令；闭环为此付出的 count 才是我们要测的分母。
    """
    prof = _get_runtime_profile()
    mo = prof.setdefault('mouse', {})
    mo['calibration_bias_x'] = float(value) if axis is CalibrationAxis.X else 0.0
    mo['calibration_bias_y'] = float(value) if axis is CalibrationAxis.Y else 0.0
    mo['calibrating'] = True
    return ipc_request('SET_CONFIG', {'profile': prof}).get('status') == 0


def _calib_wait_settled(axis, deadline_s: float = 0.8, quiet_px: float = 0.5,
                        quiet_n: int = 3) -> tuple[bool, float]:
    """清掉偏置后等瞄点静止（连续 quiet_n 个样本之间位移 < quiet_px）。

    为什么要等：上一轮的余速会把"本轮之外的位移"算进 Δpx（旧实现只清 bias 不回零，
    慢环下位移逐轮累加 ⇒ 比值散开 ⇒ fit 的一致性门必挂）。
    沉不下来不判失败 —— 恒等式对任意窗口成立，只是样本会脏一点，交给 MAD 门过滤。
    """
    t0 = time.time()
    win = []
    while time.time() - t0 < deadline_s:
        pair = _calib_sample_pair(axis, 1)
        if pair is None:
            time.sleep(0.02)
            continue
        win.append(pair['px'])
        if len(win) > quiet_n:
            win.pop(0)
        if len(win) >= quiet_n and (max(win) - min(win)) < quiet_px:
            return True, (time.time() - t0) * 1000.0
        time.sleep(0.02)
    return False, (time.time() - t0) * 1000.0


def _calib_apply_gain(calib: dict) -> tuple[bool, str]:
    """标定结果写回 RuntimeProfile。

    修复：标定测的是“每 count 对应多少 px”（gain），这是物理量：
      - gain_x/gain_y_px_per_count 写回 mouse（压枪 recoil_px_per_count、
        拟人化 response_px_per_count 都依赖它，之前未序列化导致标定结果白测）；
      - personal_trajectory.response_px_per_count 联动 gain_y（同语义：px/count）；
      - 不再改写 kp_x/kp_y。旧实现用旧后端 K_LOOP=1/7 反推 kp（25 → ≈0.26），
        在 pid1 体系下（当时 kp/kd 还被 smooth 削掉 99%）输出被缩到 deadzone 以下，
        自瞄直接瘫痪。pid1 的自适应 kp_gain 已处理灵敏度差异，标定不应动 kp。
        ★ V1.0.13 起 kp 已是生效值，这条限制仍然成立：增益归增益、PID 归 PID。
    """
    try:
        gain_x = float(calib.get('mouse_gain_x_px_per_count') or 0)
        gain_y = float(calib.get('mouse_gain_y_px_per_count') or 0)
        if gain_x <= 0 or gain_y <= 0:
            return False, '增益必须 > 0'
        with _CFG_WRITE_LOCK:
            prof = _get_runtime_profile()
            if not prof:
                return False, '读取 RuntimeProfile 失败'
            mo = prof.setdefault('mouse', {})
            mo['gain_x_px_per_count'] = round(gain_x, 4)
            mo['gain_y_px_per_count'] = round(gain_y, 4)
            # 拟人化抖动预算与压枪换算共用同一物理量：px/count 联动。
            # 注意层级：personal_trajectory 是 mouse 的子对象（RuntimeProfile 序列化结构）。
            pt = mo.setdefault('personal_trajectory', {})
            pt['response_px_per_count'] = round(gain_y, 4)
            # V3 阶段 5 前置：实测回路延迟（ms）一并落盘。
            # 此前只存在标定记录里，core 运行时读不到 ⇒ 拟人化抖动前馈没法做延迟对齐
            # （按"下一帧"扣会把前馈自己变成高频扰动）。板端实测 51ms。
            delay_ms = float(calib.get('mouse_response_delay_ms') or 0)
            if delay_ms > 0:
                mo['response_delay_ms'] = round(delay_ms, 2)
            # V1.0.12（2026-09-30）：原先"把 gain 写进指定档位的 gain_px_per_count"已删
            #   （不区分倍镜 ⇒ 不存在"本档 px/count"）。标定结果只写全局 gain（腰射口径）。
            wrote_scope = ''
            r = ipc_request('SET_CONFIG', {'profile': prof})
        ok = r.get('status') == 0
        return ok, (r.get('error', '配置已更新') + wrote_scope)
    except Exception as exc:
        return False, str(exc)


def _calib_derive_pid(gain_x: float, gain_y: float, delay_ms: float) -> dict:
    """按实测 gain/延迟推导 PID（留档与写回共用同一份结果）。

    ★ V1.0.13：不再需要 smooth —— 推导出来的 kp 就是接进环路的**生效值**。
    """
    return derive_pid_params(gain_x, gain_y, delay_ms)


def _calib_apply_pid(calib: dict) -> tuple[bool, str]:
    """自动调参核心：按标定实测 gain + 延迟推导整组 PID 并写回。

    不同客户场景（屏幕灵敏度/DPI/系统延迟/游戏内灵敏度）→ 实测 gain/延迟不同
    → 推导出不同的最佳 KP/KD/predict。只动 kp/kd/predict_x 这三个，
    rate 保持架构常量；**predict_y 一律不动** —— Y 轴预判在面板上已独立可调
    （pid1.cpp 参考默认 0），自动调参不该覆盖业主手设的值。
    """
    try:
        with _CFG_WRITE_LOCK:
            prof = _get_runtime_profile()
            if not prof:
                return False, '读取 RuntimeProfile 失败'
            pid = derive_pid_params(
                float(calib.get('mouse_gain_x_px_per_count') or 0),
                float(calib.get('mouse_gain_y_px_per_count') or 0),
                float(calib.get('mouse_response_delay_ms') or 0),
            )
            mo = prof.setdefault('mouse', {})
            mo['kp_x'] = pid['kp']
            mo['kp_y'] = pid['kp']
            mo['kd_x'] = pid['kd']
            mo['kd_y'] = pid['kd']
            mo['predict_x'] = pid['predict']
            r = ipc_request('SET_CONFIG', {'profile': prof})
        return r.get('status') == 0, r.get('error', '配置已更新')
    except Exception as exc:
        return False, str(exc)


def _calib_worker() -> None:
    """真实标定闭环：稳定检测 → X/Y 分轴正负交替注入参考点偏置 → 用真实注入 count 测 gain。

    gain(px/count) = Δ目标画面位移(px) / ΔΣ注入count —— 闭环恒等式，与 PID 参数、
    与游戏灵敏度无关，对任意时间窗成立（不必等稳态）。参数推导见 derive_pid_params。
    注入：标定时 mouse.calibrating=true（AimThread/OutputBackend 放行 AI 移动），
    kp 输出经现有控制链驱动鼠标 → 目标在画面中位移 → aim_pos_x 反馈。"""
    # ★ 前置段整体持配置锁：GET→改→SET 是读-改-写，不能被用户并发保存插队。
    with _CFG_WRITE_LOCK:
        prof0 = _get_runtime_profile()
        was_enabled = bool((prof0.get('mouse') or {}).get('enabled'))
        mo0 = prof0.setdefault('mouse', {})
        mo0['enabled'] = True
        mo0['calibrating'] = True
        # 入场清零：上一轮若异常退出（进程被杀/重启），板上可能留着非零偏置，
        # 那会让紧接着的"稳定检测"先把目标拉偏、直接判定目标不稳。
        mo0['calibration_bias_x'] = 0.0
        mo0['calibration_bias_y'] = 0.0
        # 温和档 PID（见 CALIB_PID_KP_MAX 注释）。保存用户原值：
        # 失败/取消时在 finally 恢复；成功时推导参数会覆盖，不能回头写旧值。
        saved_kp = mo0.get('kp_x')
        saved_kd = mo0.get('kd_x')
        try:
            calib_kp = min(float(saved_kp), CALIB_PID_KP_MAX)
        except (TypeError, ValueError):
            calib_kp = CALIB_PID_KP_MAX
        mo0['kp_x'] = calib_kp
        mo0['kp_y'] = calib_kp
        mo0['kd_x'] = calib_kp * CALIB_PID_KD_RATIO
        mo0['kd_y'] = calib_kp * CALIB_PID_KD_RATIO
        # ★ 首次 SET_CONFIG 必须查结果：失败还继续跑 = 全程用用户实战 KP 采数据，
        #   温和档根本没写进去，gain 样本全靠 MAD 门硬滤。
        _r0 = ipc_request('SET_CONFIG', {'profile': prof0})
        if _r0.get('status') != 0:
            raise RuntimeError(f'标定前置 SET_CONFIG 失败：{_r0.get("error") or "未知原因"}')
    try:
        _calib_set(state='preparing', status='running', phase='preparing', reason='准备标定环境',
                   round=0, progress=0.0, round_gains=[], candidate_count=0,
                   stable_frames=0, stable_ms=0, valid_sample_count=0, axis_fits={},
                   amplitude_px=0.0, amplitude_counts=0, settle_ms=0, settled=False,
                   dropped_sample_count=0)
        # 1) stabilize：同一目标/类别/尺寸稳定，中心抖动 <1px、尺寸变化 <5%，持续 800ms
        _calib_set(state='stabilize_x', phase='stabilize_x', current_axis='x')
        win, stable_start = [], None
        deadline = time.time() + 12.0
        t0 = time.time()
        while time.time() < deadline:
            if _cal['status'] != 'running':
                _calib_set(state='cancelled', phase='cancelled', reason='cancelled')
                return
            target = _calib_target()
            if target is None:
                win.clear()
                stable_start = None
                _calib_set(reason='no_target', candidate_count=0, stable_frames=0, stable_ms=0,
                           elapsed_ms=int((time.time() - t0) * 1000))
                time.sleep(0.1)
                continue
            if win and (target['target_id'] != win[-1]['target_id'] or
                        target['class_id'] != win[-1]['class_id']):
                win.clear()
                stable_start = None
            win.append(target)
            if len(win) > 10:
                win.pop(0)
            widths = [item['width'] for item in win]
            heights = [item['height'] for item in win]
            jx = max(item['x'] for item in win) - min(item['x'] for item in win)
            jy = max(item['y'] for item in win) - min(item['y'] for item in win)
            size_var = max(
                max(widths) - min(widths), max(heights) - min(heights)
            ) / max(max(widths + heights), 1.0)
            with _cal_lock:
                _cal['candidate_count'] = len(win)
                _cal['candidate_track_id'] = target['target_id']
                _cal['candidate_class_id'] = target['class_id']
                _cal['candidate_width'] = target['width']
                _cal['candidate_height'] = target['height']
                _cal['center_jitter_px'] = max(jx, jy)
                _cal['size_variation'] = size_var
                _cal['stable_frames'] = len(win)
            if len(win) >= 10 and jx < 1.0 and jy < 1.0 and size_var < 0.05:
                if stable_start is None:
                    stable_start = time.time()
                stable_ms = int((time.time() - stable_start) * 1000)
                _calib_set(state='stabilize_x', stable_ms=stable_ms, ready=True, reason='ready',
                           elapsed_ms=int((time.time() - t0) * 1000))
                if stable_ms >= 800:
                    break
            else:
                stable_start = None
                _calib_set(ready=False, reason='target_unstable', stable_ms=0)
            time.sleep(0.05)
        else:
            _calib_set(state='failed', status='failed', phase='error', reason='目标稳定检测超时', ready=False)
            return
        # 2) X/Y 分轴采样：注入**正负交替**的参考点偏置，用**真实注入 count** 当分母测 gain。
        #
        # 物理依据（本次修复的核心）：闭环里相机位移由 count 积分而来，于是恒有
        #     目标在画面里的位移(px) ≡ gain(px/count) × Σ注入count
        # 该恒等式与 PID 参数、与游戏灵敏度都无关，且对**任意时间窗**成立（不必等稳态）。
        # ⇒ gain = Δpx / ΔΣcounts。分母必须是真实 count（core 的 aim_out_counts_*）。
        # 旧实现拿"偏置的 px"当分母（量纲 px/px）⇒ 比值恒 ≈1.0 ⇒ 标定即使成功，
        # 写出的 kp 也只由那个假 gain 推出（实测恒为 15），与真实手感无关。
        if _calib_out_counts() is None:
            _calib_set(state='failed', status='failed', phase='error', ready=False,
                       reason='当前 Core 不提供 aim_out_counts_*（需 1.5.51 及以上）：'
                              '拿不到真实注入 count，无法测出物理 gain')
            return
        amplitudes = CALIB_AMPLITUDES
        axis_observations = {CalibrationAxis.X: [], CalibrationAxis.Y: []}
        dropped = {CalibrationAxis.X: 0, CalibrationAxis.Y: 0}
        no_write = False
        max_tracked_amps = []   # 每轴「最快可追幅度」闭环产物（[X, Y]，单位 px）
        for axis in (CalibrationAxis.X, CalibrationAxis.Y):
            _calib_set(
                state=f'stabilize_{axis.value}',
                phase=f'stabilize_{axis.value}',
                current_axis=axis.value,
                round=0,
                progress=0.5 if axis is CalibrationAxis.Y else 0.0,
            )
            # 每轴动作前重新确认同一候选，避免目标切换混入测量。
            # 摆动幅度闭环：追得上就继续走大档，连续追不上就收敛停摆。
            # max_tracked_amp 是这轮闭环的产物（最快可追幅度），随轴独立。
            max_tracked_amp = 0.0
            miss_streak = 0
            for index, amp in enumerate(amplitudes):
                if _cal['status'] != 'running':
                    _calib_set(state='cancelled', phase='cancelled', reason='cancelled')
                    return
                # 轮间回零：先把偏置清掉并**等瞄点静止**，否则上一轮的余速会算进本轮 Δpx。
                if not _calib_apply_bias(axis, 0.0):
                    _calib_set(state='failed', status='failed', phase='error',
                               reason='Core 配置应用失败', ready=False)
                    return
                settled, settle_ms = _calib_wait_settled(axis)
                _calib_set(
                    state=f'sampling_{axis.value}',
                    phase=f'measure_{axis.value}_response',
                    current_axis=axis.value,
                    round=index + 1,
                    amplitude_px=amp,
                    amplitude_counts=amp,   # 旧键名兼容：单位现为 px 偏置（下游无人消费）
                    settle_ms=int(settle_ms),
                    settled=settled,
                    progress=(index + (0 if axis is CalibrationAxis.X else 8)) / 16.0,
                )
                start = _calib_sample_pair(axis)
                if start is None:
                    _calib_set(state='failed', status='failed', phase='error',
                               reason='no_target', ready=False)
                    return
                write0 = _calib_write_ok()
                if not _calib_apply_bias(axis, amp):
                    _calib_set(state='failed', status='failed', phase='error',
                               reason='Core 配置应用失败', ready=False)
                    return
                # 采样窗：位移够了就收工（恒等式对任意窗口成立），或到窗口上限。
                # 同时测**真实响应延迟**（施加偏置 → 位移首次 ≥0.3px），它要喂给 PID 推导，
                # 不能用上面等静止的 settle_ms（那是几百 ms 量级，会被 fit 的 ≤50ms 直接拒）。
                injected_at = time.monotonic()
                target_px = amp * CALIB_AMP_TRACK_RATIO
                first_response_ms = None
                tracked = False   # ★ 闭环判据：采样窗内位移是否追到幅度 × 0.6
                deadline = injected_at + 0.8
                while time.monotonic() < deadline:
                    if _cal['status'] != 'running':
                        _calib_apply_bias(axis, 0.0)
                        _calib_set(state='cancelled', phase='cancelled', reason='cancelled')
                        return
                    time.sleep(0.008)
                    cur = _calib_target()
                    if cur is None or cur['target_id'] != start['target']['target_id']:
                        continue
                    moved = abs((cur['x'] if axis is CalibrationAxis.X else cur['y']) - start['px'])
                    if first_response_ms is None and moved >= 0.3:
                        first_response_ms = (time.monotonic() - injected_at) * 1000.0
                    if moved >= abs(target_px):
                        tracked = True
                        break
                end = _calib_sample_pair(axis)
                # 本轮结束立即回零（下一轮开头还会再清一次并等静止）。
                _calib_apply_bias(axis, 0.0)
                _calib_set(valid_sample_count=sum(len(v) for v in axis_observations.values()))
                if end is None:
                    continue
                d_px = abs(end['px'] - start['px'])
                d_counts = abs(end['counts'] - start['counts'])
                same_target = (end['target']['target_id'] == start['target']['target_id'] and
                               end['target']['class_id'] == start['target']['class_id'])
                # 本窗口内 usbproxy 是否真的写出过包：写了 count 却没写出包 = 注入没生效，
                # 此时画面里即使有位移也不是我们造成的 ⇒ 本样本必须作废。
                wrote = (_calib_write_ok() - write0) > 0
                if same_target and d_px >= CALIB_MIN_DELTA_PX and d_counts >= CALIB_MIN_COUNTS and wrote:
                    axis_observations[axis].append(CalibrationObservation(
                        axis=axis,
                        injected_count=float(d_counts),
                        measured_delta_px=d_px,
                        response_delay_ms=float(first_response_ms if first_response_ms is not None
                                                else (time.monotonic() - injected_at) * 1000.0),
                        target_id=f"{start['target']['target_id']}:{start['target']['class_id']}",
                        valid=True,
                    ))
                else:
                    dropped[axis] += 1
                    if d_counts >= CALIB_MIN_COUNTS and not wrote:
                        no_write = True
                    elif wrote and d_counts >= CALIB_MIN_COUNTS and d_px < CALIB_MIN_DELTA_PX:
                        # 注入生效但位移不够 ⇒ 温和档对这个低 gain 系统太慢：
                        # 轮间抬 KP（不超过用户原配置），让后续轮补测。
                        try:
                            kp_cap = float(saved_kp)
                        except (TypeError, ValueError):
                            kp_cap = CALIB_PID_KP_MAX
                        new_kp = min(calib_kp * 1.7, max(kp_cap, CALIB_PID_KP_MAX))
                        if new_kp > calib_kp + 1e-6:
                            calib_kp = new_kp
                            prof = _get_runtime_profile()
                            mo = prof.setdefault('mouse', {})
                            mo['kp_x'] = mo['kp_y'] = calib_kp
                            mo['kd_x'] = mo['kd_y'] = calib_kp * CALIB_PID_KD_RATIO
                            ipc_request('SET_CONFIG', {'profile': prof})
                            _calib_set(reason='低增益：已抬高标定期 KP 继续测量')
                _calib_set(dropped_sample_count=sum(dropped.values()))
                # ★ 摆动幅度闭环（2026-09-30 指令三）：追得上 → 记最快可追幅度、清零 miss；
                #   追不上（注入生效却没过 60% 幅度）→ 计数；连续 miss 到上限 ⇒ 提前收敛停摆，
                #   不再硬跑更大幅、避免把目标甩飞。max_tracked_amp = 温和档最快可追速度的量度。
                if tracked:
                    max_tracked_amp = max(max_tracked_amp, abs(amp))
                    _calib_set(max_tracked_amp=max_tracked_amp)
                    miss_streak = 0
                elif wrote:
                    miss_streak += 1
                    if miss_streak >= CALIB_AMP_MISS_LIMIT:
                        _calib_set(max_tracked_amp=max_tracked_amp,
                                   reason=f'{axis.value}轴摆动收敛：最快可追幅度 ≈ {max_tracked_amp:.0f}px')
                        break
            if not axis_observations[axis]:
                # 整轴一个样本都没过门槛：与其让 fit 报一句笼统的"有效样本不足"，
                # 不如把卡在哪说清（位移够不够 / 闭环有没有真的动 / 目标是不是被甩出画面）。
                _calib_set(
                    state='failed', status='failed', phase='error', ready=False,
                    reason=f'{axis.value}轴无有效样本（{len(amplitudes)} 轮全部低于门槛：'
                           f'位移需 ≥{CALIB_MIN_DELTA_PX}px 且 count 需 ≥{CALIB_MIN_COUNTS}'
                           f'，或目标在采样中被甩出画面）',
                )
                return
            _calib_set(
                state=f'analyzing_{axis.value}',
                phase=f'measure_{axis.value}_settle',
                current_axis=axis.value,
            )
            max_tracked_amps.append(max_tracked_amp)
        if no_write:
            _calib_set(state='failed', status='failed', phase='error', ready=False,
                       reason='注入的 count 没有落到 usbproxy（检查输出后端与连线）')
            return
        _calib_set(state='validating', phase='validating', current_axis='', progress=0.9)
        fits = {
            axis: fit_axis_measurements(axis, values)
            for axis, values in axis_observations.items()
        }
        _calib_set(axis_fits={
            axis.value: {
                'gain_px_per_count': fit.gain_px_per_count,
                'response_delay_ms': fit.response_delay_ms,
                'sample_count': fit.sample_count,
                'rejected_count': fit.rejected_count,
                'consistency': fit.consistency,
                'converged': fit.converged,
                'failure_reason': fit.failure_reason,
            }
            for axis, fit in fits.items()
        })
        if not all(fit.converged for fit in fits.values()):
            reason = '; '.join(fit.failure_reason for fit in fits.values() if not fit.converged)
            _calib_set(state='failed', status='failed', phase='error', reason=reason or '轴向拟合失败', ready=False)
            return
        gain_x = fits[CalibrationAxis.X].gain_px_per_count
        gain_y = fits[CalibrationAxis.Y].gain_px_per_count
        delay_ms = max(fits[CalibrationAxis.X].response_delay_ms, fits[CalibrationAxis.Y].response_delay_ms)
        conf = round(min(fits[CalibrationAxis.X].consistency, fits[CalibrationAxis.Y].consistency), 3)
        _calib_set(round_gains=[gain_x, gain_y], progress=0.98, phase='saving', state='applying')
        calib = {
            'mouse_gain_x_px_per_count': round(gain_x, 4),
            'mouse_gain_y_px_per_count': round(gain_y, 4),
            'mouse_response_delay_ms': round(delay_ms, 2),
            'mouse_calibration_applied': True,
            'valid': True,
            'confidence': conf,
            'calibrated_at': time.strftime('%Y%m%d_%H%M%S'),
            'model_id': _read_active_model(),
            'capture': {'crop_size': int((_get_runtime_profile().get('preview') or {}).get('roi_w') or 320)},
            'rounds': len(axis_observations[CalibrationAxis.X]) + len(axis_observations[CalibrationAxis.Y]),
            # 摆动闭环产物：两轴各自「最快可追幅度」+ 整体瓶颈（两轴较小者）。
            # 后续 rate（跟随速度）推导的数据源，当前先落盘留痕（推导待 pid_sim 验证）。
            'max_tracked_amp_x': round(max_tracked_amps[0], 2),
            'max_tracked_amp_y': round(max_tracked_amps[1], 2),
            'max_tracked_amp': round(min(max_tracked_amps), 2),
        }
        # 自动调参：按实测 gain/延迟推导 KP/KD/predict（pid1 体系，见
        # ttbox_motion/calibration.derive_pid_params + core/tools/pid_sim 仿真验证）
        try:
            calib['pid_params'] = _calib_derive_pid(gain_x, gain_y, delay_ms)
        except Exception:
            calib['pid_params'] = {}
        ok, detail = _write_calibration(calib)
        if ok:
            ok2, detail2 = _calib_apply_gain(calib)
            detail = detail + '；' + detail2
            ok = ok and ok2
            if ok2 and calib.get('pid_params'):
                ok3, detail3 = _calib_apply_pid(calib)
                detail = detail + '；' + detail3
                ok = ok and ok3
        _calib_set(
            state='completed' if ok else 'failed',
            status='success' if ok else 'failed',
            reason='completed' if ok else detail,
            ready=ok,
            progress=1.0 if ok else 0.98,
            phase='completed' if ok else 'error',
        )
    except Exception as exc:
        # ★ 2026-09-25 补：此前只有 finally、没有 except。线程内任何未预期异常
        #   （最典型：标定中途 core 重启/掉线 ⇒ _get_runtime_profile() 抛
        #   CoreUnavailableError）会穿透线程，而 finally 只清偏置/恢复 PID，
        #   **不落终态** ⇒ status 永远停在 running、state 停在非终态 ⇒ 面板 pill
        #   假装"运行中 N%"、取消按钮被禁用（disabled = !running，此时 running=False）、
        #   且永远 800ms 轮询。用户点不掉，只能重启 web。
        _calib_set(state='failed', status='failed', phase='error', ready=False,
                   reason=f'标定异常：{exc!r}')
    finally:
        # ★ 恢复段也是读-改-写，必须持锁，否则会覆盖用户并发保存的参数。
        with _CFG_WRITE_LOCK:
            try:
                prof = _get_runtime_profile()
                mo = prof.setdefault('mouse', {})
                mo['calibrating'] = False
                # ★ 偏置必须一起归零：中途取消/失败时若留着 calibration_bias_*，
                #   参考点会被永久顶偏（表现为"标定失败之后自瞄一直瞄偏"），只能靠重启清掉。
                mo['calibration_bias_x'] = 0.0
                mo['calibration_bias_y'] = 0.0
                # 温和档只在标定期生效：成功路径推导参数已由 _calib_apply_pid 写入，
                # 不能覆盖回去；失败/取消则恢复用户原 KP/KD。
                if _cal['state'] in ('failed', 'cancelled'):
                    if saved_kp is not None:
                        mo['kp_x'] = mo['kp_y'] = saved_kp
                    if saved_kd is not None:
                        mo['kd_x'] = mo['kd_y'] = saved_kd
                if not was_enabled:
                    mo['enabled'] = False
                ipc_request('SET_CONFIG', {'profile': prof})
            except Exception:
                pass


def _calib_thread_entry() -> None:
    """线程 target（真正 target=_calib_worker 的是本函数）：
    _calib_worker 的 except 从前置段之后的 try 才开始 —— 前置段（GET_CONFIG /
    首次 SET_CONFIG）抛 CoreUnavailableError 时内部 except/finally 都够不着，
    状态机会停在启动前的旧值（面板假运行）。这里兜最后一道网，落终态。"""
    try:
        _calib_worker()
    except Exception as exc:
        _calib_set(state='failed', status='failed', phase='error', ready=False,
                   reason=f'标定异常：{exc!r}')


def _calibration_payload() -> dict:
    with _cal_lock:
        runtime = {
            'running': bool(_cal['thread'] and _cal['thread'].is_alive()),
            'phase': _cal['phase'],
            'state': _cal['state'],
            'status': _cal['status'],
            'ready': _cal['ready'],
            'reason': _cal['reason'],
            'total_rounds': _cal['total_rounds'],
            'round': _cal['round'],
            'progress': _cal['progress'],
            'current_axis': _cal['current_axis'],
            'valid_sample_count': _cal['valid_sample_count'],
            'axis_fits': _cal['axis_fits'],
            'candidate_count': _cal['candidate_count'],
            'candidate_track_id': _cal['candidate_track_id'],
            'candidate_class_id': _cal['candidate_class_id'],
            'candidate_width': _cal['candidate_width'],
            'candidate_height': _cal['candidate_height'],
            'candidate_rect': {
                'x': 0, 'y': 0,
                'width': int(_cal['candidate_width']),
                'height': int(_cal['candidate_height']),
            },
            'stable_frames': _cal['stable_frames'],
            'stable_ms': _cal['stable_ms'],
            'center_jitter_px': _cal['center_jitter_px'],
            'size_variation': _cal['size_variation'],
            'elapsed_ms': _cal['elapsed_ms'],
            'amplitude_counts': _cal['amplitude_counts'],
            'amplitude_px': _cal['amplitude_px'],
            'settle_ms': _cal['settle_ms'],
            'settled': _cal['settled'],
            'dropped_sample_count': _cal['dropped_sample_count'],
            'max_tracked_amp': _cal['max_tracked_amp'],
            'error': '' if _cal['status'] != 'failed' else _cal['reason'],
        }
    record = _read_calibration()
    # ★ 2026-09-25 修复：gain 一律取**当前生效值**（RuntimeProfile 的 mouse 段），
    #   留档文件 calibration.json 只回答"标定过没有 + 元信息"。
    #   旧实现直接拿留档当"当前标定"，而板端实测 calibration.json **根本不存在** ⇒
    #   接口返回硬编码的 0.55 / 8.333ms 冒充当前标定，与 core 真实在用的 0.65 不一致。
    #   两个真源必然漂移（profile 被别的路径改过、或 _calib_apply_gain 写失败时）。
    eff = {}
    try:
        emo = (_get_runtime_profile().get('mouse')) or {}
        # ★ 除了增益，PID 三件也一并回「当前生效值」：标定写回 kp/kd/predict_x 后，
        #   前端要拿它刷新面板控件（不用刷新页面），且这里才是 core 真正在用的值
        #   （留档文件里的 pid_params 只回答"当时推导了多少"）。
        for key in ('gain_x_px_per_count', 'gain_y_px_per_count',
                    'kp_x', 'kd_x', 'predict_x'):
            try:
                eff[key] = round(float(emo[key]), 4)
            except (KeyError, TypeError, ValueError):
                pass
    except Exception:
        # core 离线：生效值未知。如实留空，**绝不**拿留档或默认值冒充生效值。
        pass

    def _rec(key, default=None, prefix=''):
        if not record:
            return default
        node = record
        for part in prefix.split(':') if prefix else []:
            node = (node or {}).get(part) or {}
        return node.get(key, default)

    # 保持 Web 契约：始终返回全 10 字段（未知的给 None，不再造假数字）
    calib = {
        'valid': bool(record.get('valid')) if record else False,
        'gain_x_px_per_count': eff.get('gain_x_px_per_count',
                                       _rec('mouse_gain_x_px_per_count')),
        'gain_y_px_per_count': eff.get('gain_y_px_per_count',
                                       _rec('mouse_gain_y_px_per_count')),
        'response_delay_ms': _rec('mouse_response_delay_ms'),
        'confidence': _rec('confidence', 0),
        'model_id': _rec('model_id', ''),
        'calibrated_at': _rec('calibrated_at', ''),
        'capture_width': _rec('crop_size', 0, prefix='capture'),
        'capture_height': _rec('crop_size', 0, prefix='capture'),
        'crop_size': _rec('crop_size', 0, prefix='capture'),
        # 本轮推导出来的 PID（只是"当时推了多少"，生效值看 effective）
        'pid_params': _rec('pid_params') or {},
        # 生效值单列一份：面板可在"未标定"时如实展示"当前运行配置里的增益"
        'effective': eff,
    }
    return {'runtime': runtime, 'calibration': calib}
