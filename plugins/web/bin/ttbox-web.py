#!/usr/bin/env python3
"""ttbox_web.py — TTBOX Web 后端（Flask），兼容 Web 前端 API。

职责：
  1. 静态托管 Web 前端（web/static/）
  2. 实现 Web 全部 API 路由（100+）
  3. 通过 Unix socket 与 TTBOX Core IPC 通信
  4. 参数翻译：Web 格式 ↔ RuntimeProfile 格式
"""
from __future__ import annotations

import base64
import functools
import io
import json
import math
import os
import re
import socket
import shutil
import struct
import subprocess
import sys
import threading
import zipfile
import time
from http import HTTPStatus
from pathlib import Path
from typing import Any

from flask import Flask, Response, jsonify, redirect, render_template, request, send_file, url_for


def _ttbox_tree_root():
    """向上找「最近一层同时含 plugins/framework/scripts/deploy」的目录 = TTBOX 树根。

    与 ``lib/paths.py::discover_root`` 判据同源；刻意重复：必须先有根才能 import lib。
    """
    cur = Path(__file__).resolve().parent
    while True:
        if all((cur / _n).is_dir() for _n in ("plugins", "framework", "scripts", "deploy")):
            return cur
        if cur.parent == cur:
            raise RuntimeError("找不到 TTBOX 树根（从 %s 向上）" % __file__)
        cur = cur.parent


_TREE_ROOT = _ttbox_tree_root()
# 两处都 append（**不 insert(0)**）：根被顶到 stdlib 前会遮蔽同名标准库模块
# （2026-09-16 故障：本地 platform 包遮住 stdlib ⇒ werkzeug platform.system() 崩）。
sys.path.append(str(_TREE_ROOT))                      # framework / ttbox_motion 可导入
sys.path.append(str(_TREE_ROOT / "plugins" / "web"))  # 本插件目录（lib 包）
# 路径单点真源（A-PATH-5）+ 日志骨架（§5.4 必须有文件 sink，journald 在板端会滚动丢）。
from plugins.web.lib import paths as ttbox_paths
from plugins.web.lib import logging_setup as ttbox_logging
from plugins.web.lib import web_compress  # noqa: E402  静态资源 gzip（2026-10-03）

_LOG = ttbox_logging.get_logger()

# S1-2026-09-18（A0-3c/A0-3d）：framework_api.py（26 条死路由）与 api_v1.py（47 条死路由）
# 已随交付减法移出出货包，此处同步摘除注册点（原 install_framework_api(app) 与
# api_v1 蓝图注册块删除；/ui-custom.css 设计器常驻路由随 api_v1 一并移除，现役面板零引用）。

# 板端 /opt/ttbox/web 运行时同样加载 TTBOX 根目录领域包。
from ttbox_motion.training import MotionProfileStore, MotionSampleError, MotionTrainingError
from ttbox_motion.calibration import (
    CalibrationAxis,
    CalibrationObservation,
    CalibrationSession,
    CalibrationState,
    derive_pid_params,
    fit_axis_measurements,
)

# scripts 目录经 A-PATH-3 相对派生（<root>/scripts；开发机=仓库根、板端=release 树），
# 用 append 而非 insert(0)（防遮蔽 stdlib，见上方 platform 冲突说明）。
sys.path.append(ttbox_paths.scripts_dir())

# ====================================================================
# 配置
# ====================================================================
# 静态常量与目录派生已拆到 plugins/web/lib/settings.py（2026-10-02 web 换写法 S1）。
# 本模块只保留**两类**东西：
#   ① 打补丁锚点 —— 测试用 monkeypatch.setattr(web_mod, '<名>') 改的那些常量/函数，
#      必须定义在本模块，否则补丁落空（详见 plugins/web/lib/hub.py 顶部说明）；
#   ② 需要 ttbox_motion 领域包的对象（MOTION_STORE），避免 lib 层耦合 sys.path 引导。
from plugins.web.lib.settings import (  # noqa: E402
    IPC_SOCKET,
    LISTEN_HOST,
    LISTEN_PORT,
    MOTION_PROFILES_DIR,
    ROOT_DIR,
    STATIC_DIR,
    TTBOX_HDMIRX_EDID,
    WEB_DIR,
    _is_preset_name,
    _preset_names,
    _RESERVED_PRESET_PREFIX,
    kAppVersion,
)

# ★ 补丁锚点：TEMPLATE_DIR / PRESETS_DIR 会被测试改到临时目录（品牌换皮、预设读写用例）。
TEMPLATE_DIR = WEB_DIR / 'templates'
PRESETS_DIR = ttbox_paths.presets_dir()

MOTION_STORE = MotionProfileStore(MOTION_PROFILES_DIR)

# lib 层的动态查找点（2026-10-02 web 换写法）：被测试打补丁的名字留在本模块，
# 搬进 lib/ 的模块经 hub 在**调用时**向本模块取 —— 见 plugins/web/lib/hub.py 顶部说明。
from plugins.web.lib import hub  # noqa: E402


# ====================================================================
# 板载资源采集 —— 实现已拆到 plugins/web/lib/sysinfo.py（2026-10-02 web 换写法 S1）
# ====================================================================
_DISPLAY_CACHE = {'ts': 0.0, 'data': None}  # 显示模式探测缓存（ts + data）
from plugins.web.lib.sysinfo import (  # noqa: E402
    _storage,
    collect_network_summary,
    collect_system_stats,
)


# ====================================================================
# IPC 通信（V-13：唯一实现 = plugins/web/lib/ipc.py，本处仅薄封装）
# ====================================================================
from plugins.web.lib import ipc as _ttbox_ipc  # noqa: E402  IPC 客户端单点实现（B-CONST-1）

# ★ 配置读-改-写串行锁已单点化到 plugins/web/lib/locks.py（2026-10-02 S4）——
#   此前本文件里前后定义过两次（此处与标定区各一份），靠后一次覆盖前一次才没出错。
from plugins.web.lib.locks import _CFG_WRITE_LOCK  # noqa: E402
# ====================================================================
# 远端管理占位 —— 实现体已搬到 plugins/web/lib/remote_guard.py（S10 第一步）
#   1 个函数：_remote_not_ready
# ====================================================================
from plugins.web.lib.remote_guard import (  # noqa: E402
    _remote_not_ready,
)  # noqa: E402

# ====================================================================
# 电源动作与配置深合并 —— 实现体已搬到 plugins/web/lib/power.py（S10 第一步）
#   2 个函数：_power_action / _deep_merge_profile
# ====================================================================
from plugins.web.lib.power import (  # noqa: E402
    _deep_merge_profile,
    _power_action,
)  # noqa: E402

# ====================================================================
# 鼠标/显示器小工具 —— 实现体已搬到 plugins/web/lib/hw_mouse.py（S10 第一步）
#   3 个函数：_mouse_current_mode / _mouse_apply_payload / _display_mode_entry
# ====================================================================
from plugins.web.lib.hw_mouse import (  # noqa: E402
    _display_mode_entry,
    _mouse_apply_payload,
    _mouse_current_mode,
)  # noqa: E402

# ====================================================================
# 模型补丁响应 —— 实现体已搬到 plugins/web/lib/model_patch.py（S10 第一步）
#   1 个函数：_models_patch_response
# ====================================================================
from plugins.web.lib.model_patch import (  # noqa: E402
    _models_patch_response,
)  # noqa: E402

# ====================================================================
# 核心状态与云端授权子块 —— 实现体已搬到 plugins/web/lib/core_state.py（S10 第一步）
#   2 个函数：_core_state_payload / _cloud_license_subblock
# ====================================================================
from plugins.web.lib.core_state import (  # noqa: E402
    _cloud_license_subblock,
    _core_state_payload,
)  # noqa: E402

# ====================================================================
# 页面上下文与机器码 —— 实现体已搬到 plugins/web/lib/page_ctx.py（S10 第一步）
#   2 个函数：_page_context / _machine_code
# ====================================================================
from plugins.web.lib.page_ctx import (  # noqa: E402
    _machine_code,
    _page_context,
)  # noqa: E402

# ====================================================================
# 授权载荷投影 —— 实现体已搬到 plugins/web/lib/license_payload.py（S10 第一步）
#   1 个函数：_license_payload
# ====================================================================
from plugins.web.lib.license_payload import (  # noqa: E402
    _license_payload,
)  # noqa: E402

# ====================================================================
# 云端生命周期钩子 —— 实现体已搬到 plugins/web/lib/cloud_hooks.py（S10 第一步）
#   3 个函数：_cloud_deactivate_callback / _ensure_heartbeat_worker / _invalidate_activation_cache
# ====================================================================
from plugins.web.lib.cloud_hooks import (  # noqa: E402
    _cloud_deactivate_callback,
    _ensure_heartbeat_worker,
    _invalidate_activation_cache,
)  # noqa: E402

# ====================================================================
# 系统小工具 —— 实现体已搬到 plugins/web/lib/sysutil.py（S10 第一步）
#   2 个函数：_is_static / _retire_web_credentials
# ====================================================================
from plugins.web.lib.sysutil import (  # noqa: E402
    _is_static,
    _retire_web_credentials,
)  # noqa: E402

# ====================================================================
# 硬件 域 —— 已拆到 plugins/web/api/hardware.py（S9-b）
#   5 条 URL 一字未改；视图函数在此 re-export（测试/外部引用仍按 web_mod.<名>）。
# ====================================================================
# 装饰器实现体已搬到 lib/locks.py（装饰器在 import 期求值，不能经 hub）；此处按原名 + 别名各导一份，函数体里的 @_config_write_serialized 照旧可用。
from plugins.web.lib.locks import (  # noqa: E402
    config_write_serialized as _config_write_serialized,
)  # noqa: E402
from plugins.web.api.hardware import (  # noqa: E402
    get_display_hardware,
    get_mouse_hardware,
    update_display_hardware,
    update_mouse_hardware,
    update_mouse_proxy_mode,
)  # noqa: E402

# ====================================================================
# 模型库 域 —— 已拆到 plugins/web/api/models.py（S9-b）
#   16 条 URL 一字未改；视图函数在此 re-export（测试/外部引用仍按 web_mod.<名>）。
# ====================================================================
from plugins.web.api.models import (  # noqa: E402
    bind_model_preset,
    convert_status,
    delete_model,
    import_model,
    import_onnx,
    list_models,
    model_device_code,
    remote_connect,
    remote_delete,
    remote_import,
    remote_models,
    select_model,
    update_model_class_names,
    update_model_hailo_pipeline_depth,
    update_model_remote_frame_format,
    update_model_rknn_concurrency,
)  # noqa: E402

# ====================================================================
# 系统与主题 域 —— 已拆到 plugins/web/api/system.py（S9-b）
#   15 条 URL 一字未改；视图函数在此 re-export（测试/外部引用仍按 web_mod.<名>）。
# ====================================================================
from plugins.web.api.system import (  # noqa: E402
    expand_storage,
    get_announcement,
    get_storage_status,
    get_system_status,
    get_themes,
    install_theme,
    poweroff_system,
    reactivate_device,
    reboot_system,
    redeem_theme,
    select_theme,
    theme_asset,
    theme_preview,
    update_system_hostname,
    update_system_web_port,
)  # noqa: E402

# ====================================================================
# OTA 与更新 域 —— 已拆到 plugins/web/api/ota.py（S9-b）
#   4 条 URL 一字未改；视图函数在此 re-export（测试/外部引用仍按 web_mod.<名>）。
# ====================================================================
from plugins.web.api.ota import (  # noqa: E402
    api_ota_install,
    api_update_check,
    api_update_install,
    api_update_status,
)  # noqa: E402

# ====================================================================
# 授权 域 —— 已拆到 plugins/web/api/license.py（S9-b）
#   5 条 URL 一字未改；视图函数在此 re-export（测试/外部引用仍按 web_mod.<名>）。
# ====================================================================
from plugins.web.api.license import (  # noqa: E402
    activate_license,
    get_activation_full_recovery,
    get_license,
    reset_activation_local_identity,
    start_activation_full_recovery,
)  # noqa: E402

# ====================================================================
# 运行状态与配置 域 —— 已拆到 plugins/web/api/state.py（S9-b）
#   4 条 URL 一字未改；视图函数在此 re-export（测试/外部引用仍按 web_mod.<名>）。
# ====================================================================
# 装饰器实现体已搬到 lib/locks.py（装饰器在 import 期求值，不能经 hub）；此处按原名 + 别名各导一份，函数体里的 @_config_write_serialized 照旧可用。
from plugins.web.lib.locks import (  # noqa: E402
    config_write_serialized as _config_write_serialized,
)  # noqa: E402
from plugins.web.api.state import (  # noqa: E402
    get_config_web,
    get_events,
    get_state,
    update_config,
)  # noqa: E402

# ====================================================================
# 预设参数 域 —— 已拆到 plugins/web/api/presets.py（S9-b）
#   5 条 URL 一字未改；视图函数在此 re-export（测试/外部引用仍按 web_mod.<名>）。
# ====================================================================
# 装饰器实现体已搬到 lib/locks.py（装饰器在 import 期求值，不能经 hub）；此处按原名 + 别名各导一份，函数体里的 @_config_write_serialized 照旧可用。
from plugins.web.lib.locks import (  # noqa: E402
    config_write_serialized as _config_write_serialized,
)  # noqa: E402
from plugins.web.api.presets import (  # noqa: E402
    export_preset,
    import_preset,
    list_presets,
    load_preset,
    save_or_delete_preset,
)  # noqa: E402

# ====================================================================
# 自动标定 域 —— 已拆到 plugins/web/api/calib.py（S9-b）
#   5 条 URL 一字未改；视图函数在此 re-export（测试/外部引用仍按 web_mod.<名>）。
# ====================================================================
from plugins.web.api.calib import (  # noqa: E402
    cancel_auto_calibration,
    clear_auto_calibration,
    get_auto_calibration,
    start_auto_calibration,
    update_auto_calibration,
)  # noqa: E402


def ipc_request(req_type: str, params: dict | None = None, timeout: float = 5) -> dict:
    """向 TTBOX Core IPC 发送请求（薄封装 → lib/ipc.py::request）。

    保留本模块级函数名 ipc_request：所有调用点与测试（monkeypatch module.ipc_request）
    都依赖它。socket 取本模块 IPC_SOCKET（= lib.paths.ipc_socket()，A-PATH-5）；
    传输形态（Unix/TCP）与错误语义统一由 lib/ipc.py 维护（V-13 根治重复实现）。
    """
    return _ttbox_ipc.request(req_type, params, timeout, socket_path=IPC_SOCKET)


class CoreUnavailableError(RuntimeError):
    """Core IPC 不可达（运行期配置/状态无法读取）。

    由模块级 Flask errorhandler 统一转 503 + {core_offline:true}，使前端能明示
    「Core 离线」——这是**有意**的 fail-loud（V-01/V-02 修复）：宁可报错，也不静默
    返回空配置或磁盘旧值（否则「读失败」与「配置为空」不可区分）。
    """


# ====================================================================
# 云端凭据（web 拥有的**部署凭据文件**）—— 与「运行期配置」严格区分（C-CFG-3）
# ====================================================================
# 为什么仍有一个读文件函数（C-CFG-3 的边界）：
#   · 「运行期配置」（runtime_profile 等）唯一真源 = Core，web **禁止**读写（一律 IPC）。
#   · 「云端凭据」（cloud.app_secret / cloud.license_base_url）**不在** Core 的 config.d
#     分层内（00-factory.json 无 cloud 键）⇒ 它是 web 自己拥有、由部署/验收脚本写入的
#     凭据文件，不是第二配置真源。web 取 HMAC 签名密钥只能读它，无法经 Core IPC。
#   · 历史上此文件曾被误当「配置唯一真相」（旧 _config_file_path 的假声明）——已删除该
#     声明；它**不是** Core --config 指定的文件（Core 读 /etc/ttbox/config.d/）。
WEB_CREDENTIALS_FILE = ttbox_paths.web_credentials_file()


def _load_cloud_credentials() -> dict:
    """读取 web 云端凭据文件（仅取 cloud.* 段供签名/URL）。

    文件不存在 ⇒ 空 dict（开箱未配云端是合法状态）；文件存在但损坏 / 非法 ⇒ 上抛
    （fail-loud，绝不把"读失败"静默当"空凭据"）。
    """
    path = WEB_CREDENTIALS_FILE
    try:
        with open(path, 'r', encoding='utf-8') as f:
            cfg = json.load(f)
    except FileNotFoundError:
        return {}
    if not isinstance(cfg, dict):
        raise RuntimeError(f'云端凭据文件格式非法（非 JSON 对象）: {path}')
    return cfg


def _get_runtime_profile() -> dict:
    """获取当前 RuntimeProfile —— **唯一来源 = Core IPC（GET_CONFIG）**（C-CFG-3）。

    Web 不再直读磁盘配置文件：Core 是运行期配置的唯一真相源与唯一写入者。
    Core 不可达时**如实报错（fail-loud）**，绝不返回 {} 或磁盘内容——否则会把
    "读失败"与"配置为空"混为一谈（M2.07.1 F11 同族纪律：禁止静默回落）。
    """
    r = ipc_request('GET_CONFIG')
    if r.get('status') != 0:
        raise CoreUnavailableError(
            'Core 离线，无法读取运行期配置（' + str(r.get('error') or 'unknown') + '）')
    prof = r.get('data', {}).get('runtime_profile', {})
    if isinstance(prof, str):
        try:
            prof = json.loads(prof)
        except (TypeError, ValueError) as exc:
            raise CoreUnavailableError(f'Core 返回的 runtime_profile 非合法 JSON: {exc}')
    return prof or {}


def _get_status() -> dict:
    """获取 Core 运行状态。"""
    r = ipc_request('GET_STATUS')
    return r.get('data', {}) if r.get('status') == 0 else {}


# 秒 → ISO8601(UTC,'Z') 转换单点已拆到 plugins/web/lib/timefmt.py（2026-10-02 web 换写法 S1）。
from plugins.web.lib.timefmt import _unix_to_iso  # noqa: E402


# ====================================================================
# 品牌解析（ui_brand / 换皮 / 主题）已拆到 plugins/web/lib/branding.py
# ====================================================================
# ★ UI_BRANDS_CONFIG 是测试的 monkeypatch 锚点，必须留在本模块（见 lib/hub.py）。
UI_BRANDS_CONFIG = ROOT_DIR / 'config' / 'ui_brands.json'
from plugins.web.lib.branding import (  # noqa: E402
    DEFAULT_UI_BRAND,
    _DEFAULT_BRAND_ACCENT,
    _VALID_THEME_MODES,
    DEFAULT_UI_SKIN,
    _VALID_SKINS,
    _normalize_skin,
    _UI_BRAND_FALLBACK,
    _UI_BRANDS_CACHE,
    _ui_brands_table,
    _normalize_ui_brand,
    _normalize_hex_color,
    _normalize_brand_logo,
    _resolve_theme_fields,
    _brand_payload,
    _license_block,
    _current_ui_brand,
    _ui_block,
    _brand_template_name,
)
# 热键位映射 已拆到 plugins/web/lib/hotkeys.py（2026-10-02 web 换写法 S3）
from plugins.web.lib.hotkeys import (  # noqa: E402
    HOTKEY_BITS,
    BIT_HOTKEYS,
    _hotkey_to_bits,
    AIM_PROFILE_VALID_BITS,
    _bits_to_hotkey,
    _hotkey_guard_to_web,
)
# 控制器参数块 已拆到 plugins/web/lib/controller_params.py（2026-10-02 web 换写法 S3）
from plugins.web.lib.controller_params import (  # noqa: E402
    CONTROLLER_NUMS,
    CONTROLLER_BOOLS,
    CTRL_BLOCKS,
    CTRL_SELECTOR_FIELDS,
    _coerce_ctrl_value,
    _ctrl_read_block,
    _ctrl_write_block,
    _ctrl_read_vec,
    _ctrl_read_table,
)
# 采集几何与 FOV 已拆到 plugins/web/lib/capture_geometry.py（2026-10-02 web 换写法 S3）
from plugins.web.lib.capture_geometry import (  # noqa: E402
    CROP_SIZE_FULL_FRAME,
    CROP_SIZE_MIN_VALID,
    CROP_SIZE_MAX_VALID,
    normalize_capture_crop_size,
    normalize_profile_capture_size,
    FOV_FACTOR_MIN,
    _fov_factor_clamp,
    _fov_radius_to_factor,
)
# ====================================================================
# 参数翻译层 —— 实现已拆到 plugins/web/lib/profile_translate.py（2026-10-02 web 换写法 S5）
#   ① Web 体 → RuntimeProfile（web_body_to_profile）/ 反向（profile_to_web）
#      + aim_profiles[] 卡组校验（validate_aim_profiles）整块搬走，本处按原名 re-export。
#   ② 无补丁锚点：这三个名字测试只读模块属性、0 处 monkeypatch ⇒ 纯 import 即可。
#   ③ 段内用到的 _get_runtime_profile 是补丁锚点（29 处），lib 侧改为经 hub 调用时取。
# ====================================================================
from plugins.web.lib.profile_translate import (  # noqa: E402
    AIM_PROFILE_MAX,
    AIM_PROFILE_MIN,
    ConfigValidationError,
    _aim_profile_core_dict,
    _aim_profile_key_bits,
    _aim_profiles_to_web,
    _hotkey_mode_to_web,
    profile_to_web,
    validate_aim_profiles,
    web_body_to_profile,
)  # noqa: E402


# ====================================================================
# 状态快照 —— 实现已拆到 plugins/web/lib/state_snapshot.py（2026-10-02 web 换写法 S5）
#   面板 /api/state 的主装配（collect_web_state）+ 模型卡片视图（_models_view）。
#   ① 无补丁锚点：这两个名字测试 0 处 monkeypatch（只被调用 / 读属性）⇒ 纯 import。
#   ② 段内引用的 16 个入口函数全是补丁锚点或晚定义 ⇒ lib 侧经 hub 调用时取。
# ====================================================================
from plugins.web.lib.state_snapshot import (  # noqa: E402
    _models_view,
    collect_web_state,
)  # noqa: E402

# ====================================================================
# Flask 应用
# ====================================================================
app = Flask(
    __name__,
    template_folder=str(TEMPLATE_DIR),
    static_folder=str(STATIC_DIR),

)

# ★ 2026-10-03 性能优化：静态资源 gzip（实测传输 392.6 KB → 约 1/3）。
#   只压 /static/ 下的文本类资源；.mjpg 预览视频流显式排除（压了破坏流边界）。
#   详见 plugins/web/lib/web_compress.py 的口径说明。
web_compress.install(app)

# ★ 2026-09-23：请求体体积上限。此前**完全没有限制**——模型上传直接落盘到
#   models/_incoming，一次请求就能把磁盘写满（配合面板无鉴权，代价极低）。
#   现役模型 4~11 MB，256 MB 留了充足余量。超限由 Flask 抛 413。
MAX_UPLOAD_BYTES = 256 * 1024 * 1024
app.config['MAX_CONTENT_LENGTH'] = MAX_UPLOAD_BYTES


@app.errorhandler(413)
def _handle_payload_too_large(_exc):
    """请求体超限：给前端一个能看懂的 JSON，而不是默认的 HTML 413 页。"""
    return jsonify({'ok': False, 'error': f'上传内容过大（上限 {MAX_UPLOAD_BYTES // (1024 * 1024)} MB）'}), 413


@app.errorhandler(CoreUnavailableError)
def _handle_core_unavailable(exc: CoreUnavailableError):
    """Core IPC 不可达 ⇒ 503 + core_offline 标志（前端据此明示\"Core 离线\"）。

    V-01/V-02 修复的一部分：Web 配置/状态唯一来源 = Core IPC；Core 不可达时
    **如实报错**，绝不返回 {} 或磁盘内容（禁止静默回落）。
    """
    return jsonify({'ok': False, 'error': str(exc), 'core_offline': True}), 503


# ====================================================================
# M2.07 免密化 + 激活 gate（D1/D9/D10 裁决）
# ====================================================================
# 设计依据 m2.07-impl-spec.md §0：
#   · D1 免密全拆：首次设置 / 登录 / token 会话 / 限速 / 鉴权执法点全部删除，
#     所有 API（含 /api/ota/install、reboot、poweroff）直通；
#   · before_request 重写为两件事：LAN 黑名单 403（网络层功能，保留）+ 激活 gate；
#   · D10：web_credentials.json 机制退役（启动时改名 .retired，代码不再读它）。
# --------------------------------------------------------------------

# 凭据退役路径（D10）：启动时若存在则原地改名 .retired（best-effort）。
WEB_CREDENTIALS_PATH = '/etc/ttbox/web_credentials.json'


# ---- 局域网黑名单整块下线（D04，2026-09-18 定案 §2.5）----
# 现状是"看起来在拦、其实全放行"：名单来源脚本未进 payload ⇒ 名单恒空 ⇒ fail-open。
# 坏掉的安全功能比没有更危险（使用者以为被挡住了）⇒ Web 层判定、缓存、端点、
# scripts/lan_blocklist.sh 一并删除，不做修复替代。


# ---- 激活 gate（D9）----
# ★ 白名单常量单点定义（§7.8）：页面 302 与 API 403 共用本语义；改白名单只改这里。
#   白名单 = /api/license、/api/license/activate、/api/system（激活页状态展示）。
_ACTIVATION_WHITELIST = frozenset({
    ('GET', '/api/license'),
    ('POST', '/api/license/activate'),
    ('GET', '/api/system'),
})
_ACTIVATION_WHITELIST_PREFIXES = ()

# 页面路由（全部）：不进 API 403 面；页面自身的激活态引导在 _enforce_gate 里做。
# ★ M2.07：/setup、/login 页面随免密化下线；新增 /activate 激活页。
_PAGE_ROUTES = frozenset({'/', '/desktop', '/mobile', '/activate'})

# 需"已激活"才可访问的页面：未激活 ⇒ 302 到 /activate 激活页。
# ★ 注意 /activate 本体**不在此集合**（否则未激活访问激活页会自跳转死循环）。
_ACTIVATION_PAGES = frozenset({'/', '/desktop', '/mobile'})


# ---- 激活态判定（带 2s TTL 缓存：/api/state 轮询频率高，避免每请求都走一次 IPC）----
_ACTIVATION_CACHE = {'ts': 0.0, 'ok': False}
_ACTIVATION_TTL_S = 2.0


def _activation_ok() -> bool:
    """core 是否处于有效授权（_license_block().activated 投影，零推导）。"""
    now = time.time()
    if now - _ACTIVATION_CACHE['ts'] <= _ACTIVATION_TTL_S:
        return _ACTIVATION_CACHE['ok']
    ok = bool(_license_block().get('activated'))
    _ACTIVATION_CACHE.update({'ts': now, 'ok': ok})
    return ok


@app.before_request
def _enforce_gate():
    """★ M2.07 唯一入口执法点（D1/D9）：LAN 黑名单 403 + 激活 gate。

    判定顺序：
      静态/页面 → 页面未激活(302 /activate) → API 白名单 → API 未激活(403 activation_required)。
    鉴权语义已整体退役（D1）：所有 API 免密直通，安全边界由网络层承担。
    """
    path = request.path
    if _is_static(path):
        return None
    if path in _PAGE_ROUTES:
        # 页面路由：需激活的页面在未激活时 → 激活页（302，P0-2）；/activate 本体恒可渲染。
        if path in _ACTIVATION_PAGES and not _activation_ok():
            return redirect('/activate')
        return None
    # API 面：白名单（激活前可达）或已激活直通
    if (request.method, path) in _ACTIVATION_WHITELIST:
        return None
    for prefix in _ACTIVATION_WHITELIST_PREFIXES:
        if path.startswith(prefix):
            return None
    if not _activation_ok():
        return jsonify({'ok': False, 'error': 'activation_required'}), 403
    return None


# ====================================================================
# M2.07：云端卡密激活（web 层：CloudLicenseClient + CloudSessionStore + HeartbeatWorker）
# ====================================================================
# 链路（impl-spec §4.1）：激活页输卡密 → 本文件 /api/license/activate →
#   CloudLicenseClient.card_login（HMAC 四头签名）→ 云端 200 →
#   cloud_session.json 落盘（0600）→ IPC ACTIVATE_CLOUD 交 core（LicenseDaemon.activate_cloud
#   落盘 + Gate publish，单一真相源红线；web **否决直写** LicenseStore 文件）→ 心跳线程拉起。
# 心跳归 web 进程（D3）：60s 间隔、token 过期自动重 card-login；
#   心跳失败/超时**不锁 AI**（仅面板提示），云端 403（到期/禁用）才回调 deactivate（D4 快路径）。
# --------------------------------------------------------------------
from plugins.web.lib.cloud_client import CloudLicenseClient, CloudLicenseError
from plugins.web.lib.cloud_session import (CloudSessionStore, card_mask, device_serial,
                               parse_expire_at)
from plugins.web.lib.heartbeat_worker import HeartbeatWorker

# 云端会话态（D8）：card_key 明文落盘是自动重登前提（0600 原子写，风险已登记 §8-2）
CLOUD_SESSION_PATH = os.environ.get(
    'TTBOX_CLOUD_SESSION', ttbox_paths.config_dir() + '/cloud_session.json')
_CLOUD_SESSION = CloudSessionStore(CLOUD_SESSION_PATH)
# 配置热读：每请求重读云端凭据文件的 cloud 段（B15-8：改 URL 即时生效）
_CLOUD_CLIENT = CloudLicenseClient(_load_cloud_credentials)
# 激活并发锁：防同机并发触发两次云激活
_ACTIVATION_LOCK = threading.Lock()
_HEARTBEAT: HeartbeatWorker | None = None
_HEARTBEAT_START_LOCK = threading.Lock()

# D5：云端 card-login 无功能集字段 ⇒ web 下发全闭集（收窄执法仍在 core normalize_features，
# 闭集唯一权威 = LicenseStateMachine::known_features()）。
_CLOUD_FEATURES_FULL = ['capture', 'inference', 'aim', 'ota']


# ====================================================================
# 页面路由
# ====================================================================
# S1-2026-09-18（A0-3c/A0-3d）：原 install_framework_api(app) 与 /api/v1 蓝图注册块已删，
# framework_api.py / api_v1.py 两个死路由文件随交付减法移出出货包。

@app.after_request
def add_no_cache_headers(response):
    # HTML 页面和 API 全部禁缓存（防止浏览器缓存旧 JS/旧数据导致页面异常）
    if not request.path.startswith('/static/') or request.path.endswith('.html'):
        response.headers['Cache-Control'] = 'no-store, no-cache, must-revalidate'
        response.headers['Pragma'] = 'no-cache'
    return response

# 默认入口 = 面板（index.html），'/' 直接托管、不劫持到任何其它路径。
# --------------------------------------------------------------------
# 面板模板上下文（品牌化）：'/' 、'/desktop' 、'/mobile' 三端共用**同一构造器**。
#
# 此前三处各写一份 17 行硬编码文案（app_title/brand_mark/brand_eyebrow/brand_title/
# default_hotspot_ssid/default_local_name），改一个品牌要改三个地方 ⇒ 必然漂移。
# 现网已经漂了：服务端 brand_eyebrow='TTBOX'，而 app.js 的 brandConfig 是
# 'TTBOX SYSTEM'（页面加载瞬间 SSR 文案与 JS 接管后文案不一致，肉眼可见跳变）。
# 现在唯一来源 = _ui_block()（→ 品牌注册表 → 签名卡 ui_brand）。
# --------------------------------------------------------------------
_PAGE_MODULE_LABELS = ['首页', '配置', '模型', '预设', '运动', '校准',
                       '硬件', '网络', '系统', '更新', '主题', '激活']
_PAGE_ASSET_VERSION = '2026.09.13.1'


# ====================================================================
# 页面路由域（/ /desktop /mobile /activate）—— 已拆到 plugins/web/api/pages.py
#   （2026-10-02 web 换写法 S9-a 第 2 刀）4 条 URL 一字未改。
# ====================================================================
from plugins.web.api.pages import (  # noqa: E402
    activate_page,
    desktop,
    index,
    mobile,
)  # noqa: E402


# ====================================================================
# API 路由
# ====================================================================


# ── 根分区扩容：真实探测（取代此前的硬编码结论）────────────────────────────
# 输出契约对齐前端 storageExpandLabel()/storageExpandLog()：
#   ok / supported / expandable / reason / message / method / missing_tools /
#   action_available / action_reason / root{device,disk,label,free_after_partition}
#
# ★ 两个问题必须分开回答，不能挤进同一个布尔：
#   ① expandable       = 这台机器物理上有没有可扩空间（读分区表算出来）
#   ② action_available = 本仓有没有把"改分区表 + resize2fs"装箱成执行通道
# 旧实现把两者合成一个恒真的 expandable=True，还附赠一句凭空的
# "检测到磁盘尾部还有约 N GB 可扩容空间"（那个 N 取自 df 的可用空间，
# 与磁盘尾部有没有未分配扇区毫无关系 —— 纯编造）。
# 扩容探测已拆到 plugins/web/lib/rootfs.py（2026-10-02 web 换写法 S2）。
# ★ _run_quiet / _sysfs_int **留在本模块**：测试会 monkeypatch 这两个名字（见 lib/hub.py），
#   rootfs.py 里经 hub 在调用时取，补丁因此照旧生效。
from plugins.web.lib.rootfs import (  # noqa: E402
    _ROOTFS_PROBE_CACHE,
    _rootfs_expand_payload,
    _rootfs_expand_probe,
    _rootfs_expand_probe_uncached,
)


def _run_quiet(argv: list, timeout: float = 5.0) -> str:
    """跑一条只读探测命令：非 0 退出 / 超时 / 异常一律返回空串，不抛。"""
    try:
        out = subprocess.run(argv, capture_output=True, text=True, timeout=timeout)
    except Exception:
        return ''
    return (out.stdout or '').strip() if out.returncode == 0 else ''


def _sysfs_int(path: str) -> int:
    """读 sysfs 里的整数；读不到返回 -1 —— 用来区分"值为 0"和"读不到"。"""
    try:
        with open(path) as fh:
            return int(fh.read().strip())
    except Exception:
        return -1


# 局域网黑名单 4 个端点（GET/POST /api/system/lan-blocklist、/scan、DELETE）已随
# D04（2026-09-18 定案 §2.5）整块下线，不再保留任何壳。


# ---- 2026-09-18 更新功能定案（§三 D 组）：OTA 接线 ----
# 分发服务器地址**写死在盒子里**。
# ★ 单点纪律：本常量与 scripts/ttbox.sh 的 TTBOX_OTA_SERVER_URL 必须同值——
#   改一处必须同步改另一处；ttbox.sh doctor 会比对两处一致（单测
#   test_ota_server_url_two_places_in_sync 也在 Windows 侧钉住这条）。
#   正式服务器（2026-09-27 起）：**IP 直连** https://47.104.18.178:10086/ota。
#   2026-09-27 实锤：业主移动宽带按「域名家族+IP」过滤 cctv2.top（TLS 全端口掐
#   真域名 SNI），域名访问必须等 ICP 备案；IP 直连不发 SNI 可绕过（服务器已有
#   LE IP 证书 + nginx 10086 default_server 块）。备案通过后可切回域名。
#   地址必须带端口。值含 example.com（占位）⇒
#   /api/update/check 仍 fail-closed 503（单测会 patch 回占位值验证这条）。
OTA_SERVER_URL = 'https://47.104.18.178:10086/ota'
# 任务目录（特权通道，§2.2）：web（User=ttbox）写 JSON，root 更新器经 path 单元消费
OTA_JOBS_DIR = '/var/lib/ttbox/ota/jobs'
# 运行根下落点一律经 lib/paths.py 派生（A-PATH-4）：散写 "/opt/ttbox/…" 会让 TTBOX_PREFIX 失效。
OTA_UPDATER_PATH = ttbox_paths.prefix_path('current', 'scripts', 'ttbox_ota_updater.py')
OTA_STATUS_FILE = ttbox_paths.prefix_path('state', 'ota_status.json')
OTA_DEFAULT_KEY_ID = 'ttbox-ota-2026b'


# ====================================================================
# OTA 更新通道助手 —— 实现已拆到 plugins/web/lib/ota.py（2026-10-02 web 换写法 S6）
#   ① 四个 OTA 常量是补丁锚点（OTA_SERVER_URL ×2 / OTA_UPDATER_PATH ×3 / OTA_JOBS_DIR ×3）
#      ⇒ 一律留在本模块；lib 侧改用调用时 hub.get() 取。
#   ② _ota_install_impl 是「写任务文件给 root 更新器」的特权通道入口，逻辑原样。
# ====================================================================
from plugins.web.lib.ota import (  # noqa: E402
    _ota_current_version,
    _ota_install_impl,
    _ota_server_latest,
    _ota_ver_key,
)  # noqa: E402


def _power_action_allowed(verb: str) -> tuple:
    """问 systemd-logind：当前身份到底能不能执行 reboot / poweroff。

    Web 以非 root 的 ttbox 身份运行，systemctl reboot 成不成功取决于 polkit。
    旧实现是"起线程睡 1.5s 后 os.system(...)，同时立刻回 scheduled=True"——
    授权通不过也照样报成功，用户看到的是一句没有事实支撑的"已发送"。
    改为先向 logind 要真实答案，再决定要不要下承诺。
    """
    method = 'CanReboot' if verb == 'reboot' else 'CanPowerOff'
    out = _run_quiet(['busctl', 'call', 'org.freedesktop.login1',
                      '/org/freedesktop/login1', 'org.freedesktop.login1', method])
    for token in ('yes', 'no', 'challenge'):
        if f'"{token}"' in out:
            if token == 'yes':
                return True, ''
            if token == 'challenge':
                return False, '需要交互式授权（polkit challenge），Web 端无法代为确认'
            return False, '当前账户无权执行该电源操作（polkit 拒绝）'
    return False, f'无法查询 {method}（systemd-logind / busctl 不可用）'


# ====================================================================
# 硬件载荷簇 —— 实现已拆到 plugins/web/lib/hw_payloads.py（2026-10-02 web 换写法 S7 第二刀）
#   自启/风扇/loopout 四块载荷整段搬走，本处按原名 re-export。
#   这五个函数测试只读模块属性、0 处 monkeypatch ⇒ 纯 import 即可，lib侧亦零偏离。
# ====================================================================
from plugins.web.lib.hw_payloads import (  # noqa: E402
    _auto_start_enabled,
    _auto_start_payload,
    _fan_control_payload,
    _fan_enabled,
    _loopout_payload,
)  # noqa: E402


# -- 模型卡 UI 扩展字段持久化（game_profile/preset_name/hailo/remote 等）--
# 不进 core manifest：这些是 Web 层 UI 绑定字段，独立存 installed/<id>/ui_meta.json，
# 避免污染 ModelAdapter 校验元数据、也不需要重编 Core。
# ====================================================================
# 模型 UI meta 簇 —— 实现已拆到 plugins/web/lib/model_ui_meta.py（2026-10-02 web 换写法 S7 第三刀）
#   ui_meta.json 读写 + 字段合并 + 并发投影整块搬走，本处按原名 re-export。
#   ipc_request 是补丁锚点（10 处），lib 侧经 hub 调用时取 ⇒ 替身仍能穿透。
# ====================================================================
from plugins.web.lib.model_ui_meta import (  # noqa: E402
    _MODEL_UI_META_KEYS,
    _effective_rknn_concurrency,
    _merge_model_ui_meta,
    _model_ui_meta_path,
    _read_model_ui_meta,
    _write_model_ui_meta,
)  # noqa: E402


# ====================================================================
# 模型标签解析 + 导入预设保存 —— 同属 lib/model_convert.py（2026-10-02 web 换写法 S8 第二刀 A 段）
#   与下方转换器簇同域，故合并为一个模块；此处按原名 re-export。
#   PRESETS_DIR 是补丁锚点，lib 侧经 hub.get 调用时取。
# ====================================================================
from plugins.web.lib.model_convert import (  # noqa: E402
    _parse_model_labels_file,
    _save_model_preset_from_import,
)  # noqa: E402


# ====================================================================
# 模型转换簇 —— 实现已拆到 plugins/web/lib/model_convert.py（2026-10-02 web 换写法 S8 第二刀）
#   标签解析 + 导入预设保存 + 转换状态机 + onnx 转换 + worker 整块搬走，按原名 re-export。
#   PRESETS_DIR / ipc_request 是补丁锚点，lib 侧经 hub 调用时取 ⇒ 替身仍能穿透。
# ====================================================================
from plugins.web.lib.model_convert import (  # noqa: E402
    _CONVERTER_PYTHON,
    _CONVERTER_SCRIPT,
    _CONVERT_CALIB_DIR,
    _CONVERT_LOCK,
    _CONVERT_STATE,
    _CONVERT_WORKDIR,
    _TB,
    _conversion_worker,
    _convert_state_public,
    _parse_model_labels_file,
    _run_onnx_conversion,
    _save_model_preset_from_import,
)  # noqa: E402


# ====================================================================
# 模型导入事务锁 —— 实现已拆到 plugins/web/lib/import_lock.py（2026-10-02 web 换写法 S8 第一刀）
#   事件表 + 锁 + 4 个事务函数整块搬走，本处按原名 re-export。
#   _is_importing 是补丁锚点，且 lib/state_snapshot.py 已经 hub 转发它 ——
#   入口这份re-export 就是那个转发的目标，身份一致。
# ====================================================================
from plugins.web.lib.import_lock import (  # noqa: E402
    _MODEL_IMPORT_EVENTS,
    _MODEL_IMPORT_EVENTS_LOCK,
    _begin_import,
    _end_import,
    _is_importing,
    _wait_import_done,
)  # noqa: E402


# -- 控制/校准 --
# 自动标定（真实闭环）：目标反馈读 Core GET_STATUS.metrics（aim_pos_x/y = AimThread 选中目标中心），
# 运动注入走 mouse.calibrating 标定模式（AimThread/OutputBackend 在 calibrating 期间无视热键放行 AI 移动）。
# 标定结果写 <config_dir>/calibration.json（A-PATH-4 派生；原散写 /opt/ttbox/config/…），
# 并把 kp 换算写回 RuntimeProfile（Core 热更新）。
# ====================================================================
# 自动标定 —— 实现已拆到 plugins/web/lib/calibration.py（2026-10-02 web 换写法 S4）
# 本处只留两类东西：
#   ① 补丁锚点：_calib_target(×3) / _calib_out_counts(×3) / _calib_sample_pair(×2)
#      —— 被 monkeypatch.setattr(web_mod, '<名>') 打补丁 ⇒ 必须定义在本模块，不能搬。
#   ② 其余名字原样 re-export：入口调用点（/api/control/calibration* 四个路由）与
#      测试读模块属性（CALIB_* / _calib_apply_bias / _calib_wait_settled / ...）照旧可用。
# ====================================================================
from plugins.web.lib.calibration import (  # noqa: E402
    CALIB_AMPLITUDES,
    CALIB_AMP_MISS_LIMIT,
    CALIB_AMP_TRACK_RATIO,
    CALIB_AIM_GAIN_MAX,
    CALIB_MIN_COUNTS,
    CALIB_MIN_DELTA_PX,
    _cal,
    _cal_lock,
    _calib_apply_bias,
    _calib_apply_gain,
    _calib_apply_pid,
    _calib_derive_pid,
    _calib_set,
    _calib_thread_entry,
    _calib_wait_settled,
    _calib_worker,
    _calibration_payload,
    _clear_calibration,
    _read_active_model,
    _write_calibration,
)  # noqa: E402


def _calib_target() -> dict | None:
    """读取 Core 当前选中目标的结构化观测。

    数据来自 AimThread 的真实 TargetSelection：目标 ID、类别、中心和框尺寸。
    没有运行、没有目标或旧版 Core 未提供身份字段时，返回 None。
    """
    st = _get_status()
    m = st.get('metrics', {}) if isinstance(st, dict) else {}
    if not st.get('runtime_running') or not m.get('aim_has_target'):
        return None
    target_id = int(m.get('aim_target_id', -1))
    class_id = int(m.get('aim_target_class_id', -1))
    width = float(m.get('aim_target_width', 0.0))
    height = float(m.get('aim_target_height', 0.0))
    if target_id < 0 or class_id < 0 or width <= 0 or height <= 0:
        return None
    return {
        'x': float(m.get('aim_pos_x', 0.0)),
        'y': float(m.get('aim_pos_y', 0.0)),
        'target_id': target_id,
        'class_id': class_id,
        'width': width,
        'height': height,
        'error_x': float(m.get('aim_error_x', 0.0)),
        'error_y': float(m.get('aim_error_y', 0.0)),
        'timestamp': time.monotonic(),
    }


def _calib_out_counts() -> tuple[int, int] | None:
    """读 core 累计**请求投递**的 HID count（auto 标定的分母真源）。

    没有这个字段的旧 core 返回 None —— 调用方必须**明确失败**而不是退回用 px 当分母：
    那正是修复前的老毛病（量纲 px/px ⇒ 比值恒 ≈1.0 ⇒ 标定结果与真实手感无关）。
    """
    st = _get_status()
    m = st.get('metrics', {}) if isinstance(st, dict) else {}
    # 两条轴都必须在：只到一半说明 Core 是中间态/被改坏，此时把缺的那轴当 0
    # 会让那条轴的 gain 直接错（分母恒 0），宁可整体判"读不到"。
    if 'aim_out_counts_x' not in m or 'aim_out_counts_y' not in m:
        return None
    try:
        return int(m['aim_out_counts_x']), int(m['aim_out_counts_y'])
    except (TypeError, ValueError):
        return None


def _calib_sample_pair(axis, n: int = 3):
    """尽量"同时"采一对 (目标位移参考, 累计count 在本轴的读数)。

    Δpx 与 Δcounts 必须取自同一时间窗：闭环每秒会走若干 count，两次读数相隔太久
    会把窗口外的运动算进来（gain 直接偏）。这里交替读、取中位，把时刻偏差压到毫秒级。
    """
    pairs = []
    for _ in range(n):
        c = _calib_out_counts()
        t = _calib_target()
        if t is not None and c is not None:
            pairs.append((t['x'] if axis is CalibrationAxis.X else t['y'],
                          c[0] if axis is CalibrationAxis.X else c[1], t))
        time.sleep(0.004)
    if not pairs:
        return None
    # 按 px 排序后取中位**那一对**（px/count/目标 必须同源同时刻；
    # 分别取中位会拼出三个不同时刻的值，反而引入伪位移）
    mid = sorted(pairs, key=lambda p: p[0])[len(pairs) // 2]
    return {'px': mid[0], 'counts': mid[1], 'target': mid[2]}


# 瞄准轨迹记录（诊断）：后台线程按 50Hz 采样核心 aim 状态，存 /opt/ttbox/run/aim_trace.json
_aim_trace = {'running': False, 'samples': [], 'started_at': 0, 'stop_at': 0, 'thread': None}


# -- 硬件 --
def _mouse_save_or_ipc(prof: dict, mouse: dict) -> tuple[bool, str]:
    """写 mouse 配置 —— 唯一写入者 = Core（SET_CONFIG 热更新，C-CFG-3）。

    Core 不可达 ⇒ 如实失败（fail-loud），**不落盘**（web 不再直写配置文件，否则会
    制造与 Core persist 相冲的第三份配置）。
    """
    r = ipc_request('SET_CONFIG', {'profile': prof})
    if r.get('status') != 0:
        return False, 'Core 未运行，无法保存配置（请先启动 ttbox-core）'
    return True, ''


# ── usb-proxy gadget 身份配置通道（cmd.sock，0x4F50 协议）────────────────────
# 真源 = usbproxy/mouse_control.cpp：kSetConfigReq(14) 载荷 =
#     <B apply_now><HHHH><BBB><H><BBBB><5×<H len,bytes>>
# apply_now=1 时 usb-proxy 先落盘 gadget-config.json，再 _exit(0)，由 systemd
# Restart=always 拉起新配置 —— 这就是前端那句"Windows 会重新枚举"的实现。
#
# 旧实现把 usb_* 字段塞进 RuntimeProfile 发给 Core（Core 根本没这些成员，静默
# 丢弃），却恒返 applied=True，还附一条凭空揎造的 /etc/default/ttbox-usb-proxy
# —— 该路径全仓无人读，纯装饰。
# ====================================================================
# usbproxy 配置簇 —— 实现已拆到 plugins/web/lib/usbproxy_cfg.py（2026-10-02 web 换写法 S7）
#   USB_PROXY_* 协议常量 + _usbproxy_* 五个函数整块搬走，本处按原名 re-export。
#   _usbproxy_send_set_config / _usbproxy_unit_mode 是补丁锚点（测试各打3/4 处），
#   lib 侧经 hub 调用时取 ⇒ 入口打的替身仍能穿透。
# ====================================================================
from plugins.web.lib.usbproxy_cfg import (  # noqa: E402
    USB_PROXY_CONFIG_KEYS,
    USB_PROXY_HEX_KEYS,
    USB_PROXY_MAGIC,
    USB_PROXY_REQ_SET_CONFIG,
    USB_PROXY_RESP_ERROR,
    USB_PROXY_RESP_SET_CONFIG,
    USB_PROXY_VERSION,
    _usbproxy_config_for_form,
    _usbproxy_current_gadget_config,
    _usbproxy_effective_mode,
    _usbproxy_gadget_config_path,
    _usbproxy_send_set_config,
    _usbproxy_unit_mode,
)  # noqa: E402


# ====================================================================
# 按域注册的 Blueprint（S9）—— 注册入口，调用点在文件末尾（app 建好之后）
# ====================================================================
from plugins.web.api import register_blueprints  # noqa: E402


# ====================================================================
# 网页背景（branding background）域 —— 已拆到 plugins/web/api/brand.py
#   （2026-10-02 web 换写法 S9-a 第 1 刀）5 条路由 URL 一字未改。
#   视图函数在此 re-export：测试与可能的外部引用仍按web_mod.<名> 访问。
# ====================================================================
from plugins.web.api.brand import (  # noqa: E402
    delete_branding_background,
    get_branding_background,
    get_branding_background_image,
    update_branding_background,
    upload_branding_background,
)  # noqa: E402


# ====================================================================
# 运动训练/个人运动模型域 —— 已拆到 plugins/web/api/motion.py
#   （2026-10-02 web 换写法 S9-a 第 3 刀）13 条 URL 一字未改。
# ====================================================================
from plugins.web.api.motion import (  # noqa: E402
    activate_motion_profile,
    append_motion_training_sample,
    clear_motion_profile_samples,
    create_motion_profile,
    deactivate_motion_profile,
    delete_motion_profile,
    export_motion_profile,
    heartbeat_motion_training_session,
    list_motion_profiles,
    rename_motion_profile,
    start_motion_training_session,
    stop_motion_training_session,
    train_motion_profile,
)  # noqa: E402


# -- 预览 --
# 预览流健康监控：任何 MJPEG 连接收到帧数据即刷新 last_frame_ts。
# 前端 /api/state 轮询依据 preview.alive 判断预览流是否存活（服务重启/断线时自动重建连接）。
_PREVIEW_MONITOR = {"last_frame_ts": 0.0, "active_conns": 0}


# ====================================================================
# 入口
# ====================================================================
def main():
    # §5.4：日志骨架挂最前 —— 启动序后面任何一步出问题，现场都要能在 web.log 里看到原因。
    ttbox_logging.setup_logging()
    # ---- M2.07 启动序 ----
    # D10：web_credentials.json 机制退役（存在则改名 .retired；代码不再读它）。
    _retire_web_credentials()
    # D3：心跳归 web 进程 —— 有存量云端会话则恢复心跳对账（无会话时 worker
    #     空转轮询，激活成功后再由 _ensure_heartbeat_worker 幂等拉起）。
    _ensure_heartbeat_worker()
    from waitress import serve
    _LOG.info('TTBOX Web 后端启动: http://%s:%s (模板=%s 静态=%s IPC=%s)',
              LISTEN_HOST, LISTEN_PORT, TEMPLATE_DIR, STATIC_DIR, IPC_SOCKET)
    # waitress 默认 4 线程会被 MJPEG 长连接占满（每个预览流永久占 1 线程），导致 API 请求
    # 排队、预览流卡顿、配置保存无响应。threads 从 16 提到 64（2026-09-16 实测出现过**整个
    # 服务无响应**：进程活着、端口在听，连首页都 0 字节超时；元凶是 /designer 里嵌的整份
    # 面板 + 用户多开标签页的轮询各占线程）。64 只是抬高天花板，治本要给轮询类接口加超时。
    serve(app, host=LISTEN_HOST, port=LISTEN_PORT, threads=64)


# ---- 按域注册的 Blueprint（S9）-------------------------------------------
# 路由已从本文件搬到 plugins/web/api/<域>.py，注册入口只有这一个函数。
# URL 一字未改（各域文件里仍写完整路径，不用 url_prefix）——
# 校验：python scripts/check_route_snapshot.py（89 条逐条比对，漂移即 FAIL）。
register_blueprints(app)


# ---- 把自己登记为 lib 层的动态查找点（必须在任何 lib 代码调用 hub 之前执行）----
# ★ 2026-10-04 修正：原来写 `hub.bind(sys.modules[__name__])`，会在
#   `importlib.util.spec_from_file_location(...) + exec_module()` 加载方式下
#   直接 `KeyError`（framework/tests/test_web_plugin.py 两条用例红）。
#   根因：那条加载路径**不把模块放进 sys.modules**（只有 import 才放）。
#
# ★ 踩过的三条错路（别再改回去，每条都实测报红过）：
#   hub.bind(sys.modules[__name__]) —— 动态加载直接 KeyError。
#   hub.bind(globals()) 配 hub 用 getattr —— getattr(dict, name) 全 AttributeError，
#                            168 条用例当场报红。
#   types.ModuleType(...) 后赋 __dict__ —— `__dict__` 是只读属性，
#                                     AttributeError: readonly attribute。
#   ⇒ 正解：绑定**模块全局命名空间本身**（globals()），并让 hub 侧按 dict 存取。
#     dict 就是模块全局的确切语义，也不会被模块 __getattr__（PEP 562）劫持。
hub.bind(globals())


if __name__ == '__main__':
    main()
