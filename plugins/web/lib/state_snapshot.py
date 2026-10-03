"""state_snapshot.py — 面板状态快照装配（2026-10-02 web 换写法 S5）。

从 ``plugins/web/bin/ttbox-web.py`` 的 ``_models_view`` / ``collect_web_state`` 两块
**原样搬出**，入口按老名字 re-export —— 482 个测试的语义一行未动。

## 依赖接缝（见 lib/hub.py 顶部约定）

段内共引用入口的 16 个函数，其中 9 个是测试的 monkeypatch 锚点 ——
``_get_status`` / ``_get_runtime_profile`` / ``ipc_request`` / ``_license_block`` /
``_ui_block`` / ``_calibration_payload`` / ``_core_state_payload`` / ``_fan_control_payload`` /
``_loopout_payload``（见 ``plugins/web/tests/test_web_model_input.py::_patch_state_deps``）。
这些名字**必须留在入口**，故本模块一律经 ``hub.call()`` 在**调用时**向入口取，
不在 import 期快照；其余 7 个（``_auto_start_payload`` / ``_ota_current_version`` /
``_preset_names`` / ``_cloud_license_subblock`` / ``_models_*`` 等）同法处理，避免以后
有人给它们加补丁时又踩一次。

## 与搬运原文的两处偏离（全文件唯一两处，其余逐行原样）

``_PREVIEW_MONITOR`` 是入口 preview 区定义的模块级字典，本模块 import 期它还不存在，
故改经 ``_preview_monitor()`` 取 —— 同样是**调用时**取，拿到的是入口那一个对象，
预览路由对 ``active_conns`` / ``last_frame_ts`` 的累加照样可见。
"""
from __future__ import annotations

import time

from plugins.web.lib import hub
from plugins.web.lib.profile_translate import profile_to_web
from plugins.web.lib.settings import kAppVersion


def _preview_monitor():
    """入口的预览连接计数（定义在 preview 区，import 期取不到 ⇒ 调用时取）。"""
    return hub.get('_PREVIEW_MONITOR')



def _auto_start_payload(*args, **kwargs):
    """留在入口（补丁锚点 / 晚定义），此处仅转发 —— 调用时取。"""
    return hub.call('_auto_start_payload', *args, **kwargs)

def _calibration_payload(*args, **kwargs):
    """留在入口（补丁锚点 / 晚定义），此处仅转发 —— 调用时取。"""
    return hub.call('_calibration_payload', *args, **kwargs)

def _cloud_license_subblock(*args, **kwargs):
    """留在入口（补丁锚点 / 晚定义），此处仅转发 —— 调用时取。"""
    return hub.call('_cloud_license_subblock', *args, **kwargs)

def _core_state_payload(*args, **kwargs):
    """留在入口（补丁锚点 / 晚定义），此处仅转发 —— 调用时取。"""
    return hub.call('_core_state_payload', *args, **kwargs)

def _effective_rknn_concurrency(*args, **kwargs):
    """留在入口（补丁锚点 / 晚定义），此处仅转发 —— 调用时取。"""
    return hub.call('_effective_rknn_concurrency', *args, **kwargs)

def _fan_control_payload(*args, **kwargs):
    """留在入口（补丁锚点 / 晚定义），此处仅转发 —— 调用时取。"""
    return hub.call('_fan_control_payload', *args, **kwargs)

def _get_runtime_profile(*args, **kwargs):
    """留在入口（补丁锚点 / 晚定义），此处仅转发 —— 调用时取。"""
    return hub.call('_get_runtime_profile', *args, **kwargs)

def _get_status(*args, **kwargs):
    """留在入口（补丁锚点 / 晚定义），此处仅转发 —— 调用时取。"""
    return hub.call('_get_status', *args, **kwargs)

def _is_importing(*args, **kwargs):
    """留在入口（补丁锚点 / 晚定义），此处仅转发 —— 调用时取。"""
    return hub.call('_is_importing', *args, **kwargs)

def _license_block(*args, **kwargs):
    """留在入口（补丁锚点 / 晚定义），此处仅转发 —— 调用时取。"""
    return hub.call('_license_block', *args, **kwargs)

def _loopout_payload(*args, **kwargs):
    """留在入口（补丁锚点 / 晚定义），此处仅转发 —— 调用时取。"""
    return hub.call('_loopout_payload', *args, **kwargs)

def _merge_model_ui_meta(*args, **kwargs):
    """留在入口（补丁锚点 / 晚定义），此处仅转发 —— 调用时取。"""
    return hub.call('_merge_model_ui_meta', *args, **kwargs)

def _ota_current_version(*args, **kwargs):
    """留在入口（补丁锚点 / 晚定义），此处仅转发 —— 调用时取。"""
    return hub.call('_ota_current_version', *args, **kwargs)

def _preset_names(*args, **kwargs):
    """留在入口（补丁锚点 / 晚定义），此处仅转发 —— 调用时取。"""
    return hub.call('_preset_names', *args, **kwargs)

def _ui_block(*args, **kwargs):
    """留在入口（补丁锚点 / 晚定义），此处仅转发 —— 调用时取。"""
    return hub.call('_ui_block', *args, **kwargs)

def ipc_request(*args, **kwargs):
    """留在入口（补丁锚点 / 晚定义），此处仅转发 —— 调用时取。"""
    return hub.call('ipc_request', *args, **kwargs)


def _models_view(ml_data: dict) -> list:
    """MODEL_LIST data → 面板模型卡片数组（单一真源）。

    /api/state.data.models、/api/models、/api/models/select 三处**必须同形**，否则
    切换模型后面板卡片字段会漂移（参照物 applySelectedModel 会把 result.models 直接
    灌进 state.data.models）。故集中在本函数，杜绝多处各写一份。
    """
    models = []
    for mm in (ml_data or {}).get('models', []):
        models.append({
            'id': mm.get('model_id'),
            'model_id': mm.get('model_id'),
            'name': mm.get('label') or mm.get('model_id'),
            'display_name': mm.get('label') or mm.get('model_id'),
            'label': mm.get('label'),
            'version': mm.get('version'),
            'status': mm.get('status_name') or ('installed' if mm.get('status') == 2 else 'staging'),
            'origin': mm.get('origin'),
            'backend': 'rknn',
            'enabled': True,
            'imported': True,
            'input_width': mm.get('input_width', 0),
            'input_height': mm.get('input_height', 0),
            'output_count': mm.get('output_count', 0),
            'class_count': mm.get('class_count', 0),
            'class_names': mm.get('class_names') or [],
            'rknn_concurrency': _effective_rknn_concurrency(mm),
            # ★ 1.5.61：正在导入事务里的模型（staging 已建、installed 未建）不可切换，
            #   前端据此把卡片置灰并拦截点击，避免用户看到列表就点、吃到 MODEL_NOT_FOUND。
            'importing': _is_importing(mm.get('model_id') or ''),
        })
    return [_merge_model_ui_meta(m) for m in models]


def collect_web_state() -> dict:
    """合成 /api/state 的完整数据。"""
    st = _get_status()
    prof = _get_runtime_profile()
    ml = ipc_request('MODEL_LIST')
    ml_data0 = (ml.get('data', {}) or {}) if ml.get('status') == 0 else {}
    # 真源统一：registry active 覆盖 profile.model_id（防止 PUT config 用旧缓存回写跳回）
    registry_active = ml_data0.get('active', '')
    if registry_active:
        prof['model_id'] = registry_active
    ml_data = (ml.get('data', {}) or {}) if ml.get('status') == 0 else {}
    # Web 同构：state.models = 数组，字段对齐前端模型卡片（id/display_name/backend/enabled/尺寸）
    models = _models_view(ml_data)
    active_model = registry_active or prof.get('model_id', '') or ''

    m = st.get('metrics', {})
    # config 回读直接复用 profile_to_web（单一真源，避免两处翻译漂移）
    config_web = profile_to_web(prof)
    running = bool(st.get('running')) and bool(st.get('runtime_running'))
    capture_fps = float(m.get('capture_fps') or 0.0)
    input_width = int(m.get('input_width') or 0)
    input_height = int(m.get('input_height') or 0)
    # CoreRuntime 的真实指标没有 infer_total/last_frame 这两个旧字段。
    # HDMI 是否恢复只看采集 FPS 和有效输入尺寸，避免有帧时仍错误显示 degraded。
    degraded = running and (capture_fps <= 0.0 or input_width <= 0 or input_height <= 0)
    last_frame = int(m.get('last_frame') or 0)
    runtime_status = 'degraded' if degraded else ('running' if running else 'stopped')
    runtime_error = m.get('last_error') or ('HDMI 输入未锁定或尚未收到帧' if degraded else '')

    # 品牌块单次求值（M2：ui_brand 来自签名卡）。复用同一份 license 投影，
    # 避免 _license_block() 被重复调用（它内部要过一次 IPC GET_STATUS）。
    _lic = _license_block()
    _ui = _ui_block(_lic.get('ui_brand'))

    return {
        'ok': True,
        'data': {
            # 对外可见版本 = 系统部署版本（current 软链名，与 /api/update/check 的 current_version 同源）。
            # core 自报的 kCoreVersion 是「二进制编译版本」，core 未随包重编时会滞后
            # （如 1.5.26 包复用 1.5.23 的 core 二进制），直接展示会与更新检查打架。
            'app_version': _ota_current_version() or str(st.get('version', kAppVersion)) or kAppVersion,
            'version': _ota_current_version() or str(st.get('version', kAppVersion)) or kAppVersion,
            'config': config_web,
            'auto_start': _auto_start_payload(),
            # 预览流健康：前端据此在服务重启/断线后自动重建 MJPEG 连接（防卡框冻结）
            'preview': {
                'alive': (time.time() - _preview_monitor()['last_frame_ts']) < 2.5,
                'active_conns': _preview_monitor()['active_conns'],
            },
            'models': models,  # Web 同构：数组
            'selected_model_id': active_model,
            # ★ 2026-09-23：走 _preset_names()，排除 `_` 保留名（DTB 诊断报告等非预设文件）
            'presets': _preset_names(),
            'state': {
                'aim': {
                    'active': m.get('aim_active', False),
                    'active_hotkey': m.get('aim_active_hotkey', ''),
                    'active_target_track_id': int(m.get('aim_target_id', -1)),
                    'aim_profile_alternate_offset_states': [False],
                    # 热键保护的真实挂起状态（Core 侧 hotkey_guard 的 toggle 翻转结果）。
                    # 旧实现恒 False —— 用户按了挂起键，面板徽标还说"未禁用"，是撒谎。
                    'hotkeys_suspended': bool(m.get('aim_hotkeys_suspended', False)),
                    'last_error': runtime_error or ('未导入模型' if not prof.get('model_id') else ''),
                    'locked': False,
                },
                'last_error': runtime_error or ('未导入模型' if not prof.get('model_id') else ''),
                # 自动标定状态由 TTBOX Calibration Domain 维护，普通轮询只读，不触发保存提示。
                'calibration': _calibration_payload()['runtime'],
                'capture': {
                    'input_width': input_width,
                    'input_height': input_height,
                    'capture_fps': capture_fps,
                    'buffer_age_ms': m.get('buffer_age_ms', 0),
                    'last_dequeued_count': m.get('last_dequeued_count', 0),
                    'buffer_count': m.get('buffer_count', 0),
                },
                # 预览链路真实指标（PreviewModule 统计；0 = 未启动/无样本）
                'preview': {
                    'fps': m.get('preview_fps', 0),
                    'encode_ms': m.get('preview_encode_ms', 0),
                    'width': m.get('preview_width', 0),
                    'height': m.get('preview_height', 0),
                    'bytes': m.get('preview_bytes', 0),
                    'frames': m.get('preview_frames', 0),
                    'dropped': m.get('preview_dropped', 0),
                    # ★ M2.03：受限预览水印（core 投影；未授权全功能时 true）。前端可据此提示受限。
                    'watermark': bool(m.get('preview_watermark', False)),
                },
                'core': _core_state_payload(),
                'crosshair': {
                    'color_index': -1, 'component_area': 0,
                    'component_bbox': {'height': 0, 'width': 0, 'x': 0, 'y': 0},
                    'enabled': False, 'preset_color': '', 'score': 0.0,
                    'valid': False, 'x': 0.0, 'y': 0.0,
                },
                'fan_control': _fan_control_payload(),
                'detection': {
                    'detections': m.get('detect_count', 0),
                    'tracks': m.get('tracks', 0),
                    'inference_fps': m.get('fps', 0),
                    'inference_ms': m.get('infer_ms', 0),
                    'model_loaded': bool(prof.get('model_id')),
                    'frame_id': last_frame,
                    'timestamp_us': m.get('last_timestamp_us', 0),
                    'target_box': {
                        'x1': m.get('aim_target_x1', 0),
                        'y1': m.get('aim_target_y1', 0),
                        'x2': m.get('aim_target_x2', 0),
                        'y2': m.get('aim_target_y2', 0),
                        'class_id': m.get('aim_target_class_id', -1),
                        'target_id': m.get('aim_target_id', -1),
                    } if m.get('aim_has_target', False) else None,
                    'boxes': m.get('detection_boxes', []),
                },
                'latency': {
                    'capture_to_mouse_send_ms': m.get('e2e_ms', 0),
                    'preprocess_to_track_ms': (
                        # 真值 = 预处理(RGA) + 推理 + 解码。此前直接拿 e2e_ms 顶替，
                        # 会把"端到端"当成"预处理→跟踪"显示，口径不对。
                        (m.get('resize_ms', 0) or 0)
                        + (m.get('infer_run_ms', 0) or 0)
                        + (m.get('decode_ms', 0) or 0)
                    ),
                    'raw_preprocess_backend': m.get('raw_preprocess_backend', ''),
                    'raw_preprocess_error': m.get('raw_preprocess_error', ''),
                    'queue_wait_ms': m.get('buffer_age_ms', 0),
                    'rga_ms': m.get('resize_ms', 0),
                    'rknn_set_input_ms': m.get('infer_set_input_ms', 0),
                    'rknn_ms': m.get('infer_run_ms', 0),
                    'rknn_output_ms': m.get('infer_output_ms', 0),
                    'decode_ms': m.get('decode_ms', 0),
                    'e2e_ms': m.get('e2e_ms', 0),
                    'e2e_p95_ms': m.get('e2e_p95_ms', 0),
                    'e2e_p99_ms': m.get('e2e_p99_ms', 0),
                },
                'loopout': _loopout_payload(),
                # T1.15：模型输入通路诊断（人话版）。
                # 字段与 core IPC GET_STATUS.metrics.model_* 1:1 同名同义，**本层不做任何二次判定**：
                #   "是哪条路径"的唯一判据在 core（rknn/InputQuant.hpp::classify_input_pass），
                #   "为什么回落"的唯一文案也在 core（rknn/InputPathSummary.hpp::describe_input_path）。
                # 前端只负责把 pass_mode 这三个稳定串翻译成中文标签，不重算谓词。
                'model_input': {
                    'pass_mode': m.get('model_input_pass_mode', 'unknown'),
                    'fast_path_active': bool(m.get('model_fast_path_active', False)),
                    'zero_copy_ready': bool(m.get('model_zero_copy_ready', False)),
                    'tensor_type': m.get('model_input_type_name', 'unknown'),
                    'tensor_format': m.get('model_input_fmt_name', 'unknown'),
                    'quant_type': m.get('model_input_qnt_name', 'unknown'),
                    'zero_point': int(m.get('model_input_zp') or 0),
                    'scale': m.get('model_input_scale', 0.0),
                    'model_width': int(m.get('model_input_width') or 0),
                    'model_height': int(m.get('model_input_height') or 0),
                    'external_dma_requested': bool(m.get('model_external_dma_requested', False)),
                    'external_dma_bound': bool(m.get('model_external_dma_bound', False)),
                    'workers_total': int(m.get('model_workers_total') or 0),
                    'workers_zero_copy': int(m.get('model_workers_zero_copy') or 0),
                    'workers_fast_path': int(m.get('model_workers_fast_path') or 0),
                    # 注：聚合吞吐 inference_capacity_fps 只留在 core 指标与 IPC 里，
                    # 不进面板 model_input 契约（1.5.41 撤掉「推理并发」格子后无人消费，
                    # 留着会让 test_web_model_input 的键集契约变红）
                    'note': m.get('model_input_note', '') or '',
                },
                'motion_training': {
                    'collection_active': False,
                    'lease_remaining_ms': 0,
                    'model_error': '',
                    'model_loaded': False,
                    'model_quality': 0,
                    'model_status': 'disabled',
                    'profile_id': '',
                    'session_id': '',
                },
                'updated_at_ms': int(time.time() * 1000),
                'control_trace': {
                    'target_point': {'x': m.get('target_point_x', 0), 'y': m.get('target_point_y', 0)},
                    'reference': {'x': m.get('reference_x', 0), 'y': m.get('reference_y', 0)},
                    'error': {'x': m.get('aim_error_x', 0), 'y': m.get('aim_error_y', 0)},
                    # control_y = 控制域误差（平滑瞄准点 − 参考点），闭环纠偏的**输入量**。
                    # error.y 是原始目标点误差（含检测框跳变）⇒ 判闭环效果只看 control_y。
                    'control_y': m.get('aim_control_y', 0),
                    'pid_output': {'x': m.get('pid_output_x', 0), 'y': m.get('pid_output_y', 0)},
                    'scheduler_input': {'x': m.get('scheduler_input_x', 0), 'y': m.get('scheduler_input_y', 0)},
                    # 压枪速率引擎遥测（2026-09-30）：本帧下压量 / 累计下压 / 当前拉速。
                    'recoil': {
                        'add_y': m.get('recoil_add_y', 0),
                        'acc_px': m.get('recoil_acc_px', 0),
                        'rate_px_s': m.get('recoil_rate_px_s', 0),
                    },
                    'hid_move': {'x': m.get('mouse_dx', 0), 'y': m.get('mouse_dy', 0)},
                    'injection_allowed': bool(m.get('injection_allowed', False)),
                    'mouse_control_connected': bool(m.get('mouse_control_connected', False)),
                    'mouse_control_socket_write_ok': m.get('mouse_control_socket_write_ok', 0),
                    'mouse_control_socket_write_fail': m.get('mouse_control_socket_write_fail', 0),
                    'mouse_control_send_count': m.get('mouse_control_send_count', 0),
                    'last_mouse_control_dx': m.get('last_mouse_control_dx', 0),
                    'last_mouse_control_dy': m.get('last_mouse_control_dy', 0),
                    'last_mouse_control_wheel': m.get('last_mouse_control_wheel', 0),
                    'last_mouse_control_timestamp_us': m.get('last_mouse_control_timestamp_us', 0),
                },
                # 单一真相源（T1.07b）：只投影 core IPC GET_STATUS.license；
                # core 不可达 ⇒ _license_block() 诚实报未激活，绝不回退"默认激活"。
                'license': _lic,
                # ★ M2.07：云端字段增量（卡密类型/到期北京时间/解绑余量/心跳在线）——
                #   总览激活卡与系统状态页直接消费。
                'cloud': _cloud_license_subblock(),
                # 物理移动屏蔽实时状态（真实来源：RuntimeProfile mouse 配置 + 输出模式支持性）
                'mouse_output': {
                    'mode': 'full_passthrough',
                    'physical_motion_block_support': 'supported',
                    'physical_motion_block_mask': (
                        (1 if (prof.get('mouse') or {}).get('block_physical_x') else 0) |
                        (2 if (prof.get('mouse') or {}).get('block_physical_y') else 0)
                    ),
                    'physical_motion_block_error': '',
                },
                # MJPEG 流（动态预览）：img 标签原生支持 multipart/x-mixed-replace，
                # 前端 previewImage 直接消费（S1-2026-09-18：/api/preview.jpg 静态单帧端点已删除）
                'preview_path': '/api/preview.mjpg',
                'running': running and not degraded,
                'selected_model_id': prof.get('model_id', ''),
                'status': runtime_status,
            },
            # ui 块 = 品牌表投影（M2：签名卡 ui_brand → 皮肤）。此前是 8 行硬编码，
            # 与 _license_payload() 各写一份 ⇒ 必然漂移；现统一走 _ui_block()。
            'ui': _ui,
            'ui_brand': _ui['ui_brand'],
        },
    }
