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
from datetime import datetime, timezone
from http import HTTPStatus
from pathlib import Path
from typing import Any

from flask import Flask, Response, jsonify, redirect, render_template, request, send_file, url_for
# 让 TTBOX 根目录下的 framework / ttbox_motion 领域包可被加载
# 注意（2026-09-16 线上故障修复）：TTBOX 根目录必须用 append 而非 insert(0)。
# /opt/ttbox/platform/ 是带 __init__.py 的正式包，与 Python 标准库 `platform` 同名；
# 若用 insert(0) 把根目录顶到 sys.path 最前，它会遮蔽标准库，导致
#   werkzeug/serving.py:70 `platform.system()` → AttributeError: module 'platform' has no attribute 'system'
# 从而 flask 一 import 就崩，Web 服务 100% 起不来（板端实测 status=1/FAILURE）。
# 放到末尾后：stdlib 优先，platform 正常；framework / ttbox_motion 无同名冲突，照样可加载。
sys.path.append(str(Path(__file__).resolve().parents[3]))
sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
# 运行期路径单点真源（A-PATH-5 / B-CONST-2）：本插件所有路径默认值一律引用它，
# 不再散写 "/opt/ttbox/..."。跨语言同值由 docs/protocols/config-path-env-registry.md
# 登记 + scripts/ttbox_conventions_gate.sh 断言。
from lib import paths as ttbox_paths

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
ROOT_DIR = Path(os.environ.get('TTBOX_ROOT', Path(__file__).resolve().parents[1])).resolve()
WEB_DIR = ROOT_DIR
STATIC_DIR = WEB_DIR / 'static'
TEMPLATE_DIR = WEB_DIR / 'templates'
# IPC socket 唯一真源（A-PATH-5）：TTBOX_IPC_SOCKET 环境变量 > paths.py 默认。
IPC_SOCKET = ttbox_paths.ipc_socket()
# Web 控制台监听地址与端口（V-06/V-20）：**在源码里定死**，不读任何 env。
# 端口真源 = plugins/web/lib/paths.py::WEB_PORT_DEFAULT（跨语言同值，门禁断言）；
# 面板是设备唯一入口，端口若能被子系统/界面改动，现场就会出现"面板打不开/书签失效"。
LISTEN_HOST = '0.0.0.0'
LISTEN_PORT = ttbox_paths.WEB_PORT_DEFAULT

PRESETS_DIR = ttbox_paths.presets_dir()
MOTION_PROFILES_DIR = Path(ttbox_paths.motion_profiles_dir())
MOTION_STORE = MotionProfileStore(MOTION_PROFILES_DIR)

# ── 预设名枚举的**唯一真源**（2026-09-23）────────────────────────────────
# 坑：预设目录里除了用户预设，还会落**自动生成的非预设文件** —— 例如
#   scripts/ttbox_dtb_fix.sh 写的 DTB 诊断报告 `/opt/ttbox/presets/_dtbfix.json`
#   （故意放在这里，好让面板 `/api/presets/_dtbfix/export` 能读回）。
# 该文件由 root 生成、644、目录归 ttbox ⇒ **可读不可写**。
# 但此前三处枚举都是裸 `glob('*.json')`，把它当成了一个预设列进列表；
# 目录里又只有它一个 json ⇒ 前端把它当"当前预设"，切换模型后的自动保存就去写它
# ⇒ PermissionError ⇒ HTTP 500 ⇒ 面板显示"预设保存失败"。
# 约定：**以 `_` 开头的名字是保留名**（自动产物），不进预设列表、不允许写入/改名/删除，
# 只读（export）仍然可用。
_RESERVED_PRESET_PREFIX = '_'


def _is_preset_name(name: str) -> bool:
    """是否为**可写**的用户预设名（保留名 `_xxx` 一律否）。"""
    return bool(name) and not str(name).startswith(_RESERVED_PRESET_PREFIX)


def _preset_names() -> list:
    """列出用户预设名（保留名除外）。目录不存在时顺手建出来。"""
    d = Path(PRESETS_DIR)
    d.mkdir(parents=True, exist_ok=True)
    return sorted(p.stem for p in d.glob('*.json') if _is_preset_name(p.stem))

# TTBOX 自己的 EDID 工具（完全独立于板端其它 EDID 工具）
TTBOX_HDMIRX_EDID = ttbox_paths.hdmirx_edid_tool()

# ★ T1.07b：授权面**不设本地常量**（原"恒激活"伪造常量块已删，其名字亦不再出现 ——
#   否则「grep 该名 ⇒ 0」这条门禁会被自己的注释命中而失效）。
# Web 不再持有任何授权真相，只投影 core IPC `GET_STATUS.license`（单一真相源，
# 见 t1.07b-impl-spec.md §0.1 / §3；wire 字段以 t1.09-t1.10-impl-spec.md §3 为准）。

kAppVersion = '2026.08.03.1'  # 面板对外版本（B-CONST-3：与 core kCoreVersion / release 版本三名分离）


# ====================================================================
# 板载资源采集（真实 procfs/sysfs 读数，非占位）
# ====================================================================
_CPU_T0 = [0, 0.0]
_DISPLAY_CACHE = {'ts': 0.0, 'data': None}  # [样本次数, 累计 idle] —— 双采样差分算 CPU%
_CPU_TOTAL0 = [0, 0.0]


def _read_float(path, default=0.0):
    try:
        with open(path, 'r') as f:
            return float(f.read().strip())
    except Exception:
        return default


def _read_int(path, default=0):
    try:
        with open(path, 'r') as f:
            return int(f.read().strip())
    except Exception:
        return default


def _cpu_percent():
    """两次 /proc/stat 采样差分 → CPU 占用%（0~100）。"""
    try:
        with open('/proc/stat') as f:
            parts = f.readline().split()
        vals = [int(x) for x in parts[1:]]
        if len(vals) < 4:
            return 0.0
        idle = vals[3] + (vals[4] if len(vals) > 4 else 0)
        total = sum(vals)
        if _CPU_T0[0] > 0:
            didle = idle - _CPU_T0[1]
            dtotal = total - _CPU_TOTAL0[1]
            _CPU_T0[0] += 1
            _CPU_TOTAL0[0] += 1
            if dtotal > 0:
                return round(max(0.0, min(100.0, 100.0 * (1.0 - didle / dtotal))), 1)
        _CPU_T0[0] += 1
        _CPU_TOTAL0[0] += 1
        _CPU_T0[1] = idle
        _CPU_TOTAL0[1] = total
        return 0.0
    except Exception:
        return 0.0


def _memory():
    try:
        with open('/proc/meminfo') as f:
            mem = {}
            for line in f:
                k, _, v = line.partition(':')
                if k in ('MemTotal', 'MemFree', 'MemAvailable', 'Buffers', 'Cached'):
                    mem[k] = int(v.strip().split()[0]) * 1024
        total = mem.get('MemTotal', 0)
        avail = mem.get('MemAvailable', mem.get('MemFree', 0))
        used = max(0, total - avail)
        return {
            'total': total, 'used': used, 'free': avail,
            'percent': round(100.0 * used / total, 1) if total else 0.0,
        }
    except Exception:
        return {'total': 0, 'used': 0, 'free': 0, 'percent': 0.0}


def _temperature():
    """优先 SoC 温度（thermal_zone0 soc-thermal），回退第一个可用 zone。"""
    best = None
    try:
        import glob
        for z in sorted(glob.glob('/sys/class/thermal/thermal_zone*')):
            try:
                with open(z + '/type') as f:
                    ztype = f.read().strip()
            except Exception:
                continue
            temp = _read_float(z + '/temp', 0.0) / 1000.0
            if temp <= 0:
                continue
            if ztype == 'soc-thermal':
                return {'celsius': round(temp, 1), 'label': 'SoC', 'zone': ztype}
            if best is None:
                best = (temp, ztype)
    except Exception:
        pass
    if best:
        return {'celsius': round(best[0], 1), 'label': best[1], 'zone': best[1]}
    return {'celsius': 0.0, 'label': 'thermal', 'zone': ''}


def _storage():
    try:
        st = os.statvfs('/')
        total = st.f_blocks * st.f_frsize
        free = st.f_bfree * st.f_frsize
        used = total - free
        avail = st.f_bavail * st.f_frsize
        return {
            'total': total, 'used': used, 'free': avail,
            'percent': round(100.0 * used / total, 1) if total else 0.0,
        }
    except Exception:
        return {'total': 0, 'used': 0, 'free': 0, 'percent': 0.0}


def _load_average():
    try:
        with open('/proc/loadavg') as f:
            parts = f.read().split()
        return [float(x) for x in parts[:3]]
    except Exception:
        return []


def _hostname():
    try:
        import socket as _s
        return _s.gethostname()
    except Exception:
        return 'ttbox'


def _lan_ipv4():
    try:
        import subprocess as _sp
        out = _sp.check_output(['hostname', '-I'], text=True, timeout=2).split()
        for ip in out:
            if ip and not ip.startswith('127.'):
                return ip
    except Exception:
        pass
    return ''


def _uptime_seconds():
    try:
        with open('/proc/uptime') as f:
            return float(f.read().split()[0])
    except Exception:
        return 0.0


def collect_system_stats() -> dict:
    return {
        'hostname': _hostname(),
        'uptime_seconds': _uptime_seconds(),
        'cpu_percent': _cpu_percent(),
        'load_average': _load_average(),
        'memory': _memory(),
        'temperature': _temperature(),
        'storage': _storage(),
        'lan_ipv4': _lan_ipv4(),
        'lan_url': '',
        'mdns_url': '',
        'web_port': LISTEN_PORT,
        'os': 'Orange Pi 1.2.0',
        'version': '',
        'app_version': 'ttbox-0.1.0',
    }


def collect_network_summary() -> dict:
    """保持 Web 契约 hostname PUT / web-port PUT 返回的网络摘要结构。"""
    stats = collect_system_stats()
    return {
        'hostname': stats.get('hostname', ''),
        'lan_ipv4': stats.get('lan_ipv4', ''),
        'lan_url': stats.get('lan_url', ''),
        'mdns_url': stats.get('mdns_url', ''),
        'web_port': stats.get('web_port', LISTEN_PORT),
    }


# ====================================================================
# IPC 通信（V-13：唯一实现 = plugins/web/lib/ipc.py，本处仅薄封装）
# ====================================================================
from lib import ipc as _ttbox_ipc  # noqa: E402  IPC 客户端单点实现（B-CONST-1）

# ★ 配置读-改-写串行锁：Core 只保证单次 SET_CONFIG 原子，不保证跨请求的 RMW 原子。
#   waitress 64 线程 + 标定线程并发时，A 读→B 读→A 写→B 写 ⇒ A 的修改被 B 的整份
#   快照静默抹掉（"设置偶发不生效/被改回去"）。所有 GET_CONFIG→SET_CONFIG 序列必须持锁。
_CFG_WRITE_LOCK = threading.RLock()


def _config_write_serialized(fn):
    """把整个请求处理函数包进 _CFG_WRITE_LOCK（用于短平快的配置 RMW 端点）。
    长任务（标定 worker）不能整函数持锁，只包各 RMW 段。"""
    @functools.wraps(fn)
    def wrapper(*args, **kwargs):
        with _CFG_WRITE_LOCK:
            return fn(*args, **kwargs)
    wrapper.__name__ = fn.__name__
    return wrapper


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


def _unix_to_iso(sec) -> str:
    """IPC wire 的 `expires_at` / `grace_until` 单位是 unix **秒**；本函数是全仓唯一的
    秒 → ISO8601(UTC, 'Z') 转换点（T1.07b 陷阱 4：不得在别处二次换算）。

    0 / 负 / 不可解析 ⇒ `''`（语义 = "无到期"，**不是** `1970-01-01T00:00:00Z`）。
    """
    try:
        sec = int(sec)
    except (TypeError, ValueError):
        return ''
    if sec <= 0:
        return ''
    try:
        return datetime.fromtimestamp(sec, timezone.utc).isoformat().replace('+00:00', 'Z')
    except (OverflowError, OSError, ValueError):
        return ''


# ====================================================================
# 品牌（ui_brand）解析 —— 对标 yu 的 _normalize_ui_brand / _brand_payload
# ====================================================================
# 为什么需要这一层（对标 yu 的换皮机制）：
#   `ui_brand` 是**签名卡下发的渠道标识**（M2 三件套 features/plan/ui_brand 之一），
#   流向：签名卡 → Core(LicenseStateMachine) → IPC GET_STATUS.license.ui_brand
#        → 本层投影 → 前端 app.js applyBrand()。
#
# yu 的做法（app.py:362-427）：闭集 {"yu","xh","xcsh"}，未知一律回 "yu"；
#   然后 _brand_payload() 用 if 链把 token 摊成 9 个展示字段；
#   _xcsh_template_name() 再把 index/mobile 指到 templates/xcsh/ 换皮。
#
# 本层做三件事与之对齐，并各自加固：
#   ① 闭集归一：token 必须在注册表命中，否则回默认品牌。
#      —— 与 yu 同语义，但注册表**数据驱动**（config/ui_brands.json）：
#         渠道新增品牌 = 加一条 JSON，零代码改动（yu 需改 Python）。
#   ② 安全边界不放松：Core 侧 sanitize_ui_brand 已做字符集/长度硬约束
#      （拒 `<script>`、非 ASCII、超长、首字符非字母数字）。本层**只收窄不放大**：
#      Core 挡"注入"，本层挡"token 合法但无皮肤"。两层叠加。
#   ③ 模板换皮带**回退**：品牌声明 template_dir 后，若 templates/<dir>/index.html
#      不存在则静默用默认模板。yu 的 _xcsh_template_name 无此回退 ——
#      缺目录会 render_template 抛 TemplateNotFound → 500，面板直接打不开。
#      此处刻意做得更稳：渠道包只下静态皮肤、忘带模板也不会白屏。
# --------------------------------------------------------------------

UI_BRANDS_CONFIG = Path(__file__).resolve().parents[1] / 'config' / 'ui_brands.json'
DEFAULT_UI_BRAND = 'ttbox'

# ★ M2.04（schema v2）：品牌外观默认值（单一默认点）。
_DEFAULT_BRAND_ACCENT = '#2F81F7'
_VALID_THEME_MODES = ('dark', 'light')

# ★ 面板内联皮肤闭集（与参照物 html[data-ui-brand=...] 覆盖段一一对应）。
#   注册表条目可声明 "skin"；缺省/未知一律归一为默认皮肤 'yu'。皮肤**只决定视觉**
#   （documentElement.dataset.uiBrand + body.ui-brand-*），文案永远由品牌注册表投影
#   下发（payload.ui.brand_*），面板内不再硬编码任何品牌文案。
DEFAULT_UI_SKIN = 'yu'
_VALID_SKINS = ('yu', 'xh', 'xcsh')


def _normalize_skin(value: Any) -> str:
    """token → 皮肤闭集 {yu, xh, xcsh}；空/未知/非法一律回默认皮肤（对标参照物 normalizeUiBrand）。"""
    token = str(value or '').strip().lower()
    return token if token in _VALID_SKINS else DEFAULT_UI_SKIN


# 内置兜底表：注册表文件缺失/损坏时仍能起服务（绝不因品牌配置问题白屏）
_UI_BRAND_FALLBACK = {
    DEFAULT_UI_BRAND: {
        'brand_name': 'TTBOX',
        'brand_mark': 'TT',
        'brand_eyebrow': 'TTBOX SYSTEM',
        'brand_title': 'TTBOX 控制台',
        'app_title': 'TTBOX 控制台',
        'default_theme': 'dark',
        'allow_theme_switch': True,
        'default_local_name': 'ttbox',
        'default_hotspot_ssid': 'TTBOX',
        'fallback_reset_text': '重置默认 Wi-Fi',
        # ★ v2 外观字段（v1 条目缺省时由此补齐；向后兼容）
        'brand_accent': _DEFAULT_BRAND_ACCENT,
        'brand_logo': None,
        # ★ 面板皮肤（缺省 yu）；渠道条目可覆盖以套用 xh/xcsh 内联皮肤。
        'skin': DEFAULT_UI_SKIN,
        'template_dir': None,
        'static_dir': None,
    },
}

_UI_BRANDS_CACHE: dict = {'mtime': None, 'path': None, 'data': None}


def _ui_brands_table() -> dict:
    """品牌注册表，按 mtime 失效热重载。任何异常 ⇒ 内置兜底表。

    返回 {'default': str, 'brands': {id: {...}}}，保证：
      · default 一定在 brands 里；
      · brands 一定非空；
      · 每个 entry 都是 dict。
    这样下游 `_normalize_ui_brand` 永不返回"查不到的 id"。
    """
    fallback = {'default': DEFAULT_UI_BRAND, 'brands': dict(_UI_BRAND_FALLBACK)}
    path = UI_BRANDS_CONFIG
    try:
        mtime = path.stat().st_mtime
    except OSError:
        return fallback

    cache = _UI_BRANDS_CACHE
    if (cache['data'] is not None and cache['mtime'] == mtime
            and cache['path'] == str(path)):
        return cache['data']

    try:
        raw = json.loads(path.read_text(encoding='utf-8'))
        if not isinstance(raw, dict) or not isinstance(raw.get('brands'), dict):
            return fallback
        brands = {k: v for k, v in raw['brands'].items()
                  if isinstance(k, str) and k and isinstance(v, dict)}
        # 默认品牌必须存在，否则注册表不可用（宁可整体回退，不做半残选择）
        if DEFAULT_UI_BRAND not in brands:
            return fallback
        default = raw.get('default')
        if not isinstance(default, str) or default not in brands:
            default = DEFAULT_UI_BRAND
    except (OSError, json.JSONDecodeError, ValueError):
        return fallback

    table = {'default': default, 'brands': brands}
    cache.update({'mtime': mtime, 'path': str(path), 'data': table})
    return table


def _normalize_ui_brand(value: Any) -> str:
    """token → 闭集内的品牌 id；空/未知/非法一律回默认品牌（对标 yu 同函数）。"""
    table = _ui_brands_table()
    token = str(value or '').strip().lower()
    if token and token in table['brands']:
        return token
    return table['default']


def _normalize_hex_color(value: Any, fallback: str = _DEFAULT_BRAND_ACCENT) -> str:
    """★ M2.04：品牌强调色归一 —— 仅接受 #RRGGBB（大小写皆可，统一大写）；其余回默认。

    安全边界：色值只进 CSS 变量，仍严格收窄为 7 字符 hex，杜绝注入（如 `red;}`）。
    """
    if isinstance(value, str):
        v = value.strip()
        if len(v) == 7 and v[0] == '#':
            try:
                int(v[1:], 16)
                return '#' + v[1:].upper()
            except ValueError:
                return fallback
    return fallback


def _normalize_brand_logo(value: Any):
    """★ M2.04：品牌 logo 归一 —— 仅接受安全**相对**路径，否则 None（前端不显示）。

    拒绝：空 / 超长(>64) / 以 '/' 开头（绝对路径）/ 含 '..'（上跳）/ 含 URL scheme 或
    非法字符。返回值恒为 `str|None`（None = 不挂 logo，前端零兜底）。
    """
    if not isinstance(value, str):
        return None
    v = value.strip()
    if not v or len(v) > 64 or v.startswith('/') or '..' in v:
        return None
    for ch in v:
        if not (ch.isalnum() or ch in '._-/'):
            return None
    return v


def _resolve_theme_fields(entry: dict, base: dict) -> dict:
    """★ M2.04：把 v1/v2 两种写法归一到 (mode, accent, logo)。

    兼容矩阵：
      · v1 条目（无 theme / brand_accent）⇒ mode 取 default_theme，accent 取默认色。
      · v2 平铺（brand_accent / brand_logo）⇒ 分别归一。
      · v2 子对象（theme.mode / theme.accent）⇒ 优先级高于平铺。
    """
    theme_raw = entry.get('theme') if isinstance(entry.get('theme'), dict) else {}
    mode = base.get('default_theme') if base.get('default_theme') in _VALID_THEME_MODES else 'dark'
    if isinstance(theme_raw.get('mode'), str) and theme_raw['mode'].strip().lower() in _VALID_THEME_MODES:
        mode = theme_raw['mode'].strip().lower()
    accent_src = theme_raw.get('accent')
    if accent_src is None:
        accent_src = base.get('brand_accent')
    accent = _normalize_hex_color(accent_src)
    logo = _normalize_brand_logo(base.get('brand_logo'))
    return {'mode': mode, 'accent': accent, 'logo': logo}


def _brand_payload(brand: Any = None) -> dict:
    """品牌 id → 展示字段全集（对标 yu `_brand_payload`）。

    brand=None ⇒ 取当前授权卡下发的品牌。
    template_dir / static_dir 是**路径控制字段**，不对外暴露给前端
    （它们只用于服务端选模板/静态前缀，进 JSON 会被误当展示字段）。
    """
    table = _ui_brands_table()
    ui_brand = _current_ui_brand() if brand is None else _normalize_ui_brand(brand)

    entry = dict(table['brands'].get(ui_brand) or {})
    template_dir = entry.pop('template_dir', None)
    static_dir = entry.pop('static_dir', None)

    # 缺字段的渠道条目用默认品牌同名字段补齐 —— 渠道包只改 3 个字段也能跑
    base = dict(_UI_BRAND_FALLBACK[DEFAULT_UI_BRAND])
    base.update({k: v for k, v in entry.items() if v is not None})

    # ★ M2.04：v2 外观（向后兼容 v1 —— 缺字段由 default_theme/默认强调色派生）
    theme = _resolve_theme_fields(entry, base)

    payload = {
        'ui_brand': ui_brand,
        # ★ 面板内联皮肤（闭集 yu/xh/xcsh；缺省 yu）。只决定视觉，不承载文案。
        'skin': _normalize_skin(base.get('skin')),
        'brand_name': str(base.get('brand_name') or 'TTBOX'),
        'brand_mark': str(base.get('brand_mark') or 'TT'),
        'brand_eyebrow': str(base.get('brand_eyebrow') or 'TTBOX SYSTEM'),
        'brand_title': str(base.get('brand_title') or 'TTBOX 控制台'),
        'app_title': str(base.get('app_title') or 'TTBOX 控制台'),
        'default_theme': theme['mode'],
        'allow_theme_switch': bool(base.get('allow_theme_switch', True)),
        'default_local_name': str(base.get('default_local_name') or 'ttbox'),
        'default_hotspot_ssid': str(base.get('default_hotspot_ssid') or 'TTBOX'),
        'fallback_reset_text': str(base.get('fallback_reset_text') or '重置默认 Wi-Fi'),
        # ★ v2 外观字段（前端消费；v1 只认 default_theme 也不会崩 —— 只增不改）
        'brand_accent': theme['accent'],
        'brand_logo': theme['logo'],
        'theme': {'mode': theme['mode'], 'accent': theme['accent']},
    }
    payload['template_dir'] = str(template_dir) if template_dir else None
    payload['static_dir'] = str(static_dir) if static_dir else None
    return payload


def _license_block() -> dict:
    """Web `license` 子块 = core IPC `GET_STATUS.license` 的**投影**（T1.07b：单一真相源）。

    - 字段映射表写死于 `t1.07b-impl-spec.md §3`，wire 形状来自
      `t1.09-t1.10-impl-spec.md §3`（= `core/src/ipc/IpcServer.cpp` 序列化块）；
      **本层零推导**，禁止新增/改写授权语义。
    - core 不可达 / 无 license 块 ⇒ **诚实报未激活**（`activated:false` +
      `state:'unactivated'`）；**不得** fallback 到任何"默认激活"形态 ——
      M1 = AUTH=OFF + NullLicenseClient，未激活本就是**正确的**形态（A23）。
    """
    lic = _get_status().get('license', {})
    if not isinstance(lic, dict) or not lic:
        # core 不可达 ⇒ 诚实默认：无签名卡 ⇒ 无短码、无任何能力位（**绝不**回退"默认激活"）。
        return {'activated': False, 'state': 'unactivated', 'plan': 'none', 'is_pro': False,
                'features': [], 'expires_at': '', 'grace_until': '', 'message': '',
                'heartbeat_interval_s': 60, 'status': 'unactivated', 'valid': False,
                # 未激活 ⇒ 无签名卡 ⇒ 无渠道品牌 ⇒ 默认原厂皮（不是"缺字段"，是明确默认）
                'ui_brand': DEFAULT_UI_BRAND,
                # ★ M2.05：卡号短码（core 不可达 ⇒ 空串，前端不得自行拼造）。
                'short_code': '',
                # ★ M2.03：能力位（core 不可达 ⇒ 四项全 False；前端据此隐藏功能入口）。
                'capabilities': {'capture': False, 'inference': False, 'aim': False, 'ota': False}}
    state = str(lic.get('state', 'unactivated'))
    activated = bool(lic.get('activated', False))
    feats = lic.get('features') or []
    if not isinstance(feats, (list, tuple)):
        feats = []
    try:
        heartbeat = int(lic.get('heartbeat_interval_s', 60))
    except (TypeError, ValueError):
        heartbeat = 60
    # ★ M2.03：能力位 = core IPC 的 capabilities 投影（零推导）；缺失/非 dict 一律视为全 False。
    caps_raw = lic.get('capabilities')
    if not isinstance(caps_raw, dict):
        caps_raw = {}
    capabilities = {k: bool(caps_raw.get(k, False))
                    for k in ('capture', 'inference', 'aim', 'ota')}
    return {
        # ---- 授权语义：逐字段来自 IPC，零推导 ----
        'activated': activated,
        'state': state,
        'plan': str(lic.get('plan', 'none')),
        'is_pro': bool(lic.get('is_pro', False)),
        'features': list(feats),                              # 空数组合法（M1 无文档）
        'expires_at': _unix_to_iso(lic.get('expires_at', 0)),
        'grace_until': _unix_to_iso(lic.get('grace_until', 0)),
        'message': str(lic.get('last_error', '')),             # 诊断文案
        'heartbeat_interval_s': heartbeat,
        # ---- M2.05：卡号可读短码（core 派生；不可信态 = 空串）。仅投影，禁止本层再派生。----
        'short_code': str(lic.get('short_code', '') or ''),
        # ---- M2.03：能力位（capture/inference/aim/ota）。前端据此做功能可见性门控。----
        'capabilities': capabilities,
        # ---- 渠道品牌（M2 三件套之一）----
        # 例外说明：这不是"发明授权语义"，而是把 Core 下发的 token **收窄到闭集**。
        # Core 已做字符集/长度硬约束；这里再判"注册表里有没有这个皮肤"，
        # 使本字段对前端**恒可用**：拿到的要么是真实品牌 id，要么是默认品牌，
        # 永远不会是"合法但打不开"的悬空 token。见上方品牌解析段注释。
        'ui_brand': _normalize_ui_brand(lic.get('ui_brand')),
        # ---- 旧前端兼容别名（不新增语义）：status ≡ state，valid ≡ activated（两者同值）----
        'status': state,
        'valid': activated,
    }


def _current_ui_brand() -> str:
    """当前生效品牌 = license 投影的 ui_brand（单一真相源 = Core IPC，经闭集归一）。"""
    return _normalize_ui_brand(_license_block().get('ui_brand'))


def _ui_block(brand: Any = None) -> dict:
    """Web `ui` 子块 = 品牌表投影（唯一来源，杜绝 collect_web_state 与
    _license_payload 两处各写一份硬编码 —— 那正是本次要消灭的分叉）。

    brand=None ⇒ 取当前授权卡品牌；显式传入 ⇒ 解析该品牌（供 /designer 预览用）。
    可显式传 `license_block['ui_brand']`，避免重复走 IPC。
    """
    b = _brand_payload(brand)
    return {
        'app_title': b['app_title'],
        'brand_name': b['brand_name'],
        'brand_mark': b['brand_mark'],
        'brand_eyebrow': b['brand_eyebrow'],
        'brand_title': b['brand_title'],
        'ui_brand': b['ui_brand'],
        # ★ 面板内联皮肤（参照物 data-ui-brand 闭集）；与文案解耦，只负责选皮。
        'skin': b['skin'],
        'default_theme': b['default_theme'],
        'allow_theme_switch': b['allow_theme_switch'],
        'default_local_name': b['default_local_name'],
        'default_hotspot_ssid': b['default_hotspot_ssid'],
        'fallback_reset_text': b['fallback_reset_text'],
        # ★ M2.04（v2）：品牌外观字段（强调色 / logo / theme 子对象）。前端据此上色与挂 logo。
        'brand_accent': b['brand_accent'],
        'brand_logo': b['brand_logo'],
        'theme': b['theme'],
        # 静态资源前缀：渠道品牌可把皮肤放 static/<id>/（对标 yu static/xcsh/）。
        # 默认品牌 ⇒ 'static'（= 现网路径，行为零变化）。
        'static_prefix': b['static_dir'] or '',
    }


def _brand_template_name(template: str, brand: Any = None) -> str:
    """按品牌解析模板名（对标 yu `_xcsh_template_name`，但带**存在性回退**）。

    仅对 index.html / mobile.html 做换皮（与 yu 一致：activate.html 全品牌共用，
    因为激活页必须在"还没读到卡"时就能打开 —— 此时品牌尚未确定）。

    回退链：templates/<brand_dir>/<tpl> 存在 ⇒ 用它；否则用默认 templates/<tpl>。
    缺皮肤文件夹**不是错误**：渠道包只下静态资源、忘带模板时面板照常打开。
    """
    try:
        b = _brand_payload(brand)
    except Exception:
        return template
    brand_dir = b.get('template_dir')
    if not brand_dir or brand_dir == DEFAULT_UI_BRAND:
        return template
    if template not in ('index.html', 'mobile.html'):
        return template
    candidate = TEMPLATE_DIR / brand_dir / template
    try:
        if candidate.is_file():
            return f'{brand_dir}/{template}'
    except OSError:
        pass
    return template

# ====================================================================
# 参数翻译（Web 格式 ↔ RuntimeProfile 格式）
# 映射依据：Web 前端 collectConfig()（web/static/app.js:5663）+ Web daemon
# 二进制字段名实测。predict_x/y 是 Pid1Controller 的 I 通道增益（无量纲），
# rate_x/y 是 kp_gain_rate —— 全部直通。
# ★ V1.0.13（2026-09-30）：smooth_x/y 已随 core 一起删除（折叠进 kp/kd），
#   aim_offset_x/y 与 capture.offset_x/y 也一并删除（落点只留「瞄点」一个入口）。
#   老配置里若还带这些键，core 一律静默忽略。
# ====================================================================
HOTKEY_BITS = {'left': 1, 'right': 2, 'middle': 4, 'back': 8, 'forward': 16}
BIT_HOTKEYS = {v: k for k, v in HOTKEY_BITS.items()}


def _hotkey_to_bits(v, default=0):
    """Web 热键字符串（'left'/'right'/''）→ 位掩码。"""
    if isinstance(v, str):
        return HOTKEY_BITS.get(v.strip().lower(), default)
    if isinstance(v, (int, float)):
        return int(v)
    return default


# 合法的热键位掩码集合（左1 右2 中4 侧8 侧16）。
# ★ 2026-09-25：只判「hk == 0」是不够的 —— -1 / 32 / 255 都会放行，落到 core 的
#   static_cast<uint8_t> 上会绕回 / 越界（如 -1 ⇒ 255），命中判据 `buttons & 255 != 0`
#   对**任意**物理键成立 ⇒ 按什么键都瞄准（热键闸门 fail-open）。
#   组合掩码（如 3 = 左|右）core 侧能用，但面板无法回填（_bits_to_hotkey(3) 返回 ''，
#   会被 `or 'right'` 静默显示成右键，下次保存真变成 2 ⇒ 静默漂移）⇒ 面板这一层拒掉。
AIM_PROFILE_VALID_BITS = frozenset(HOTKEY_BITS.values())


def _bits_to_hotkey(v):
    """位掩码 → Web 热键字符串（0 → ''）。"""
    try:
        return BIT_HOTKEYS.get(int(v), '')
    except (TypeError, ValueError):
        return ''


def _hotkey_guard_to_web(hg) -> dict:
    """Core 的 mouse.hotkey_guard → 前端控件值。

    缺字段时给的是**与 Core 结构体一致的默认值**（enabled=False / middle），
    不是另立一套。挂起状态本身不在这里——它是运行期状态，走 state.aim.hotkeys_suspended。
    """
    hg = hg if isinstance(hg, dict) else {}
    return {
        'enabled': bool(hg.get('enabled', False)),
        'toggle_hotkey': _bits_to_hotkey(hg.get('toggle_hotkey', 4)) or 'middle',
    }


# controller 内的数值/布尔直通字段（Web key → mouse key）
CONTROLLER_NUMS = {
    'kp_x': 'kp_x', 'kp_y': 'kp_y',
    'kd_x': 'kd_x', 'kd_y': 'kd_y',
    'predict_x': 'predict_x', 'predict_y': 'predict_y',
    'rate_x': 'rate_x', 'rate_y': 'rate_y',
    'output_deadzone': 'output_deadzone',
    'selector_lost_grace_ms': 'lost_grace_ms',
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
    ('lead2', 'lead2', [
        ('enabled', 'b', False), ('gain', 'n', 0.05), ('max_offset', 'n', 25.0),
        ('decay', 'n', 0.95), ('activation_distance', 'n', 100.0), ('dead_zone', 'n', 1.0),
        ('hold_ms', 'n', 10.0), ('cooldown_ms', 'n', 250.0),
        ('y_suppress_enabled', 'b', True), ('y_suppress_min', 'n', 0.5),
        ('y_suppress_max', 'n', 2.0),
    ]),
    ('humanize', 'humanize', [
        ('enabled', 'b', False), ('smooth_factor', 'n', 0.0), ('overshoot', 'n', 0.0),
        ('brake_distance', 'n', 0.0), ('noise_sigma', 'n', 0.2), ('delay_ms', 'n', 0.0),
        ('delay_random_ms', 'n', 0.0),
    ]),
    # ★ 2026-09-29 修正：speed_fluctuation / accuracy_sim 原本以**扁平键**挂在 humanize 段里
    #   （'speed_fluctuation_enabled' …）⇒ 实际键名成了 humanize_speed_fluctuation_enabled，
    #   而面板 2026-09-28 已把这两组拆成独立卡片，控件 id 是 speed_fluctuation_enabled。
    #   两边键名对不上 ⇒ 面板上「速度波动」「精度模拟」调了存不下去（假开关）；
    #   即便存下去也落在 mouse.humanize.* 里，而 Core 读的是 mouse.speed_fluctuation.* /
    #   mouse.accuracy_sim.*（2026-09-28 已拆成独立段，RuntimeProfile.cpp:1300-1315）。
    #   现在后端也拆成独立段，core / 面板表 / 后端表 三处口径才真正一致。
    #   total_distance_px 不进表：BB 原版是几何常量（sqrt(Centre²+Centre²)），面板无控件。
    ('speed_fluctuation', 'speed_fluctuation', [
        ('enabled', 'b', False), ('start_speed', 'n', 0.80), ('accel_ratio', 'n', 0.20),
        ('decel_ratio', 'n', 0.20), ('intensity', 'n', 0.15),
    ]),
    ('accuracy_sim', 'accuracy_sim', [
        ('enabled', 'b', False), ('perfect_rate', 'n', 90.0),
        ('offset_strength', 'n', 0.50), ('direction', 'i', 0),
    ]),
    ('anti_overshoot', 'anti_overshoot', [
        ('enabled', 'b', False), ('outer_distance', 'n', 20.0), ('outer_strength', 'n', 50.0),
        ('inner_distance', 'n', 10.0), ('inner_strength', 'n', 90.0),
        ('outer_frames', 'i', 11), ('inner_frames', 'i', 6),
        ('reset_cooldown_ms', 'n', 500.0),
    ]),
    ('speed_adaptive_kp', 'speed_adaptive_kp', [
        ('enabled', 'b', False), ('move_mult', 'n', 1.5), ('static_mult', 'n', 0.8),
        ('threshold', 'n', 3.0), ('frames', 'i', 5),
    ]),
    ('global_wave', 'global_wave', [
        ('enabled', 'b', False), ('amp_x', 'n', 0.10), ('amp_y', 'n', 0.10),
        ('freq', 'n', 1.0), ('smooth', 'n', 0.50),
    ]),
    # 个人动作曲线：core 一直在用（AimThread 的 personal_gain），但面板**从来没有控件**
    #   （此前只有几行手写搬运，UI 缺）⇒ 只能手改 json。本轮补成表驱动 + 卡片。
    #   只暴露 PersonalMotion 真正读的两个字段（enabled / curve_blend）；
    #   speed_blend / reaction_blend / max_reaction_delay_ms core 从不读，不进表。
    # 贝塞尔弧线（2026-09-26 接线）：只暴露 warp 用法真正读到的字段。
    #   generation / segments 是 path1/path2 拆点列那套用的，主链走 warp_error 不读它们
    #   ⇒ 刻意不进表（不补默认值，由 Core 结构体默认兜住），免得面板摆一堆无效开关。
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
    ('bezier', 'bezier', [
        ('enabled', 'b', False), ('curvature', 'n', 0.20),
        ('linear_threshold', 'n', 45.0), ('peak_min', 'n', 0.20), ('peak_max', 'n', 0.60),
        ('dir_up', 'b', True), ('dir_down', 'b', True),
        ('dir_left', 'b', False), ('dir_right', 'b', False), ('min_move', 'n', 0.10),
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
CROP_SIZE_FULL_FRAME = 0
CROP_SIZE_MIN_VALID = 64
CROP_SIZE_MAX_VALID = 3840


def normalize_capture_crop_size(value) -> int:
    """把面板的 crop_size 归一化为 Core 合法值（0=全帧，或 64~3840）。

    非数字 / None ⇒ 0（全帧 = 不裁剪）。**绝不产生 1~63**：那是合法域之外的"夹出来"的假值，
    正是它把整份配置送进 Core 的校验拒绝分支。
    """
    try:
        v = int(round(float(value)))
    except (TypeError, ValueError):
        return CROP_SIZE_FULL_FRAME
    if v <= CROP_SIZE_FULL_FRAME:
        return CROP_SIZE_FULL_FRAME
    if v < CROP_SIZE_MIN_VALID:
        return CROP_SIZE_MIN_VALID
    if v > CROP_SIZE_MAX_VALID:
        return CROP_SIZE_MAX_VALID
    return v


def normalize_profile_capture_size(prof: dict) -> dict:
    """归一化 profile.capture.width/height（就地把非法值拉回 Core 合法域）。

    为什么在**合并之后**再归一一次：profile 可能来自三条路——面板全量提交、预设文件（可能是
    旧版 RuntimeProfile 结构）、Core 当前运行配置。任何一条路上残留 1~63 的值都会让整份
    SET_CONFIG 被 Core 拒收（连带其它本来合法的字段一起丢）。这里做最后一道闸。
    """
    if not isinstance(prof, dict):
        return prof
    cap = prof.get('capture')
    if isinstance(cap, dict):
        for key in ('width', 'height'):
            if key in cap or (key == 'width' and cap.get('width') is None):
                cap[key] = normalize_capture_crop_size(cap.get(key))
    return prof


# 瞄准半径倍率（总览「FOV 半径」/ 热键卡「热键 FOV 缩放」）的合法区间与夹取。
# 为什么必须有下限：倍率 0 会让 core 的 fov.radius 撞上「FOV 半径必须在 (0,1]」校验
# （core/src/model/RuntimeProfile.cpp:141）⇒ 整个保存失败；半径 0 本身也等于选靶全灭。
# 取 0.1 ⇒ 半径 = 0.05 × 内接圆（板端 640 截取 ⇒ 32px），仍可用且合法。
FOV_FACTOR_MIN = 0.1

# V1.0.12（2026-09-30）：ZOOM_SCALE_MIN/MAX 与 GAIN_PX_PER_COUNT_MAX 已删。
# 业主口径「不区分倍镜，靠压枪和自瞄把准星拉回目标身上」⇒ 本档不再有"倍镜真实倍率"，
# 也没有"本档 px/count"。旧配置里出现这两个键由 core 侧静默忽略。


def _fov_factor_clamp(v, default=1.0) -> float:
    """把倍率夹到 [FOV_FACTOR_MIN, 1.0]；非数值 / NaN 回退 default。"""
    try:
        f = float(v)
    except (TypeError, ValueError):
        return default
    if f != f:  # NaN
        return default
    return max(FOV_FACTOR_MIN, min(1.0, f))


def _fov_radius_to_factor(radius, enabled=True) -> float:
    """core 的 fov.radius → 面板倍率。

    core 侧 fov_range = fov.radius × 2（AimThread.cpp:117），所以倍率 = radius × 2；
    enabled=False 时 core 强制 fov_range=1.0 ⇒ 倍率就是 1.0（= 内接圆）。
    """
    if not enabled:
        return 1.0
    try:
        return _fov_factor_clamp(float(radius) * 2.0)
    except (TypeError, ValueError):
        return 1.0


# ---- 瞄准档位（多热键，2026-09-24）----
# 面板「热键与类别」页每张卡片 = 一个档位，提交体是 aim_profiles[] 数组。
# core 侧 MouseProfile.aim_profiles 是热键的**唯一真源**：老的平铺
# aim_hotkey / aim_hotkey2 / aim_hotkey_mode 已从结构体删除，只在 JSON 解析时
# 作为「数组缺失（老配置）」的合成源。所以这里必须整表遍历，不能再只取 [0]。
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
    #      core AimThread 取 fov_range = (fov.radius × 2) × aim_profiles[active].fov_scale。
    #      5e8d3a5 曾在这一处一次性乘完；多档位之后那样做会把所有档锁死在同一个半径。
    #    core 侧 fov_range = fov.enabled ? fov.radius*2 : 1.0（AimThread.cpp:117），
    #    所以这里必须写 radius = k/2 且 enabled=True。旧实现两处有问题：
    #      · `enabled = range_factor < 1.0` ⇒ 1.00 走 enabled=False（fov_range 被强制成 1.0）、
    #        0.99 走 enabled=True（fov_range=1.98）⇒ 滑块往小拖，圆反而几乎翻倍（非单调）；
    #      · 从不读 fov_scale，回填时又恒写 1.0 ⇒ 热键卡那个旋钮是死的，提示文案却在承诺"乘"。
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
        fov['radius'] = round(_fov_factor_clamp(body['range_factor']) / 2.0, 6)

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
    hz = mouse.get('humanize') or {}
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


def _core_state_payload() -> dict:
    """Core 服务真实状态（systemctl is-active ttbox-core），不硬编码。"""
    try:
        out = subprocess.check_output(['systemctl', 'is-active', 'ttbox-core'],
                                      text=True, timeout=3).strip()
    except Exception:
        out = 'inactive'
    if out == 'active':
        return {'installed': True, 'loaded': True, 'status': 'loaded',
                'message': '核心模块已加载', 'version': '2026.05.16'}
    return {'installed': True, 'loaded': False, 'status': 'not_running',
            'message': '核心模块未运行（ttbox-core 未启动）', 'version': '2026.05.16'}


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
                'alive': (time.time() - _PREVIEW_MONITOR['last_frame_ts']) < 2.5,
                'active_conns': _PREVIEW_MONITOR['active_conns'],
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

# ====================================================================
# Flask 应用
# ====================================================================
app = Flask(
    __name__,
    template_folder=str(TEMPLATE_DIR),
    static_folder=str(STATIC_DIR),

)

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


def _retire_web_credentials() -> None:
    """web_credentials.json 退役：改名 .retired（D10）。失败仅记日志，不阻断启动。"""
    try:
        if os.path.exists(WEB_CREDENTIALS_PATH):
            os.replace(WEB_CREDENTIALS_PATH, WEB_CREDENTIALS_PATH + '.retired')
            print(f'[M2.07] 已退役旧凭据文件 → {WEB_CREDENTIALS_PATH}.retired')
    except OSError as e:
        print(f'[M2.07] 凭据退役失败（忽略，不阻断启动）: {e}')


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


def _is_static(path: str) -> bool:
    """**纯静态资源**放行（§2.3：/static/*、/favicon.ico）。

    ★ 页面路由不在此函数内放行（历史上误将 _PAGE_ROUTES 并入本函数 ⇒ 未激活跳转
      /activate 的分支永不达，页面恒 200）。页面路由交由 _enforce_gate 依激活态处理。
    ★ 接管加固（fail-closed）：含"上跳段(..)"、反斜杠、或**百分号编码**的点/斜杠/
      反斜杠（%2e / %2f / %5c）的路径**一律不认**，交回入口 gate（未激活 403 /
      已激活交 Flask safe_join 兜底）。理由：`path.startswith('/static/')` 是前缀匹配，
      `/static/../api/state` 这类路径会绕过唯一执法点。收紧后变形路径恒 False。
      合法性：真实静态资源 URL 不含 `..` 段、`\\`、或编码分隔符，故不误伤
      （实测 /static/*.js|css 仍 200）。
    """
    low = path.lower()
    if ('\\' in path or '..' in path.split('/')
            or '%2e' in low or '%2f' in low or '%5c' in low):
        return False
    return (path == '/favicon.ico'
            or path.startswith('/static/'))


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


def _invalidate_activation_cache() -> None:
    """激活/失活后立即失效缓存（使 gate 与页面引导即时翻转）。"""
    _ACTIVATION_CACHE['ts'] = 0.0


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
from lib.cloud_client import CloudLicenseClient, CloudLicenseError
from lib.cloud_session import (CloudSessionStore, card_mask, device_serial,
                               parse_expire_at)
from lib.heartbeat_worker import HeartbeatWorker

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


def _machine_code() -> str:
    """machine_code 单源（§7.4）：cpu_serial（/proc/cpuinfo Serial）；
    读取失败回退 core GET_STATUS.license.bind_device；两者皆空 ⇒ ''（调用方拒绝激活）。"""
    s = device_serial()
    if s:
        return s
    try:
        return str(_get_status().get('license', {}).get('bind_device', '') or '')
    except Exception:
        return ''


def _cloud_deactivate_callback() -> None:
    """云端 403（到期/禁用）⇒ 快路径：IPC ACTIVATE_CLOUD{deactivate:true} 让 core 立即锁定。

    注意：发布到 LicenseGate 由 core 完成（本层不碰授权真相）；本回调只触发并清缓存。
    """
    ipc_request('ACTIVATE_CLOUD', {
        'deactivate': True,
        'source': 'cloud',
        'reason': '云端返回403（卡密到期或已禁用）',
    }, timeout=5)
    _invalidate_activation_cache()


def _ensure_heartbeat_worker() -> None:
    """心跳线程幂等拉起（激活成功 / web 启动时恢复会话后调用）。"""
    global _HEARTBEAT
    with _HEARTBEAT_START_LOCK:
        if _HEARTBEAT is not None and _HEARTBEAT.running():
            return
        _HEARTBEAT = HeartbeatWorker(
            _CLOUD_CLIENT, _CLOUD_SESSION,
            on_expired=_cloud_deactivate_callback,
            client_version=kAppVersion,
            machine_code=_machine_code,
        )
        _HEARTBEAT.start()


def _cloud_license_subblock() -> dict:
    """/api/license 的 `cloud` 子块（§3.3）：core 投影之上的云端字段增量。

    core 不可达 / 无会话 ⇒ honest 空（online=false、expire_at=''），不造假值。
    """
    sess = _CLOUD_SESSION.load()
    if _HEARTBEAT is not None:
        hb = _HEARTBEAT.snapshot()
    else:
        hb = {'online': False, 'last_ok_at': 0.0, 'expire_at': '',
              'interval': 60, 'timeout': 180, 'error': ''}
    return {
        'source': 'cloud' if sess else 'none',
        'card_mask': str(sess.get('card_mask', '') or ''),
        'expire_at': str(sess.get('expire_at', '') or ''),
        'max_devices': int(sess.get('max_devices', 0) or 0),
        'heartbeat': {
            'online': bool(hb.get('online')),
            'last_ok_at': float(hb.get('last_ok_at') or 0.0),
            'interval': int(hb.get('interval') or 60),
            'timeout': int(hb.get('timeout') or 180),
            'error': str(hb.get('error') or ''),
        },
        'machine_code': _machine_code(),
    }


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


def _page_context() -> dict:
    """品牌化页面上下文；render_template 用 ** 展开。"""
    ui = _ui_block()
    return {
        'app_title': ui['app_title'],
        'ui_brand': ui['ui_brand'],
        # ★ 模板插槽 data-ui-brand / body.ui-brand-* 的取值（闭集皮肤；缺省 yu）。
        'ui_skin': ui['skin'],
        'brand_mark': ui['brand_mark'],
        'brand_eyebrow': ui['brand_eyebrow'],
        'brand_title': ui['brand_title'],
        'default_theme': ui['default_theme'],
        'allow_theme_switch': ui['allow_theme_switch'],
        'default_hotspot_ssid': ui['default_hotspot_ssid'],
        'default_local_name': ui['default_local_name'],
        'asset_version': _PAGE_ASSET_VERSION,
        # color_scheme 跟随品牌默认主题：原厂 ttbox = 'dark'（与改动前逐字节一致）；
        # 渠道品牌可声明 'light'，此时 data-theme 直接落 light —— 与 yu 的
        # xh（default_theme=light + allow_theme_switch=False）行为对齐。
        'visual_theme': {'id': 'default', 'version': 'built-in',
                         'color_scheme': ui['default_theme'], 'styles': []},
        'module_labels': list(_PAGE_MODULE_LABELS),
        'motion_training_available': True,
        'motion_training_collection_available': True,
        'show_aim_trace_button': True,
        # 渠道皮肤静态前缀：非默认品牌时指向 static/<brand>/，模板据此可选加载皮肤 CSS。
        # 默认品牌 = '' ⇒ 模板走静态托管根路径（现网行为零变化）。
        'brand_static_prefix': ui.get('static_prefix') or '',
        'brand_has_custom_skin': (ui.get('static_prefix') or '') not in ('', DEFAULT_UI_BRAND),
    }


@app.get('/')
def index():
    # M2.07：免密直通；未激活由 before_request 统一 302 /activate（D9）。
    return render_template(_brand_template_name('index.html'), **_page_context())


@app.get('/desktop')
def desktop():
    # M2.07：与 '/' 同一引导（未激活 → /activate，before_request 执法）
    return render_template(_brand_template_name('index.html'),
                           mode='desktop', **_page_context())


@app.get('/mobile')
def mobile():
    # M2.07：与 '/' 同一引导（未激活 → /activate，before_request 执法）。
    # mobile 的品牌皮肤按 yu 的做法走 templates/<brand>/mobile.html；
    # 本仓无 mobile.html（三端共用 index.html + mode 参数）。
    return render_template(_brand_template_name('index.html'),
                           mode='mobile', **_page_context())


@app.get('/activate')
def activate_page():
    """激活页（M2.07 新增，1:1 复刻竞品暗色激活卡片）。已激活直接进面板。

    ★ 页面上下文：激活页**全品牌共用**（未激活时品牌未定，见 _brand_template_name），
      但 {{ brand_mark }} 仍取自 _page_context()（未激活 ⇒ _ui_block 落默认品牌），
      否则 Jinja Undefined 会把标识渲染成空白。故必须传 **_page_context()。
    """
    if _activation_ok():
        return redirect('/')
    return render_template('activate.html', **_page_context())


# ====================================================================
# API 路由
# ====================================================================

# -- 系统/状态 --
@app.get('/api/state')
def get_state():
    return jsonify(collect_web_state())


@app.get('/api/announcement')
def get_announcement():
    # 保持 Web 契约：无公告源可达时返回 503 + error 结构。
    # ★ T1.07b：旧文案把"公告不可达"错说成"授权服务器失败" —— 既是旧授权残留，
    #   又与"授权真相只在 core"冲突（此处与授权无关，不得暗示授权服务器存在）。
    return jsonify({'ok': False, 'error': 'announcement source unavailable'}), 503


@app.get('/api/system')
def get_system_status():
    return jsonify({'ok': True, 'data': collect_system_stats()})



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
ROOTFS_TAIL_MIN_BYTES = 64 * 1024 * 1024
ROOTFS_EXPAND_FS = ('ext2', 'ext3', 'ext4')
ROOTFS_EXPAND_TOOLS = ('growpart', 'resize2fs')
_ROOTFS_PROBE_CACHE: dict = {'ts': 0.0, 'data': None}
_ROOTFS_PROBE_TTL_SEC = 5.0


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


def _human_bytes(n: int) -> str:
    if n < 0:
        return '未知'
    value = float(n)
    for unit in ('B', 'KB', 'MB', 'GB'):
        if value < 1024 or unit == 'GB':
            return f'{value:.1f} {unit}'
        value /= 1024
    return f'{value:.1f} GB'


def _rootfs_expand_probe_uncached() -> dict:
    """探测根分区扩容可行性 —— 数据全部来自 findmnt / lsblk / sysfs，无预置结论。"""
    probe = {
        'ok': True,
        'supported': True,
        'expandable': False,
        'reason': '',
        'message': '',
        'method': 'growpart',
        'missing_tools': [],
        # 执行通道：本仓尚未把"改分区表 + resize2fs"装箱（没有特权 worker）。
        # 探测到可扩空间时 expandable 为真、按钮依然不可用，由前端读这个字段。
        'action_available': False,
        'action_reason': 'expand_worker_not_implemented',
        'root': {},
    }
    src = _run_quiet(['findmnt', '-no', 'SOURCE', '/'])
    fstype = _run_quiet(['findmnt', '-no', 'FSTYPE', '/'])
    root = {'device': src, 'label': '根分区', 'disk': '', 'fstype': fstype}
    probe['root'] = root

    if not src.startswith('/dev/'):
        probe.update(supported=False, reason='unsupported_root',
                     message=f'根文件系统不是块设备分区（{src or "未知"}），不支持在线扩容')
        return probe

    part = os.path.basename(src)
    disk_lines = _run_quiet(['lsblk', '-no', 'PKNAME', src]).splitlines()
    disk = disk_lines[0].strip() if disk_lines else ''
    if disk:
        root['disk'] = '/dev/' + disk

    p_start = _sysfs_int(f'/sys/class/block/{part}/start')
    p_size = _sysfs_int(f'/sys/class/block/{part}/size')
    d_size = _sysfs_int(f'/sys/class/block/{disk}/size') if disk else -1
    tail = -1
    if p_start >= 0 and p_size >= 0 and d_size >= 0:
        tail = (d_size - (p_start + p_size)) * 512
        root['free_after_partition'] = tail

    missing = [t for t in ROOTFS_EXPAND_TOOLS if shutil.which(t) is None]
    probe['missing_tools'] = missing

    if fstype and fstype not in ROOTFS_EXPAND_FS:
        probe.update(reason='unsupported_filesystem',
                     message=f'根分区文件系统是 {fstype}，本仓只支持 ext4 在线扩容')
        return probe
    if missing:
        probe.update(reason='missing_tools',
                     message='缺少扩容工具：' + '、'.join(missing))
        return probe
    if tail < 0:
        probe.update(ok=False, reason='probe_failed',
                     message=f'读不到 {root["disk"] or part} 的分区表，扩容可行性未知')
        return probe
    if tail < ROOTFS_TAIL_MIN_BYTES:
        probe.update(reason='no_tail_space',
                     message=(f'磁盘尾部只剩 {_human_bytes(tail)}，'
                              f'不足 {_human_bytes(ROOTFS_TAIL_MIN_BYTES)}，无法扩容'))
        return probe

    probe.update(expandable=True, reason='ok',
                 message=f'磁盘尾部有 {_human_bytes(tail)} 未分配空间，可扩容')
    return probe


def _rootfs_expand_probe(force: bool = False) -> dict:
    """带 5 秒缓存的探测（findmnt/lsblk 各一次，别被轮询打成热点）。"""
    now = time.time()
    cached = _ROOTFS_PROBE_CACHE['data']
    if not force and cached is not None and now - _ROOTFS_PROBE_CACHE['ts'] < _ROOTFS_PROBE_TTL_SEC:
        return dict(cached)
    probe = _rootfs_expand_probe_uncached()
    _ROOTFS_PROBE_CACHE['ts'] = now
    _ROOTFS_PROBE_CACHE['data'] = dict(probe)
    return probe


def _rootfs_expand_payload(action: str, force: bool = False) -> dict:
    """探测结果 + df 容量，拼成一份完整的 rootfs 契约对象。"""
    s = _storage()
    payload = dict(_rootfs_expand_probe(force=force))
    payload.update({
        'action': action,
        'can_expand': payload['expandable'],
        'percent': s['percent'],
        'total': s['total'],
        'used': s['used'],
        'free': s['free'],
    })
    return payload


@app.get('/api/system/storage')
def get_storage_status():
    s = _storage()
    force = str(request.args.get('force') or '') not in ('', '0', 'false')
    return jsonify({
        'ok': True,
        'data': {
            'free': s['free'],
            'path': '/opt/ttbox',
            'percent': s['percent'],
            'root_free': s['free'],
            'root_percent': s['percent'],
            'root_total': s['total'],
            'root_used': s['used'],
            'rootfs': _rootfs_expand_payload('status', force=force),
        },
    })


@app.post('/api/system/storage/expand')
def expand_storage():
    payload = _rootfs_expand_payload('expand', force=True)
    if not payload['expandable']:
        # 409：请求合法，但当前机器没有可扩空间（或工具/文件系统不支持）。
        # 真实原因走 error.message + data.rootfs，前端原样展示。
        return jsonify({'ok': False, 'error': payload['message'],
                        'data': {'rootfs': payload}}), 409
    if not payload['action_available']:
        # 501：物理上可扩，但本仓没有改分区表的执行通道 —— 就说没实现。
        # 旧行为是回一句"扩容需重启进恢复流程"，把"没做"说成"做了一半"。
        return jsonify({'ok': False,
                        'error': '检测到可扩空间，但扩容执行通道尚未装箱（Web 不会修改分区表）',
                        'data': {'rootfs': payload}}), 501
    # fail-closed：action_available 若在未接线的情况下为真，宁可报错也不假装扩容成功。
    return jsonify({'ok': False, 'error': '扩容执行通道状态异常',
                    'data': {'rootfs': payload}}), 500


@app.put('/api/system/hostname')
def update_system_hostname():
    body = request.get_json(silent=True) or {}
    hostname = str(body.get('hostname', '')).strip()
    if not hostname or len(hostname) > 63:
        return jsonify({'ok': False, 'error': '主机名无效'})
    r = subprocess.run(['hostnamectl', 'set-hostname', hostname], capture_output=True, text=True, timeout=10)
    if r.returncode != 0:
        return jsonify({'ok': False, 'error': r.stderr or '设置失败'})
    # 保持 Web 契约：返回 hostname/lan_ipv4/lan_url/mdns_url/web_port
    return jsonify({'ok': True, 'data': collect_network_summary()})


@app.put('/api/system/web-port')
def update_system_web_port():
    # Web 控制台端口已在源码中固定（见文件头 LISTEN_PORT），不再支持运行时修改。
    # 保留这个接口是为了让界面上的"修改端口"拿到一句明确的人话，而不是改完没反应。
    #
    # ★ 2026-09-23（全仓审查复核 #38）：这里原先在这行 return **之后**还留着 30 行
    #   「写 systemd drop-in + daemon-reload + 延时重启」的实现 —— 永不可达。
    #   危害不是"多跑了一段"（它压根不跑），而是**看着像活的实现**：后来人会照着它
    #   推理"端口本来是可改的"，或者以为删掉下面这行 return 就能启用。已整块删除。
    #   真要恢复运行时改端口，得把 LISTEN_PORT 的固定一并重新设计，不是撤掉这行 return 的事。
    return jsonify({'ok': False,
                    'error': 'Web 控制台端口已固定为 8000，不支持在线修改'}), 400


# 局域网黑名单 4 个端点（GET/POST /api/system/lan-blocklist、/scan、DELETE）已随
# D04（2026-09-18 定案 §2.5）整块下线，不再保留任何壳。


@app.post('/api/system/reactivate')
def reactivate_device():
    # ★ T1.07b：旧形**焊死**"TTBOX 本地授权恒激活，无需修复" —— 未激活也答"正常"（假绿）。
    #   改为读真状态（core IPC 投影）：已激活才答"正常"；未激活必须如实报，不谎称正常。
    lic = _license_block()
    if lic.get('activated'):
        return jsonify({'ok': False, 'error': '当前授权状态正常，无需修复授权'}), 400
    # 未激活：不谎报"正常"；真实在线修复需服务器，下沉 T2.x（本任务不做）。
    return jsonify({'ok': False,
                    'error': f"授权未激活（state={lic.get('state')}）；在线修复下沉 T2.x"}), 409


@app.post('/api/ota/install')
def api_ota_install():
    """OTA-01：调度入口；实现与 /api/update/install 共用 _ota_install_impl（单一实现点）。"""
    return _ota_install_impl()


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
OTA_UPDATER_PATH = '/opt/ttbox/current/scripts/ttbox_ota_updater.py'
OTA_STATUS_FILE = '/opt/ttbox/state/ota_status.json'
OTA_DEFAULT_KEY_ID = 'ttbox-ota-2026b'


def _ota_current_version() -> str:
    """当前版本：current 软链目录名优先，IPC GET_STATUS.version 兜底（web 侧单点）。"""
    try:
        name = os.path.basename(os.path.realpath('/opt/ttbox/current').rstrip('/'))
        if name and name != 'current':
            return name
    except Exception:
        pass
    try:
        return str((_get_status() or {}).get('version') or '')
    except Exception:
        return ''


def _ota_ver_key(v):
    """与 scripts/ttbox_ota_updater.py::_version_key 同源（数字段按值、其余按字典序）。"""
    import re as _re
    parts = []
    for seg in _re.split(r'[.\-_+]', str(v or '')):
        if not seg:
            continue
        if seg.isdigit():
            parts.append((0, int(seg), ''))
        else:
            parts.append((1, 0, seg))
    return tuple(parts)


def _ota_server_latest() -> dict:
    """向写死的分发服务器发**一次查询**（O11 契约，docs/protocols/ota-server-contract.md）。

    返回 {'latest_version':…, 'package_url':…, 'sign_url':…}；任何异常上抛由调用方转错。
    """
    import urllib.request as _ur
    url = OTA_SERVER_URL.rstrip('/') + '/latest?current=' + _ota_current_version()
    with _ur.urlopen(url, timeout=6) as r:
        doc = json.loads(r.read().decode('utf-8', 'replace'))
    if not isinstance(doc, dict) or not doc.get('latest_version'):
        raise ValueError('服务器响应缺 latest_version')
    return doc


def _ota_install_impl():
    """OTA 安装实现（/api/ota/install 与 /api/update/install 共用，单一实现点）。

    2026-09-18 定案 §2.2/D03：web（User=ttbox）无 sudoers/polkit，不再用 systemd-run；
    改为**写任务文件**到 OTA_JOBS_DIR（root:ttbox 0770），由 ttbox-ota.path 监听并拉起
    root 更新器。前置校验失败**返错**，绝不返回 ok:true（消灭 D03 假成功）。
    """
    if not _license_block().get('capabilities', {}).get('ota'):
        return jsonify({'ok': False, 'error': "feature 'ota' not licensed"}), 403
    body = request.get_json(silent=True) or {}
    url = str(body.get('url') or '').strip()
    key_id = str(body.get('key_id') or OTA_DEFAULT_KEY_ID).strip()
    # ★ key_id/version 会进 root 更新器的路径拼装（<keys_dir>/<key_id>.pub、
    #   releases/<ver>.ota.staging），与更新器 _check_safe_id 同一白名单，
    #   源头就拒掉路径穿越（"../../tmp/evil" 可让 root 验签加载攻击者公钥）。
    _OTA_ID_RE = re.compile(r'^[A-Za-z0-9][A-Za-z0-9._-]*$')
    if not _OTA_ID_RE.match(key_id) or '..' in key_id:
        return jsonify({'ok': False, 'error': '非法 key_id'}), 400
    if not url:
        return jsonify({'ok': False, 'error': 'url is required'}), 400
    if not url.startswith('https://'):
        return jsonify({'ok': False, 'error': '仅接受 https 更新源'}), 400
    # 前置校验（D03）：更新器在、任务目录在且可写 —— 任何一条不满足都是环境故障，
    # 必须当场报错而不是假装调度成功（旧版假成功 = 装了也白装）。
    if not os.path.isfile(OTA_UPDATER_PATH):
        return jsonify({'ok': False, 'error': f'更新器缺失: {OTA_UPDATER_PATH}'}), 500
    if not os.path.isdir(OTA_JOBS_DIR):
        return jsonify({'ok': False, 'error': f'任务目录缺失: {OTA_JOBS_DIR}'}), 500
    try:
        probe = os.path.join(OTA_JOBS_DIR, '.probe-%d' % int(time.time()))
        with open(probe, 'w', encoding='utf-8') as f:
            f.write('{}')
        os.remove(probe)
    except Exception as e:
        return jsonify({'ok': False, 'error': f'任务目录不可写: {e!r}'}), 500
    job = {'url': url, 'key_id': key_id, 'enqueued_at': int(time.time())}
    if str(body.get('version') or '').strip():
        job['version'] = str(body['version']).strip()
        if not _OTA_ID_RE.match(job['version']) or '..' in job['version']:
            return jsonify({'ok': False, 'error': '非法 version'}), 400
    name = 'job-%d.json' % int(time.time() * 1000)
    try:
        with open(os.path.join(OTA_JOBS_DIR, name), 'w', encoding='utf-8') as f:
            json.dump(job, f, ensure_ascii=False, sort_keys=True)
    except Exception as e:
        return jsonify({'ok': False, 'error': f'写任务文件失败: {e!r}'}), 500
    return jsonify({'ok': True, 'data': {'scheduled': name, 'message': '更新任务已受理'}})


@app.post('/api/update/install')
def api_update_install():
    """YU 前端兼容：POST /api/update/install → 转发 OTA 调度（含 capabilities.ota 门控）。"""
    return _ota_install_impl()


@app.get('/api/update/status')
def api_update_status():
    """读更新器的 ota_status.json（2026-09-18 定案 D-14）。

    更新器在 run() 开始时写 RUNNING、结束写 SUCCESS/FAILED；本端点投影成前端
    轮询期望的形状。文件不存在 = 从未跑过更新 ⇒ idle（无假壳）。
    """
    try:
        doc = json.loads(open(OTA_STATUS_FILE, encoding='utf-8').read())
    except Exception:
        doc = {}
    state = str(doc.get('state') or '').upper()
    if state == 'RUNNING':
        # 2026-09-19 修复「卡50」：进度由更新器分阶段写入，这里只做投影。
        # 另加超时保护：状态文件 30 分钟没动过 ⇒ 更新器大概率已死，报失败而不是永远转圈。
        try:
            progress = max(1, min(99, int(doc.get('progress'))))
        except (TypeError, ValueError):
            progress = 10
        try:
            stale = (time.time() - os.path.getmtime(OTA_STATUS_FILE)) > 1800
        except OSError:
            stale = False
        if stale:
            return jsonify({'ok': True, 'data': {'status': 'failed', 'progress': 100,
                                                 'error': '更新进程中断（状态超过30分钟无进展）'}})
        return jsonify({'ok': True, 'data': {'status': 'running', 'progress': progress,
                                             'message': str(doc.get('phase') or '更新进行中'),
                                             'version': doc.get('version') or ''}})
    if state == 'SUCCESS':
        # 2026-09-20 修复「开页面就自动刷新」：状态文件装完后永久停在 SUCCESS，
        # 前端无法区分"刚装完"和"几十小时前的旧结果"。补 finished_at（文件 mtime）
        # 让前端只在"刚刚完成"时才提示刷新，历史残留忽略。
        try:
            finished_at = int(os.path.getmtime(OTA_STATUS_FILE))
        except OSError:
            finished_at = 0
        return jsonify({'ok': True, 'data': {'status': 'success', 'progress': 100,
                                             'version': doc.get('version') or '',
                                             'finished_at': finished_at}})
    if state == 'FAILED':
        try:
            finished_at = int(os.path.getmtime(OTA_STATUS_FILE))
        except OSError:
            finished_at = 0
        return jsonify({'ok': True, 'data': {'status': 'failed', 'progress': 100,
                                             'error': str(doc.get('detail')
                                                          or doc.get('error') or '更新失败'),
                                             'finished_at': finished_at}})
    return jsonify({'ok': True, 'data': {'status': 'idle'}})


@app.post('/api/update/check')
def api_update_check():
    """检查更新（2026-09-18 定案 §2.1/§三 H-25）：向写死的服务器查一次，比对版本。

    返回 data：{update_available, current_version, latest_version,
                package_url, sign_url, key_id}。前端拿 package_url 直接安装；
    sign_url 必须等于 package_url + '.sign.json'（更新器旁车规则，契约已钉死）。
    服务器地址含 example.com（占位值）⇒ fail-closed 503，不给假结果。
    """
    if 'example.com' in OTA_SERVER_URL:
        return jsonify({'ok': False, 'error': 'ota_server_not_configured',
                        'detail': '分发服务器地址尚未配置（OTA_SERVER_URL 仍为占位值）'}), 503
    if not OTA_SERVER_URL.startswith('https://'):
        return jsonify({'ok': False, 'error': 'ota_server_not_configured',
                        'detail': 'OTA_SERVER_URL 必须是 https'}), 503
    try:
        latest = _ota_server_latest()
    except Exception as e:
        return jsonify({'ok': False, 'error': 'ota_server_unreachable',
                        'detail': repr(e)}), 502
    cur = _ota_current_version()
    ver = str(latest.get('latest_version') or '').strip()
    pkg = str(latest.get('package_url') or '').strip()
    sig = str(latest.get('sign_url') or (pkg + '.sign.json' if pkg else '')).strip()
    update_available = bool(ver and pkg and _ota_ver_key(ver) > _ota_ver_key(cur)) if cur else bool(ver and pkg)
    # 增量包探测（2026-09-19）：服务器若存了 ttbox-update-<ver>-delta-from-<cur>.tgz，
    # 就把 package_url 换成增量地址（旁车 sign.json 存在才算数）。更新器侧仍 fail-closed：
    # 基线不匹配会整包拒绝，绝不带病浇筑。全量地址保留在 full_package_url 便于排查。
    full_pkg, full_sig = pkg, sig
    if update_available and pkg.startswith('https://') and cur:
        try:
            import urllib.request as _ur2
            name = pkg.rsplit('/', 1)[-1]
            if name.startswith('ttbox-update-') and name.endswith('.tgz'):
                d_url = pkg.rsplit('/', 1)[0] + '/' + name[:-4] + '-delta-from-%s.tgz' % cur
                with _ur2.urlopen(d_url + '.sign.json', timeout=6) as r:
                    d_sign = json.loads(r.read().decode('utf-8', 'replace'))
                if isinstance(d_sign, dict) and d_sign.get('sha256'):
                    pkg, sig = d_url, d_url + '.sign.json'
        except Exception:
            pass  # 探测不到增量 ⇒ 用全量，正常路径
    return jsonify({'ok': True, 'data': {
        'update_available': update_available,
        'current_version': cur,
        'latest_version': ver,
        'package_url': pkg,
        'sign_url': sig,
        'full_package_url': full_pkg,
        'full_sign_url': full_sig,
        'delta': pkg != full_pkg,
        'key_id': OTA_DEFAULT_KEY_ID,
    }})


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


def _power_action(verb: str):
    body = request.get_json(silent=True) or {}
    allowed, why = _power_action_allowed(verb)
    data = {'action': verb, 'scheduled': False, 'allowed': allowed, 'reason': why}
    if body.get('dry_run'):
        # dry_run 是验收脚本用的连通性自检（按 200 判定鉴权门是否放行），沿用 200。
        # 真实权限结论放在 allowed/reason，调用方可自行判断。
        return jsonify({'ok': True, 'data': data})
    if not allowed:
        return jsonify({'ok': False, 'error': why, 'data': data}), 403
    threading.Thread(target=lambda: (time.sleep(1.5), os.system('systemctl ' + verb)),
                     daemon=True).start()
    data['scheduled'] = True
    return jsonify({'ok': True, 'data': data})


@app.post('/api/system/reboot')
def reboot_system():
    return _power_action('reboot')


@app.post('/api/system/poweroff')
def poweroff_system():
    return _power_action('poweroff')


# -- 配置 --
def _deep_merge_profile(base: dict, patch: dict) -> dict:
    """RuntimeProfile 深合并：子对象（capture/fov/mouse/...）按键级合并而非整体替换。

    Web 前端每次 PUT 都是全量 collectConfig，但翻译层只产出非空子集；
    若浅合并，未提交的子对象（如 geometry_filter）会被 partial dict 整体顶掉，
    导致"保存一个字段 → 其它字段全丢"的参数失效问题。
    """
    merged = dict(base)
    for k, v in patch.items():
        if isinstance(v, dict) and isinstance(merged.get(k), dict):
            merged[k] = _deep_merge_profile(merged[k], v)
        else:
            merged[k] = v
    return merged


@app.put('/api/config')
@_config_write_serialized
def update_config():
    try:
        body = request.get_json(force=True)
    except Exception:
        body = {}
    if body is None:
        body = {}
    # 读当前配置：唯一来源 = Core IPC（Core 离线 ⇒ CoreUnavailableError → 503 fail-loud）
    prof = _get_runtime_profile()
    if not isinstance(body, dict) or not body:
        # ★ 空 body 不是"保存空配置成功"：原实现返回 ok:true 会把"没保存"伪装成"已保存"。
        return jsonify({'ok': False, 'error': '请求体为空，未保存'}), 400
    try:
        translated = web_body_to_profile(body, prev_profile=prof)
    except ConfigValidationError as exc:
        # 档位表配错（热键重叠 / 主键缺失 / 同时按下缺副键…）如实报 400 + 人话原因，
        # 而不是让它冒成 500。面板保存前也跑同一套校验（前端即时提示）；
        # 这里是硬护栏，防绕过面板的调用把非法档位表写进板子。
        return jsonify({'ok': False, 'error': str(exc)}), 400
    # 模型选中唯一真源是 ModelRegistry 的 active（/api/models/select 修改）。
    # 配置保存只在 body 明确携带非空 model_id 时透传；空串/缺失一律忽略，
    # 防止前端临时缺模型列表时回写空 model_id 把激活模型清掉。
    if not str(translated.get('model_id') or '').strip():
        translated.pop('model_id', None)
    base = prof
    merged = _deep_merge_profile(base, translated)
    merged = normalize_profile_capture_size(merged)
    r = ipc_request('SET_CONFIG', {'profile': merged})
    if r.get('status') != 0:
        # 不落盘（web 非配置写入者，C-CFG-3），按要求如实报错（fail-loud）。
        # ★ 必须把 Core 的原话带出来：status != 0 有两个完全不同的原因，靠状态码区分
        #   （core/src/ipc/IpcServer.hpp 的 IpcError：1=参数错 2=未找到 3=内部 4=不支持；
        #     lib/ipc.py 的传输失败也统一归 3）：
        #     ① status==3 ⇒ Core 不在 / 传输异常；
        #     ② status∈{1,2,4} ⇒ Core 在，但拒收这份配置（带具体原因，如 "capture.width 非法"）。
        #   以前一律写成"Core 未运行"，把 ② 的真实原因吞掉——板端实测就是这样把
        #   "面板所有保存都失败"指错了方向（详见 docs/交付前Web实测报告-2026-09-19.md P0-1）。
        core_error = str(r.get('error') or '').strip()
        if r.get('status') == 3:
            detail = f'；{core_error}' if core_error else ''
            return jsonify({'ok': False, 'core_offline': True,
                            'error': 'Core 未运行，配置未保存（请先启动 ttbox-core）' + detail}), 503
        return jsonify({'ok': False, 'core_error': core_error or '未知原因',
                        'error': f'配置被 Core 拒绝：{core_error or "未知原因"}'}), 400
    rr = _get_runtime_profile()
    return jsonify({'ok': True, 'data': profile_to_web(rr)})


@app.get('/api/config')
def get_config_web():
    prof = _get_runtime_profile()
    return jsonify({'ok': True, 'data': profile_to_web(prof)})


def _auto_start_enabled() -> bool:
    try:
        out = subprocess.check_output(['systemctl', 'is-enabled', 'ttbox-core'],
                                      text=True, timeout=3).strip()
        return out == 'enabled'
    except Exception:
        return False


def _auto_start_payload() -> dict:
    """保持 Web 契约：enabled=false -> status=disabled/message=''；
    enabled=true -> status=next_boot/message='将在下次开机时自动启动'。"""
    enabled = _auto_start_enabled()
    if not enabled:
        return {'enabled': False, 'status': 'disabled', 'message': '', 'updated_at': 0}
    return {'enabled': True, 'status': 'next_boot',
            'message': '将在下次开机时自动启动', 'updated_at': int(time.time())}


def _fan_enabled(pwm_path: str, pwm_raw: int) -> bool:
    """风扇是否在转：有 PWM 节点且占空比 > 0。

    单拎出来是为了可测 —— Core 启动时就把 pwm 写成 255（满转），
    这里若恒返 False，面板显示"未启用"而风扇实际在转，是反向的假信息。
    """
    return bool(pwm_path) and pwm_raw > 0


def _fan_control_payload() -> dict:
    """保持 Web 契约 fan_control 结构（真实硬件读取）。"""
    import glob as _glob
    # 找 NPU 温控 PWM 节点（hwmon8/pwm1 是历史固定节点，TTBOX 动态探测）
    pwm_path = ''
    try:
        for p in _glob.glob('/sys/class/hwmon/hwmon*/pwm1'):
            name = open(os.path.join(os.path.dirname(p), 'name')).read().strip()
            if name in ('pwm-fan', 'pwmfan', 'fan', 'soc-thermal'):
                pwm_path = p
                break
        if not pwm_path and _glob.glob('/sys/class/hwmon/hwmon*/pwm1'):
            pwm_path = sorted(_glob.glob('/sys/class/hwmon/hwmon*/pwm1'))[0]
    except Exception:
        pass
    pwm_raw = 0
    pwm_percent = 0
    pwm_writable = False
    if pwm_path:
        try:
            pwm_raw = int(open(pwm_path).read().strip())
            pwm_percent = int(round(pwm_raw * 100 / 255))
        except Exception:
            pass
        pwm_writable = os.access(pwm_path, os.W_OK)
    # NPU 温度（devfreq 或 thermal zone）
    temp_c = 0.0
    try:
        for z in _glob.glob('/sys/class/thermal/thermal_zone*/type'):
            if 'soc' in open(z).read().lower() or 'npu' in open(z).read().lower():
                temp_c = int(open(os.path.join(os.path.dirname(z), 'temp')).read().strip()) / 1000.0
                break
    except Exception:
        pass
    return {
        'control_available': bool(pwm_path),
        # enabled 必须反映硬件真实状态：Core 启动时会把 pwm 写成满转
        # （core/src/app/Application.cpp 的"风扇满转"段，pwm << 255），
        # 此处若硬编码 False，面板会显示"未启用"而风扇实际在转 —— 与物理事实相反。
        'enabled': _fan_enabled(pwm_path, pwm_raw),
        'fan_rpm': 0,
        'last_error': '' if (not pwm_path or pwm_writable) else f'{pwm_path} 不可写（权限或只读挂载）',
        'pwm_path': pwm_path,
        'pwm_percent': pwm_percent,
        'pwm_raw': pwm_raw,
        'pwm_writable': pwm_writable,
        'source': 'npu',
        'source_label': 'NPU',
        'tachometer_available': False,
        'temperature_celsius': temp_c,
        'updated_at_ms': int(time.time() * 1000),
    }


def _loopout_payload() -> dict:
    """保持 Web 契约 loopout 结构（读真实 DRM connector 状态）。"""
    import glob as _glob
    connected = False
    connector_id = 0
    for base in sorted(_glob.glob('/sys/class/drm/card*-HDMI-*')):
        try:
            if open(os.path.join(base, 'status')).read().strip() == 'connected':
                connected = True
                connector_id = int(os.path.basename(base).split('-')[-1]) if os.path.basename(base).rsplit('-', 1)[-1].isdigit() else 0
                break
        except Exception:
            pass
    return {
        'active': False,
        'available': bool(_glob.glob('/dev/dri/card0')),
        'connected': connected,
        'connector_id': connector_id,
        'crtc_id': 0,
        'drm_device': '/dev/dri/card0',
        'drm_open': False,
        'dropped': 0,
        'enabled': False,
        'fps': 0.0,
        'frames': 0,
        'height': 0,
        'last_error': '',
        'overlay_active': False,
        'overlay_available': False,
        'overlay_draw_ms': 0.0,
        'overlay_dropped': 0,
        'overlay_enabled': False,
        'overlay_last_error': '',
        'overlay_pixel_format': '',
        'overlay_plane_id': 0,
        'overlay_plane_name': '',
        'overlay_status': 'disabled',
        'overlay_updates': 0,
        'pixel_format': 'rgb888',
        'refresh': 0,
        'status': 'disabled',
        'width': 0,
    }


# -- 模型卡 UI 扩展字段持久化（game_profile/preset_name/hailo/remote 等）--
# 不进 core manifest：这些是 Web 层 UI 绑定字段，独立存 installed/<id>/ui_meta.json，
# 避免污染 ModelAdapter 校验元数据、也不需要重编 Core。
_MODEL_UI_META_KEYS = ('game_profile', 'preset_name', 'hailo_pipeline_depth',
                       'remote_frame_format', 'class_names', 'description')


def _model_ui_meta_path(model_id: str) -> Path:
    # V-04：模型库根经 lib.paths.models_root() 单点派生（TTBOX_MODELS_ROOT > <prefix>/models）；
    # 原同义异名 TTBOX_MODEL_ROOT 已删除，不保留兼容读。
    return Path(ttbox_paths.models_root()) / 'installed' / model_id / 'ui_meta.json'


def _read_model_ui_meta(model_id: str) -> dict:
    try:
        p = _model_ui_meta_path(model_id)
        if p.exists():
            data = json.loads(p.read_text(encoding='utf-8'))
            return data if isinstance(data, dict) else {}
    except Exception:
        pass
    return {}


def _write_model_ui_meta(model_id: str, patch: dict) -> dict:
    cur = _read_model_ui_meta(model_id)
    for key in _MODEL_UI_META_KEYS:
        if key in patch:
            cur[key] = patch[key]
    p = _model_ui_meta_path(model_id)
    try:
        p.parent.mkdir(parents=True, exist_ok=True)
        tmp = p.with_suffix('.json.tmp')
        tmp.write_text(json.dumps(cur, ensure_ascii=False, indent=2), encoding='utf-8')
        os.replace(str(tmp), str(p))
    except Exception:
        raise
    return cur


def _merge_model_ui_meta(model: dict) -> dict:
    merged = dict(model)
    meta = _read_model_ui_meta(str(model.get('model_id') or model.get('id') or ''))
    if meta:
        for key in _MODEL_UI_META_KEYS:
            if key in meta:
                merged[key] = meta[key]
    return merged


def _effective_rknn_concurrency(record: dict) -> int:
    """界面显示的并发 = Core 实际 worker 数。
    manifest 显式配置了 worker_cores 就用它；否则取 Core 生效的全局默认（经 IPC，
    C-CFG-3：web 不直读磁盘）。Core 离线 ⇒ fail-loud。"""
    wc = str(record.get('worker_cores') or '').strip()
    if not wc:
        r = ipc_request('GET_CONFIG')
        if r.get('status') != 0:
            raise CoreUnavailableError('Core 离线，无法读取全局 worker_cores')
        wc = str(r.get('data', {}).get('worker_cores') or '').strip()
    tokens = [t.strip() for t in wc.split(',') if t.strip() in ('1', '2', '4')]
    count = len(tokens)
    return count if 1 <= count <= 3 else 3


def _models_patch_response(model_id: str, patch: dict):
    """统一返回：校验模型存在 → 写 ui_meta → 附带最新模型列表。模型不可用时返回 None。"""
    check = ipc_request('MODEL_LIST')
    if check.get('status') != 0:
        return None
    known = [m.get('model_id') for m in check.get('data', {}).get('models', [])]
    if model_id not in known:
        return None
    _write_model_ui_meta(model_id, patch)
    models_resp = list_models()
    models_data = {}
    if models_resp is not None:
        try:
            models_data = models_resp.get_json() or {}
        except Exception:
            models_data = {}
    return {'ok': True, 'data': {
        'message': '已保存',
        'model_id': model_id,
        **(models_data.get('data') or {}),
    }}


def _parse_model_labels_file(f) -> list:
    """解析导入表单的类别标签文件（.txt/.names/.json/.csv），返回去重后的类别名列表。"""
    if f is None or not f.filename:
        return []
    filename = (f.filename or '').lower()
    raw = f.read()
    try:
        text = raw.decode('utf-8', errors='replace') if isinstance(raw, bytes) else str(raw)
    except Exception:
        text = str(raw)
    items: list = []
    if filename.endswith('.json'):
        try:
            data = json.loads(text)
        except Exception:
            return []
        if isinstance(data, list):
            items = [str(x).strip() for x in data if str(x).strip()]
        elif isinstance(data, dict):
            for key in ('class_names', 'classes', 'names', 'labels'):
                candidate = data.get(key)
                if isinstance(candidate, list):
                    items = [str(x).strip() for x in candidate if str(x).strip()]
                    break
                if isinstance(candidate, dict):
                    items = [str(v).strip() for v in candidate.values() if str(v).strip()]
                    break
    elif filename.endswith('.csv'):
        for line in text.splitlines():
            line = line.strip()
            if not line:
                continue
            first = line.split(',', 1)[0].strip().strip('"').strip("'")
            if first:
                items.append(first)
    else:
        for line in text.splitlines():
            line = line.strip()
            if line and not line.startswith('#'):
                items.append(line)
    seen = set()
    result = []
    for item in items:
        item = str(item).strip()
        if item and item not in seen:
            seen.add(item)
            result.append(item)
    return result


def _save_model_preset_from_import(f) -> str:
    """导入预设参数文件到 PRESETS_DIR，返回安全预设名；不可用返回空字符串。"""
    if f is None or not f.filename:
        return ''
    try:
        raw = f.read()
        data = json.loads(raw)
    except Exception:
        return ''
    if not isinstance(data, dict):
        return ''
    name = str(data.get('name') or Path(f.filename).stem or 'imported')
    safe = re.sub('[^\\w\\-]', '_', name)[:64]
    d = Path(PRESETS_DIR)
    d.mkdir(parents=True, exist_ok=True)
    (d / (safe + '.json')).write_text(json.dumps(data, ensure_ascii=False, indent=2), encoding='utf-8')
    return safe


# -- 模型 --
@app.get('/api/models')
def list_models():
    response = ipc_request('MODEL_LIST')
    if response.get('status') != 0:
        return jsonify({'ok': False, 'error': response.get('error', 'ModelRegistry unavailable')}), 503
    data = response.get('data', {}) or {}
    models = []
    for record in data.get('models', []):
        models.append({
            'id': record.get('model_id'),
            'model_id': record.get('model_id'),
            'name': record.get('name') or record.get('label') or record.get('model_id'),
            'label': record.get('label'),
            'version': record.get('version'),
            'format': record.get('format', 'rknn'),
            'path': record.get('path'),
            'status': record.get('record_status') or record.get('status'),
            'status_code': record.get('status_code'),
            'failure_code': record.get('failure_code', ''),
            'failure_message': record.get('failure_message', ''),
            'checksum': record.get('checksum') or record.get('sha256', ''),
            'origin': record.get('origin'),
            'created_at': record.get('created_at'),
            'updated_at': record.get('updated_at'),
            'backend': 'rknn',
            'input_width': record.get('input_width'),
            'input_height': record.get('input_height'),
            'input_layout': record.get('input_layout'),
            'input_dtype': record.get('input_dtype'),
            'quantization': record.get('quantization'),
            'output_format': record.get('output_format'),
            'output_count': record.get('output_count'),
            'class_count': record.get('class_count'),
            'class_names': record.get('class_names') or [],
            'rknn_concurrency': _effective_rknn_concurrency(record),
            'selected': bool(record.get('selected')),
            'running': bool(record.get('running')),
            'metadata': record.get('metadata') or {},
        })
    merged_models = [_merge_model_ui_meta(m) for m in models]
    return jsonify({'ok': True, 'data': {
        'models': merged_models,
        'selected_model_id': data.get('selected_model_id', ''),
        'running_model_id': data.get('running_model_id', ''),
        'state': data.get('state', 'unknown'),
    }})



# ---------------------------------------------------------------------------
# 模型转换（ONNX → RKNN，使用 TTBOX 自有转换链，不依赖板端其它转换工具）
#
# 行为基准 = TTBOX 板端实测：
#   - 转换器: /opt/ttbox/tools/converter/convert_onnx_to_rknn.py
#   - venv:   /opt/ttbox/venv-convert（rknn-toolkit2 2.3.2）
#   - INT8 asymmetric_quantized-8, mean 0/0/0, std 255/255/255
#   - 输出布局 auto（raw6 → raw9 → yolo26 → raw3 → graph），失败自动重试
#   - 校准图: 用户上传 zip，缺省用 /opt/ttbox/calib/imgs
#   - 同一时间只允许一个转换（互斥）
# 转换任务状态: idle / converting / success / failed
# 转换产物不直接进模型库：统一走 MODEL_IMPORT → MODEL_VALIDATE → MODEL_INSTALL。
# ---------------------------------------------------------------------------
_CONVERT_STATE = {'state': 'idle', 'error': '', 'model_id': '', 'started_at': 0.0,
                  'finished_at': 0.0, 'message': ''}
_CONVERT_LOCK = threading.Lock()
_CONVERT_WORKDIR = Path(os.environ.get('TTBOX_CONVERT_WORKDIR', '/tmp/ttbox_onnx_convert'))
_CONVERTER_SCRIPT = os.environ.get('TTBOX_CONVERTER_SCRIPT',
                                   '/opt/ttbox/tools/converter/convert_onnx_to_rknn.py')
_CONVERTER_PYTHON = os.environ.get('TTBOX_CONVERTER_PYTHON',
                                   '/opt/ttbox/venv-convert/bin/python')
_CONVERT_CALIB_DIR = os.environ.get('TTBOX_CONVERT_CALIB_DIR',
                                    '/opt/ttbox/calib/imgs')


def _convert_state_public() -> dict:
    return {k: _CONVERT_STATE.get(k, '') for k in
            ('state', 'error', 'model_id', 'started_at', 'finished_at', 'message')}


def _run_onnx_conversion(onnx_path: Path, output_path: Path, log_lines: list,
                         dataset_root: str = '') -> bool:
    """调用 TTBOX 转换器。成功返回 True；失败按 Web 契约行为自动重试一次（graph 布局降级）。"""
    if not dataset_root:
        dataset_root = _CONVERT_CALIB_DIR
    cmd = [
        _CONVERTER_PYTHON, _CONVERTER_SCRIPT,
        '--onnx', str(onnx_path),
        '--dataset-root', dataset_root,
        '--dataset-count', '5',                      # 实测：缺省 5 张 evenly sample
        '--target-platform', 'rk3588',
        '--output', str(output_path),
    ]
    for attempt in (1, 2):
        try:
            proc = subprocess.run(cmd, capture_output=True, text=True, timeout=1200)
            log_lines.append((proc.stdout or '')[-4000:] + (proc.stderr or '')[-2000:])
            if proc.returncode == 0 and output_path.exists() and output_path.stat().st_size > 0:
                return True
        except subprocess.TimeoutExpired:
            log_lines.append('converter timeout (1200s)')
        if attempt == 1:
            # TTBOX 降级参数：跳过 shape inference + graph 布局
            cmd = cmd + ['--skip-shape-inference', '--output-layout', 'graph']
    return False


def _conversion_worker(onnx_tmp: Path, calib_tmp, model_id: str, label: str,
                       extra_meta: dict | None = None) -> None:
    """转换线程：ONNX → RKNN → 统一入库（与 RKNN 直接上传同一条 import→validate→install 路）。"""
    work = _CONVERT_WORKDIR / model_id
    logs: list[str] = []
    try:
        work.mkdir(parents=True, exist_ok=True)
        onnx_src = work / 'source.onnx'
        onnx_tmp.replace(onnx_src)
        rknn_out = work / (model_id + '_raw_int8_rk3588.rknn')
        dataset_root = _CONVERT_CALIB_DIR
        calib_dir = None
        if calib_tmp is not None and calib_tmp.exists():
            calib_dir = work / 'calib'
            calib_dir.mkdir(parents=True, exist_ok=True)
            try:
                with zipfile.ZipFile(calib_tmp, 'r') as zf:
                    zf.extractall(calib_dir)
                if any(p.is_file() and p.suffix.lower() in
                       {'.jpg', '.jpeg', '.png', '.bmp', '.webp'}
                       for p in calib_dir.rglob('*')):
                    dataset_root = str(calib_dir)
                else:
                    calib_dir = None
                    dataset_root = _CONVERT_CALIB_DIR
            except (zipfile.BadZipFile, OSError) as exc:
                calib_dir = None
                dataset_root = _CONVERT_CALIB_DIR
                print(f'calibration zip extract failed, fallback default: {exc}', file=sys.stderr)
        if not _run_onnx_conversion(onnx_src, rknn_out, logs, dataset_root=dataset_root):
            detail = (logs[-1] if logs else '').strip()[-300:]
            _CONVERT_STATE.update(state='failed',
                                  error=('ONNX 转换失败（已自动重试一次）' +
                                         (f'：{detail}' if detail else '')),
                                  finished_at=time.time())
            return
        # V-04：转换产物落点与 core 同根（lib.paths.models_root()），不再写死绝对路径。
        inc = Path(ttbox_paths.models_root()) / '_incoming'
        inc.mkdir(parents=True, exist_ok=True)
        incoming_rknn = inc / (model_id + '.rknn')
        incoming_rknn.write_bytes(rknn_out.read_bytes())
        import hashlib as _hashlib
        _sha = _hashlib.sha256(incoming_rknn.read_bytes()).hexdigest()
        # ★ 1.5.61：ONNX 转换入库同样是「三步导入事务」，也要登记（见 _begin_import 注释）。
        _begin_import(model_id)
        try:
            r1 = ipc_request('MODEL_IMPORT', {'src_path': str(incoming_rknn),
                                              'model_id': model_id, 'label': label,
                                              'source_format': 'onnx', 'sha256': _sha}, timeout=30)
            if r1.get('status') != 0:
                _CONVERT_STATE.update(state='failed', error=r1.get('error', '入库失败'),
                                      finished_at=time.time())
                return
            r2 = ipc_request('MODEL_VALIDATE', {'model_id': model_id}, timeout=120)
            if r2.get('status') != 0:
                _CONVERT_STATE.update(state='failed', error=r2.get('error', 'RKNN 校验失败'),
                                      finished_at=time.time())
                return
            r3 = ipc_request('MODEL_INSTALL', {'model_id': model_id}, timeout=30)
            if r3.get('status') != 0:
                _CONVERT_STATE.update(state='failed', error=r3.get('error', '安装失败'),
                                      finished_at=time.time())
                return
        finally:
            _end_import(model_id)
        if extra_meta:
            try:
                _write_model_ui_meta(model_id, extra_meta)
            except Exception:
                pass
        _CONVERT_STATE.update(state='success', error='', finished_at=time.time(),
                              message='ONNX 已转换并入库')
    except Exception as exc:  # 转换线程兜底：任何异常都必须落到 failed 状态
        _CONVERT_STATE.update(state='failed', error=str(exc)[:500], finished_at=time.time())
    finally:
        shutil.rmtree(work, ignore_errors=True)      # 临时目录必须清理
        if calib_tmp is not None:
            calib_tmp.unlink(missing_ok=True)


@app.get('/api/models/convert-status')
def convert_status():
    with _CONVERT_LOCK:
        return jsonify({'ok': True, 'data': _convert_state_public()})


@app.post('/api/models/import-onnx')
def import_onnx():
    f = request.files.get('file')
    if f is None or not f.filename:
        return jsonify({'ok': False, 'error': '缺少 ONNX 模型文件'})
    if not f.filename.lower().endswith('.onnx'):
        return jsonify({'ok': False, 'error': '仅支持 .onnx 文件（RKNN 请走 /api/models/import）'})
    with _CONVERT_LOCK:                              # 互斥：同一时间只允许一个转换
        if _CONVERT_STATE.get('state') == 'converting':
            return jsonify({'ok': False, 'error': '另一个 ONNX 转换正在进行中'}), 409
        stem = re.sub(r'\.onnx$', '', f.filename, flags=re.I)
        model_id = re.sub(r'[^A-Za-z0-9_\-]', '_', stem)[:64].strip('_') or 'model'
        label = stem.strip() or model_id
        extra_meta = {}
        class_names = _parse_model_labels_file(request.files.get('labels_file'))
        preset_name = _save_model_preset_from_import(request.files.get('preset_file'))
        if class_names:
            extra_meta['class_names'] = class_names
        if preset_name:
            extra_meta['preset_name'] = preset_name
        game_profile = str(request.form.get('game_profile') or '').strip() or 'generic'
        if game_profile != 'generic':
            extra_meta['game_profile'] = game_profile
        description = str(request.form.get('description') or '').strip()
        if description:
            extra_meta['description'] = description
        _CONVERT_WORKDIR.mkdir(parents=True, exist_ok=True)
        onnx_tmp = _CONVERT_WORKDIR / ('upload_%d.onnx' % int(time.time() * 1000))
        f.save(str(onnx_tmp))
        calib_tmp = None
        calib_zip = request.files.get('calibration_zip')
        if calib_zip is not None and calib_zip.filename:
            if not calib_zip.filename.lower().endswith('.zip'):
                onnx_tmp.unlink(missing_ok=True)
                return jsonify({'ok': False, 'error': '校准图压缩包必须是 .zip'})
            calib_tmp = _CONVERT_WORKDIR / ('calib_%d.zip' % int(time.time() * 1000))
            calib_zip.save(str(calib_tmp))
        _CONVERT_STATE.update(state='converting', error='', model_id=model_id,
                              started_at=time.time(), finished_at=0.0,
                              message='正在导入并转换')
        threading.Thread(target=_conversion_worker,
                         args=(onnx_tmp, calib_tmp, model_id, label, extra_meta),
                         daemon=True).start()
    return jsonify({'ok': True, 'data': {'message': '已开始转换', 'model_id': model_id}})


@app.get('/api/models/device-code')
def model_device_code():
    # 保持 Web 契约：code/device_fingerprint_hash/device_id/format 结构
    cpu_serial = ''
    try:
        with open('/proc/cpuinfo') as f:
            for line in f:
                if line.startswith('Serial'):
                    cpu_serial = line.split(':', 1)[1].strip()
                    break
    except Exception:
        pass
    device_id = f'opi-{cpu_serial}' if cpu_serial else 'opi-ttbox-local'
    return jsonify({'ok': True, 'data': {
        'code': 'AIMK1_' + device_id.replace('-', '')[:40],
        'device_fingerprint_hash': device_id,
        'device_id': device_id,
        'format': 'AIMK1',
    }})


# ── 模型导入事务锁（1.5.61：修 "上传后立刻切换 → MODEL_NOT_FOUND"）──────────
# 背景：Core 的 IPC 是「每连接一线程」，导入链 IMPORT→VALIDATE→INSTALL 三次 IPC
#   之间是锁空闲窗口。此刻到达的 MODEL_ACTIVATE 会抢在 INSTALL 前面跑，而
#   installed/<id> 还没建出来（staging 不在 ModelRegistry 的搜索路径里）⇒
#   必然回裸 MODEL_NOT_FOUND。过一会再点又好了——就是用户看到的现象。
# 修法：web 侧把「正在导入」的 model_id 登记进事件表；/api/models/select 命中时
#   有界等待导入结束再 ACTIVATE（不报错）。**不能**在 Core 的 activate 里重试——
#   activate 与 install 共用 registry mutex_，重试期间持锁会把 install 挡在外面。
_MODEL_IMPORT_EVENTS: dict = {}
_MODEL_IMPORT_EVENTS_LOCK = threading.Lock()


def _begin_import(model_id: str):
    with _MODEL_IMPORT_EVENTS_LOCK:
        ev = _MODEL_IMPORT_EVENTS.get(model_id)
        if ev is None:
            ev = threading.Event()
            _MODEL_IMPORT_EVENTS[model_id] = ev
        return ev


def _end_import(model_id: str):
    with _MODEL_IMPORT_EVENTS_LOCK:
        ev = _MODEL_IMPORT_EVENTS.pop(model_id, None)
    if ev is not None:
        ev.set()


def _is_importing(model_id: str) -> bool:
    with _MODEL_IMPORT_EVENTS_LOCK:
        return model_id in _MODEL_IMPORT_EVENTS


def _wait_import_done(model_id: str, timeout: float) -> bool:
    """等该模型的导入事务结束。未在导入中 ⇒ 立返 True；超时 ⇒ False。"""
    with _MODEL_IMPORT_EVENTS_LOCK:
        ev = _MODEL_IMPORT_EVENTS.get(model_id)
    if ev is None:
        return True
    return ev.wait(timeout)


@app.post('/api/models/import')
def import_model():
    f = request.files.get('file')
    if f is None or not f.filename:
        return jsonify({'ok': False, 'error': 'missing upload field: file'})
    fname = f.filename
    class_names = _parse_model_labels_file(request.files.get('labels_file'))
    preset_name = _save_model_preset_from_import(request.files.get('preset_file'))
    # Windows 本地环境（TTBOX_ALLOW_ONNX=1）允许 .onnx 直入（无 RKNN 转换链）；
    # 板端保持 .rknn 单一入口，行为不变。
    allow_onnx = os.environ.get('TTBOX_ALLOW_ONNX', '') == '1'
    lower = fname.lower()
    if lower.endswith('.onnx') and allow_onnx:
        pass
    elif not lower.endswith('.rknn'):
        return jsonify({'ok': False, 'error': '仅支持 .rknn 模型文件'})
    stem = re.sub(r'\.(rknn|onnx)$', '', fname, flags=re.I)
    model_id = re.sub(r'[^A-Za-z0-9_\-]', '_', stem)[:64].strip('_') or 'model'
    # label 保留原始文件名主干（含中文），供前端显示；model_id 是净化后的内部标识
    label = stem.strip() or model_id
    incoming = Path(ttbox_paths.models_root()) / '_incoming'
    incoming.mkdir(parents=True, exist_ok=True)
    src_ext = '.onnx' if lower.endswith('.onnx') else '.rknn'
    dst = incoming / f'{model_id}{src_ext}'
    f.save(str(dst))
    import hashlib as _hashlib
    _sha = _hashlib.sha256(dst.read_bytes()).hexdigest()
    # ★ 1.5.61：登记导入事务（切换请求撞进来前先等这个事务结束，见上方注释）。
    _begin_import(model_id)
    try:
        r1 = ipc_request('MODEL_IMPORT', {'src_path': str(dst), 'model_id': model_id, 'label': label,
                                          'source_format': 'onnx' if src_ext == '.onnx' else 'rknn',
                                          'sha256': _sha})
        if r1.get('status') != 0:
            dst.unlink(missing_ok=True)
            return jsonify({'ok': False, 'error': r1.get('error', '导入失败')})
        # ★ 超时分级（api_v1.py 表）：模型加载可到分钟级，默认 5s 会把"正在加载"误判成
        #   "Core 挂了"，操作者会反复重试。VALIDATE/ACTIVATE ≥120s、INSTALL 60s。
        r2 = ipc_request('MODEL_VALIDATE', {'model_id': model_id}, timeout=120)
        if r2.get('status') != 0:
            return jsonify({'ok': False, 'error': r2.get('error', '校验失败')})
        r3 = ipc_request('MODEL_INSTALL', {'model_id': model_id}, timeout=60)
        if r3.get('status') != 0:
            return jsonify({'ok': False, 'error': r3.get('error', '安装失败')})
    finally:
        _end_import(model_id)
    ui_meta = {}
    if class_names:
        ui_meta['class_names'] = class_names
    if preset_name:
        ui_meta['preset_name'] = preset_name
    game_profile = str(request.form.get('game_profile') or '').strip() or 'generic'
    if game_profile != 'generic':
        ui_meta['game_profile'] = game_profile
    description = str(request.form.get('description') or '').strip()
    if description:
        ui_meta['description'] = description
    if ui_meta:
        try:
            _write_model_ui_meta(model_id, ui_meta)
        except Exception:
            pass
    return jsonify({'ok': True, 'data': {'message': '导入成功', 'model_id': model_id}})


@app.post('/api/models/delete')
def delete_model():
    body = request.get_json(silent=True) or {}
    model_id = str(body.get('model_id') or '').strip()
    if not model_id:
        return jsonify({'ok': False, 'error': 'missing field: model_id'}), 400
    r = ipc_request('MODEL_REMOVE', {'model_id': model_id})
    if r.get('status') != 0:
        return jsonify({'ok': False, 'error': r.get('error', '删除失败')})
    return jsonify({'ok': True, 'data': {'message': '已删除'}})


@app.post('/api/models/select')
def select_model():
    body = request.get_json(silent=True) or {}
    model_id = str(body.get('model_id') or '').strip()
    if not model_id:
        return jsonify({'ok': False, 'error': 'missing field: model_id'}), 400
    # ★ 1.5.61：该模型若正在导入（IMPORT→VALIDATE→INSTALL 三步事务未完成），先等它装完
    #   再激活。否则 ACTIVATE 会抢在 INSTALL 前面跑，installed/<id> 还没建 ⇒ 裸
    #   MODEL_NOT_FOUND（用户视角：上传后立刻切换报错，过一会再点又好了）。
    if not _wait_import_done(model_id, timeout=90.0):
        return jsonify({'ok': False,
                        'error': f'模型 {model_id} 还在导入中，请等导入完成后再切换'}), 409
    # ★ 同 api_v1 超时分级：MODEL_ACTIVATE 要加载并跑通 RKNN，板端实测分钟级，默认 5s 必误判。
    response = ipc_request('MODEL_ACTIVATE', {'model_id': model_id}, timeout=120)
    if response.get('status') != 0:
        return jsonify({'ok': False, 'error': response.get('error', '激活失败')}), 409
    status_response = ipc_request('MODEL_LIST')
    if status_response.get('status') != 0:
        return jsonify({'ok': False, 'error': status_response.get('error', 'ModelRegistry unavailable')}), 503
    data = status_response.get('data', {}) or {}
    # ★ 参照物 applySelectedModel 消费 result.config / result.models / result.presets /
    #   result.model —— 缺任一键会导致切换模型后「配置表单 / 模型卡片 / 预设列表 / 选中项」
    #   静默不刷新。故后端并齐这 4 键：models 与 /api/state.data.models 同形（_models_view），
    #   config 复用 profile_to_web（单一真源），presets 为预设名数组，model 为当前选中模型卡片。
    models_view = _models_view(data)
    active_id = data.get('selected_model_id', model_id) or model_id
    selected_model = next((m for m in models_view if m.get('id') == active_id), None)
    # ★ 同上：排除 `_` 保留名，否则切换模型后前端会把诊断报告当"当前预设"去自动保存 ⇒ 500
    presets = _preset_names()
    try:
        config_web = profile_to_web(_get_runtime_profile())
    except Exception:
        config_web = {}
    return jsonify({'ok': True, 'data': {
        'message': '模型已切换，Core 已加载新模型并完成首帧验证',
        'restart_required': False,
        'selected_model_id': active_id,
        'running_model_id': data.get('running_model_id', ''),
        'state': data.get('state', 'switching'),
        'models': models_view,
        'config': config_web,
        'presets': presets,
        'model': selected_model,
    }})


@app.post('/api/models/bind-preset')
def bind_model_preset():
    body = request.get_json(silent=True) or {}
    if not body.get('model_id'):
        return jsonify({'ok': False, 'error': 'model_id is required'}), 400
    model_id = str(body.get('model_id') or '').strip()
    preset_name = str(body.get('preset_name') or '').strip()
    r = _models_patch_response(model_id, {'preset_name': preset_name})
    if r is None:
        return jsonify({'ok': False, 'error': '模型不存在或不可用'}), 404
    r['data']['model'] = {'preset_name': preset_name}
    return jsonify(r)


@app.post('/api/models/remote-frame-format')
def update_model_remote_frame_format():
    body = request.get_json(silent=True) or {}
    if not body.get('model_id', ''):
        return jsonify({'ok': False, 'error': 'model_id is required'})
    model_id = str(body.get('model_id') or '').strip()
    fmt = str(body.get('remote_frame_format') or 'jpeg').strip().lower()
    if fmt not in ('jpeg', 'nv12', 'h264'):
        fmt = 'jpeg'
    r = _models_patch_response(model_id, {'remote_frame_format': fmt})
    if r is None:
        return jsonify({'ok': False, 'error': '模型不存在或不可用'}), 404
    r['data']['message'] = '帧格式已保存'
    return jsonify(r)


@app.post('/api/models/rknn-concurrency')
def update_model_rknn_concurrency():
    body = request.get_json(silent=True) or {}
    if not body.get('model_id'):
        return jsonify({'ok': False, 'error': 'model_id is required'}), 400
    raw_count = body.get('count', body.get('rknn_concurrency'))
    try:
        count = int(raw_count)
    except (TypeError, ValueError):
        return jsonify({'ok': False, 'error': 'count 必须是 1~3 的整数'}), 400
    if count < 1 or count > 3:
        return jsonify({'ok': False, 'error': 'count 必须在 1~3 之间'}), 400
    r = ipc_request('MODEL_SET_CONCURRENCY', {
        'model_id': body['model_id'],
        'count': count,
    })
    if r.get('status') != 0:
        return jsonify({'ok': False, 'error': r.get('error', '设置并发失败')}), 409
    models_resp = list_models()
    models_data = {}
    if models_resp is not None:
        try:
            models_data = models_resp.get_json() or {}
        except Exception:
            models_data = {}
    return jsonify({'ok': True, 'data': {
        'message': '并发已保存并生效',
        'model_id': body['model_id'],
        'rknn_concurrency': count,
        'restart_required': False,
        **(models_data.get('data') or {}),
    }})


@app.post('/api/models/hailo-pipeline-depth')
def update_model_hailo_pipeline_depth():
    body = request.get_json(silent=True) or {}
    if not body.get('model_id', ''):
        return jsonify({'ok': False, 'error': 'model_id is required'})
    model_id = str(body.get('model_id') or '').strip()
    try:
        depth = max(1, min(4, int(body.get('hailo_pipeline_depth') or 3)))
    except (TypeError, ValueError):
        depth = 3
    r = _models_patch_response(model_id, {'hailo_pipeline_depth': depth})
    if r is None:
        return jsonify({'ok': False, 'error': '模型不存在或不可用'}), 404
    r['data']['message'] = '流水线深度已保存'
    return jsonify(r)


@app.post('/api/models/class-names')
def update_model_class_names():
    body = request.get_json(silent=True) or {}
    if not body.get('model_id'):
        return jsonify({'ok': False, 'error': 'model_id is required'}), 400
    model_id = str(body.get('model_id') or '').strip()
    class_names = body.get('class_names')
    if not isinstance(class_names, list):
        return jsonify({'ok': False, 'error': 'class_names 必须是数组'}), 400
    clean = [str(n).strip() for n in class_names if str(n).strip()]
    r = _models_patch_response(model_id, {'class_names': clean})
    if r is None:
        return jsonify({'ok': False, 'error': '模型不存在或不可用'}), 404
    r['data']['message'] = '类别名称已保存'
    return jsonify(r)


# -- 预设 --
@app.get('/api/presets')
def list_presets():
    # ★ 排除 `_` 保留名（见 _preset_names 注释）
    return jsonify({'ok': True, 'data': {'presets': _preset_names()}})


@app.post('/api/presets')
def save_or_delete_preset():
    body = request.get_json(silent=True) or {}
    name = str(body.get('name', '')).strip()
    action = body.get('action', 'save')
    if not name:
        return jsonify({'ok': False, 'error': '缺少预设名'})
    d = Path(PRESETS_DIR)
    d.mkdir(parents=True, exist_ok=True)
    safe = re.sub('[^\\w\\-]', '_', name)[:64]
    # ★ 2026-09-23：保留名（_ 开头）是自动产物，只读不可写。
    #   以前这里会直接去写 `/opt/ttbox/presets/_dtbfix.json`（root 拥有、面板 ttbox 无写权限）
    #   ⇒ PermissionError ⇒ 未捕获 ⇒ HTTP 500 ⇒ 前端只看到"预设保存失败"。
    if not _is_preset_name(safe):
        return jsonify({'ok': False, 'error': f'预设名不能以 {_RESERVED_PRESET_PREFIX} 开头（保留给自动生成的诊断文件，只读）'})
    pf = d / (safe + '.json')
    if action == 'delete':
        pf.unlink(missing_ok=True)
        return jsonify({'ok': True, 'data': {'message': '已删除'}})
    if action == 'rename':
        new_name = str(body.get('new_name', '')).strip()
        safe2 = re.sub('[^\\w\\-]', '_', new_name)[:64]
        if not _is_preset_name(safe2):
            return jsonify({'ok': False, 'error': f'新预设名不能以 {_RESERVED_PRESET_PREFIX} 开头（保留名，只读）'})
        pf2 = d / (safe2 + '.json')
        pf2.write_text(pf.read_text() if pf.exists() else '{}')
        pf.unlink(missing_ok=True)
        return jsonify({'ok': True, 'data': {'message': '已重命名'}})
    config = body.get('config')
    if config is None:
        # 保持 Web 契约：仅传 name 时保存当前运行配置为预设
        # 统一存 Web 前端格式（profile_to_web），保证 load 时 web_body_to_profile 可反翻译
        # （此前直接存 RuntimeProfile 结构 → load 翻译层不识别 → "API 成功但实际没恢复"）
        try:
            config = profile_to_web(_get_runtime_profile())
        except Exception:
            config = {}
    # ★ 写失败要给**能看懂的错误**，不能让异常冒泡成 500（前端只会笼统显示"预设保存失败"）。
    try:
        pf.write_text(json.dumps(config, ensure_ascii=False, indent=2))
    except OSError as exc:
        return jsonify({'ok': False, 'error': f'预设写入失败（{pf}: {exc}）'})
    return jsonify({'ok': True, 'data': {'name': name}})


@app.post('/api/presets/load')
@_config_write_serialized
def load_preset():
    body = request.get_json(silent=True) or {}
    name = str(body.get('name', '')).strip()
    safe = re.sub('[^\\w\\-]', '_', name)[:64]
    pf = Path(PRESETS_DIR) / (safe + '.json')
    if not pf.exists():
        # 保持 Web 契约：报具体路径错误
        return jsonify({'ok': False, 'error': f'failed to open {pf}'})
    try:
        config = json.loads(pf.read_text())
    except Exception as exc:
        return jsonify({'ok': False, 'error': f'预设损坏: {exc}'})
    if not isinstance(config, dict) or not config:
        return jsonify({'ok': False, 'error': '预设内容为空'})
    # 兼容两种预设格式：
    #  1) Web 前端格式（新版）：有 video_detection_confidence/ai/aim_profiles → web_body_to_profile 翻译
    #  2) RuntimeProfile 结构（旧版）：有 inference/mouse/fov/capture 键 → 直接深合并
    if any(k in config for k in ('video_detection_confidence', 'ai', 'aim_profiles')):
        needs_translate = True
    elif any(k in config for k in ('inference', 'mouse', 'fov', 'capture')):
        needs_translate = False
        translated = config
    else:
        needs_translate = True
    if needs_translate:
        try:
            translated = web_body_to_profile(config, prev_profile=_get_runtime_profile())
        except ConfigValidationError as exc:
            # 预设里的档位表配错 → 400 + 人话原因（与 /api/config 同一口径）
            return jsonify({'ok': False, 'error': f'预设内容非法：{exc}'}), 400
    # ★ 预设里带保存时刻的 model_id（profile_to_web 恒输出），深合并会覆盖当前值，
    #   Core 侧 "model_id 只能通过模型激活接口修改" 会整份拒收 ⇒ 换过模型后
    #   加载预设 100% 失败。两种格式（Web 翻译产物 / 旧 RuntimeProfile）都统一剥掉，
    #   model_id 只归 /api/models/select 管（与 update_config 的剥离纪律一致）。
    if isinstance(translated, dict):
        translated.pop('model_id', None)
    prof = _deep_merge_profile(_get_runtime_profile(), translated)
    prof = normalize_profile_capture_size(prof)
    r = ipc_request('SET_CONFIG', {'profile': prof})
    if r.get('status') != 0:
        # 不落盘（web 非配置写入者），如实报错（fail-loud）。
        # 状态码语义与 /api/config 一致：3=Core 不在/传输异常，1/2/4=Core 拒收（带真实原因）。
        core_error = str(r.get('error') or '').strip()
        if r.get('status') == 3:
            detail = f'；{core_error}' if core_error else ''
            return jsonify({'ok': False, 'core_offline': True,
                            'error': 'Core 未运行，配置未应用（请先启动 ttbox-core）' + detail}), 503
        return jsonify({'ok': False, 'core_error': core_error or '未知原因',
                        'error': f'预设被 Core 拒绝：{core_error or "未知原因"}'}), 400
    return jsonify({'ok': True, 'data': {'message': '已加载'}})


@app.post('/api/presets/import')
def import_preset():
    # 保持 Web 契约：需要上传文件
    f = request.files.get('file')
    if f is None or not f.filename:
        return jsonify({'ok': False, 'error': 'missing upload field: file'})
    try:
        data = json.loads(f.read())
    except Exception:
        return jsonify({'ok': False, 'error': 'invalid preset file'})
    if not isinstance(data, dict):
        return jsonify({'ok': False, 'error': 'preset file must be a JSON object'})
    # 兼容旧版导出：那时 /export 返回的是 API 信封而非预设本体，
    # 用户手里已有的导出文件形如 {"ok":true,"data":{"preset":{...}}}。
    # 这里认出来并把信封剥掉，免得那些文件白导一遍。
    env = data.get('data')
    if 'ok' in data and isinstance(env, dict) and isinstance(env.get('preset'), dict):
        data = env['preset']
    # 名称优先级：前端 FormData 里的 name（用户在"导入后名称"里填的）> 预设文件里的
    # name > 文件名主干。旧实现只认后两者，用户填了等于没填。
    requested = str(request.form.get('name') or '').strip()
    name = requested or str(data.get('name') or '') or Path(f.filename).stem or 'imported'
    safe = re.sub('[^\\w\\-]', '_', name)[:64]
    # ★ 与保存端同一条纪律：_xxx.json 是 root 生成的保留名（如诊断报告 _dtbfix.json），
    #   ttbox 只读；漏掉这个检查 → 导入名为 _xxx 的预设时覆盖 root 文件 → PermissionError
    #   → 未捕获 500（保存端修过的坑在导入端复发）。
    if not _is_preset_name(safe):
        return jsonify({'ok': False, 'error': f'预设名非法（{safe} 开头的是系统保留名）'}), 400
    d = Path(PRESETS_DIR)
    d.mkdir(parents=True, exist_ok=True)
    try:
        (d / (safe + '.json')).write_text(json.dumps(data, ensure_ascii=False, indent=2))
    except OSError as exc:
        return jsonify({'ok': False, 'error': f'预设写入失败：{exc}'}), 500
    return jsonify({'ok': True, 'data': {'name': safe, 'preset': data}})


@app.get('/api/presets/<name>/export')
def export_preset(name: str):
    """导出预设 —— 返回**预设文件本体**，不是 API 信封。

    这个路由的消费方是前端 <a href download> 链接（index.html::presetExportUrl
    拼出来），浏览器把响应体直接存成文件。旧实现返回
    {'ok':..,'data':{'preset':..}}，等于把信封当预设导出：用户再导入时，导入端看到的是
    信封（根上既没有 name，也没有 capture/ai/mouse 这些配置键），于是
    "导出 → 导入 → 加载"恒静默无效 —— 导入不报错，加载也不报错，就是没效果。

    文件不存在 / 损坏时仍返回 JSON 错误体（浏览器存下来的是一份错误说明，
    不会和正常预设混淆）。
    """
    safe = re.sub('[^\\w\\-]', '_', name)[:64]
    pf = Path(PRESETS_DIR) / (safe + '.json')
    if not pf.exists():
        return jsonify({'ok': False, 'error': f'failed to open {pf}'})
    try:
        data = json.loads(pf.read_text())
    except Exception as exc:
        return jsonify({'ok': False, 'error': f'preset is damaged: {exc}'})
    body = json.dumps(data, ensure_ascii=False, indent=2).encode('utf-8')
    return send_file(io.BytesIO(body), mimetype='application/json',
                     as_attachment=True, download_name=f'{safe}.json')


# -- 控制/校准 --
# 自动标定（真实闭环）：目标反馈读 Core GET_STATUS.metrics（aim_pos_x/y = AimThread 选中目标中心），
# 运动注入走 mouse.calibrating 标定模式（AimThread/OutputBackend 在 calibrating 期间无视热键放行 AI 移动）。
# 标定结果写 /opt/ttbox/config/calibration.json，并把 kp 换算写回 RuntimeProfile（Core 热更新）。
CALIBRATION_FILE = '/opt/ttbox/config/calibration.json'
# 标定幅度表（单位 px 的参考点偏置）：**正负交替 + 分量程**。
# 为什么不是旧的全正 [8,16,24,32,40]：慢环下位移逐轮累加（轮间只清 bias、不等瞄点归位）
# ⇒ 比值散开 ⇒ 必挂 fit 的一致性门（MAD/|中位| > 0.35）。交替后相邻两步互相抵消，
# 同时每个幅度都覆盖到，够撑满 fit 的 min_samples=5。
CALIB_AMPLITUDES = (8.0, -8.0, 16.0, -16.0, 24.0, -24.0, 32.0, -32.0)
# 标定期"温和档" PID：bias 是最高 ±32px 的阶跃，用实战参数在 ~50ms 采集回路延迟下
# 会打进持续振荡（实测 ±150px），把目标甩出画面 ⇒ 整轮 no_target 作废
# （2026-09-24 板上 A/B：kp0.10/kd0.30 十六轮全稳）。gain=Δpx/ΔΣcounts 是闭环恒等式、
# 与 PID 参数无关 ⇒ 压 PID 不影响测量。kd 按 3×kp 给阻尼。
# ★ V1.0.13（2026-09-30）：10.0 是**旧名义值**（实际生效 0.10）。core 删掉 smooth 之后
#   mouse.kp_x 就是生效值，这里必须一起换域 —— 否则标定期会把 10 直接写进 kp_x，
#   环路自激 100 倍，目标被甩飞、整轮标定白跑。
CALIB_PID_KP_MAX = 0.10
CALIB_PID_KD_RATIO = 3.0
# ★ V1.0.13（2026-09-30）：CALIB_DEFAULT_SMOOTH_X / CALIB_MAX_SMOOTH_X 与
#   _calib_live_smooth() 一并删除 —— core 侧 smooth_x 已折叠进 kp/kd，配置里没有这个量了。
#   继续按默认 9900 传，推导出的 kp 会**强 100 倍**（详见 derive_pid_params 的注释）。
# 单个样本的最低信号门槛：count 太少 ⇒ 分母接近 0，比值被噪声主导；
# 位移太少 ⇒ 被检测噪声（实测静止抖动 ±0.05px）淹没。
CALIB_MIN_COUNTS = 6
CALIB_MIN_DELTA_PX = 0.5
ACTIVE_MODEL_FILE = '/opt/ttbox/models/active_model.txt'
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
}
_cal_lock = threading.Lock()
# ★ 配置读-改-写串行锁：Core 只保证单次 SET_CONFIG 原子，不保证跨请求的 RMW 原子。
#   waitress 64 线程 + 标定线程并发时，A 读→B 读→A 写→B 写 ⇒ A 的修改被 B 的整份
#   快照静默抹掉（"设置偶发不生效/被改回去"）。所有 GET_CONFIG→SET_CONFIG 序列必须持锁。
_CFG_WRITE_LOCK = threading.RLock()


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
        for axis in (CalibrationAxis.X, CalibrationAxis.Y):
            _calib_set(
                state=f'stabilize_{axis.value}',
                phase=f'stabilize_{axis.value}',
                current_axis=axis.value,
                round=0,
                progress=0.5 if axis is CalibrationAxis.Y else 0.0,
            )
            # 每轴动作前重新确认同一候选，避免目标切换混入测量。
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
                target_px = amp * 0.6
                first_response_ms = None
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
        for key in ('gain_x_px_per_count', 'gain_y_px_per_count'):
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
        # 生效值单列一份：面板可在"未标定"时如实展示"当前运行配置里的增益"
        'effective': eff,
    }
    return {'runtime': runtime, 'calibration': calib}


@app.get('/api/control/calibration')
def get_auto_calibration():
    return jsonify({'ok': True, 'data': _calibration_payload()})


@app.put('/api/control/calibration')
def update_auto_calibration():
    body = request.get_json(silent=True) or {}
    try:
        gain_x = float(body.get('gain_x_px_per_count') or body.get('mouse_gain_x_px_per_count') or 0)
        gain_y = float(body.get('gain_y_px_per_count') or body.get('mouse_gain_y_px_per_count') or 0)
        delay = float(body.get('response_delay_ms') or body.get('mouse_response_delay_ms') or 0)
    except (TypeError, ValueError):
        return jsonify({'ok': False, 'error': '参数格式错误'}), 400
    if gain_x <= 0 or gain_y <= 0:
        return jsonify({'ok': False, 'error': '增益必须 > 0'}), 400
    calib = {
        'mouse_gain_x_px_per_count': round(gain_x, 4),
        'mouse_gain_y_px_per_count': round(gain_y, 4),
        'mouse_response_delay_ms': round(delay, 3),
        'mouse_calibration_applied': True,
        'valid': True,
        'confidence': 0.0,
        'calibrated_at': time.strftime('%Y%m%d_%H%M%S'),
        'model_id': _read_active_model(),
    }
    ok, detail = _write_calibration(calib)
    if ok:
        ok2, detail2 = _calib_apply_gain(calib)
        detail = detail + '；' + detail2
        # 手动填增益同样联动自动调参（同一推导函数，保证行为一致）
        try:
            calib['pid_params'] = _calib_derive_pid(gain_x, gain_y, delay)
        except Exception:
            calib['pid_params'] = {}
        if ok2 and calib.get('pid_params'):
            ok3, detail3 = _calib_apply_pid(calib)
            detail = detail + '；' + detail3
            ok = ok and ok3
            _write_calibration(calib)
        if ok:
            _calib_set(status='manual', phase='done', ready=True, reason='completed')
        else:
            # Core 未运行时文件已保存但参数未生效：不标记完成（诚实反映）
            _calib_set(status='manual', phase='saved', ready=False, reason=detail)
    resp = jsonify({'ok': ok, 'data': _calibration_payload(), 'detail': detail})
    resp.status_code = 200 if ok else 500
    return resp


@app.post('/api/control/calibration/start')
def start_auto_calibration():
    with _cal_lock:
        if _cal['thread'] and _cal['thread'].is_alive():
            return jsonify({'ok': False, 'error': '标定已在运行中'}), 409
    st = _get_status()
    if not st.get('runtime_running'):
        return jsonify({'ok': False, 'error': '推理服务未运行或目标反馈未就绪（请先启动推理）'}), 400
    if _calib_target() is None:
        return jsonify({'ok': False, 'error': '未识别到目标，无法开始标定（请将准星对准画面中的目标，等待检测框稳定出现）'}), 400
    # V1.0.12（2026-09-30）：不再接受 scope_index —— 不区分倍镜，标定只写全局 gain。
    # 老面板若仍带这个字段，直接忽略（不报错，保持向后兼容）。
    th = threading.Thread(target=_calib_thread_entry, daemon=True)
    with _cal_lock:
        _cal['thread'] = th
    th.start()
    return jsonify({'ok': True, 'data': _calibration_payload(), 'detail': '标定已启动'})


@app.post('/api/control/calibration/cancel')
def cancel_auto_calibration():
    # 无条件落终态 `cancelled`：worker 还活着时它会在 1~2 秒内自己退出并补一次同样的
    # state（幂等）；worker 已因异常穿透而死、state 停在非终态时（见 _calib_worker 的
    # except 注释），**只有这里能复位** —— 否则面板会永远以为在标定、且取消按钮被禁用。
    _calib_set(state='cancelled', status='idle', phase='cancelled', ready=False, reason='cancelled')
    try:
        prof = _get_runtime_profile()
        prof.setdefault('mouse', {})['calibrating'] = False
        ipc_request('SET_CONFIG', {'profile': prof})
    except Exception:
        pass
    return jsonify({'ok': True, 'data': _calibration_payload(), 'detail': '标定已取消'})


@app.delete('/api/control/calibration')
def clear_auto_calibration():
    _clear_calibration()
    return jsonify({'ok': True, 'data': _calibration_payload(), 'detail': '标定已清除'})


@app.post('/api/control/start')
def start_control():
    # 保持 Web 契约：无模型时返回 error=未导入模型
    prof = _get_runtime_profile()
    if not prof.get('model_id'):
        return jsonify({'ok': False, 'error': '未导入模型'})
    r = ipc_request('RUNTIME_CONTROL', {'action': 'start'})
    if r.get('status') != 0:
        err = r.get('error') or '启动失败'
        if '模型' not in err and 'model' not in err.lower():
            err = '未导入模型'
        return jsonify({'ok': False, 'error': err})
    state = collect_web_state()
    data = state.get('data', state) if isinstance(state, dict) else state
    return jsonify({'ok': True, 'data': data})


@app.post('/api/control/stop')
def stop_control():
    r = ipc_request('RUNTIME_CONTROL', {'action': 'stop'})
    state = collect_web_state()
    data = state.get('data', state) if isinstance(state, dict) else state
    return jsonify({'ok': r.get('status') == 0, 'data': data})


# 瞄准轨迹记录（诊断）：后台线程按 50Hz 采样核心 aim 状态，存 /opt/ttbox/run/aim_trace.json
_aim_trace = {'running': False, 'samples': [], 'started_at': 0, 'stop_at': 0, 'thread': None}


@app.post('/api/diagnostics/aim-trace')
def start_aim_trace():
    body = request.get_json(silent=True) or {}
    duration_sec = int(body.get('duration_sec', 10))
    if duration_sec <= 0 or duration_sec > 120:
        return jsonify({'ok': False, 'error': 'duration_sec 必须在 1~120 之间'})
    if _aim_trace['running']:
        return jsonify({'ok': False, 'error': '已有轨迹记录进行中'})
    # Core 未运行时拒绝开始（避免记录全 0 假轨迹）
    st = _get_status()
    if not st.get('runtime_running'):
        return jsonify({'ok': False, 'error': '推理服务未运行，无法记录瞄准轨迹'}), 400
    _aim_trace['running'] = True
    _aim_trace['samples'] = []
    _aim_trace['started_at'] = time.time()
    _aim_trace['stop_at'] = time.time() + duration_sec

    def _collect():
        while _aim_trace['running'] and time.time() < _aim_trace['stop_at']:
            try:
                st = _get_status()
                m = st.get('metrics', {})
                _aim_trace['samples'].append({
                    't': round(time.time() - _aim_trace['started_at'], 3),
                    'err_x': round(m.get('aim_error_x', 0.0), 3),
                    'err_y': round(m.get('aim_error_y', 0.0), 3),
                    'move_x': m.get('mouse_dx', 0),
                    'move_y': m.get('mouse_dy', 0),
                    'target': m.get('target_frames', 0) > 0 or m.get('aim_active', False),
                })
            except Exception:
                pass
            time.sleep(0.02)
        try:
            os.makedirs('/opt/ttbox/run', exist_ok=True)
            with open('/opt/ttbox/run/aim_trace.json', 'w') as f:
                json.dump({'samples': _aim_trace['samples'], 'duration_sec': duration_sec}, f)
        except Exception:
            pass
        _aim_trace['running'] = False

    _aim_trace['thread'] = threading.Thread(target=_collect, daemon=True)
    _aim_trace['thread'].start()
    # 保持 Web 契约：返回 duration_sec/filename/path/recording 结构
    fname = f'aim_trace_{time.strftime("%Y%m%d_%H%M%S")}.jsonl'
    return jsonify({'ok': True, 'data': {
        'duration_sec': float(duration_sec),
        'filename': fname,
        'path': f'/opt/ttbox/run/{fname}',
        'recording': True,
    }})


@app.get('/api/diagnostics/usb-proxy.zip')
def download_usb_proxy_diagnostics():
    # 真实打包 usbproxy 诊断信息
    import io
    import zipfile
    buf = io.BytesIO()
    with zipfile.ZipFile(buf, 'w', zipfile.ZIP_DEFLATED) as zf:
        try:
            zf.writestr('usbproxy_status.txt', 'TTBOX usb-proxy diagnostic\n')
            try:
                out = subprocess.check_output(['systemctl', 'status', 'ttbox-usbproxy', '--no-pager'], text=True, timeout=5)
                zf.writestr('usbproxy_service.txt', out)
            except Exception:
                zf.writestr('usbproxy_service.txt', 'ttbox-usbproxy service not active\n')
            try:
                devs = sorted(glob.glob('/dev/hidg*'))
                zf.writestr('hidg_devices.txt', '\n'.join(devs) + '\n')
            except Exception:
                pass
        except Exception:
            pass
    buf.seek(0)
    return Response(buf.getvalue(), mimetype='application/zip',
                    headers={'Content-Disposition': 'attachment; filename=usb-proxy.zip'})


@app.get('/api/events')
def get_events():
    return jsonify({'ok': True, 'data': {'events': []}})


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


@app.get('/api/hardware/mouse')
def get_mouse_hardware():
    """保持 Web 契约：11 字段结构（config/config_source/connected/mode/physical_mouse/
    service_active/service_active_text/service_enabled/service_enabled_text/
    set_config_supported/timing）。"""
    import glob
    hidg = sorted(glob.glob('/dev/hidg*'))
    prof = _get_runtime_profile()
    mouse = prof.get('mouse') or {}
    # 真实 USB 鼠标探测（保持 Web 契约：sysfs USB 鼠标）
    usb_cfg = {
        'hid_interval': 1, 'hid_protocol': 2, 'hid_report_desc_hex': '',
        'hid_report_length': 64, 'hid_subclass': 1,
        'usb_bcd_device': '0x0100', 'usb_bcd_usb': '0x0200',
        'usb_configuration': '', 'usb_device_class': 0,
        'usb_device_protocol': 0, 'usb_device_subclass': 0,
        'usb_manufacturer': '', 'usb_max_power': 100,
        'usb_pid': '0x0000', 'usb_product': '', 'usb_serial': '',
        'usb_vid': '0x0000',
    }
    physical = {'device': '', 'interface': '', 'name': ''}
    connected = False
    try:
        # 找 USB 鼠标：/sys/bus/usb/devices/*/bInterfaceClass=03 (HID)
        for dev in sorted(glob.glob('/sys/bus/usb/devices/*')):
            bdev = os.path.basename(dev)
            if bdev.count('-') < 1 or not os.path.isdir(dev):
                continue
            iface = os.path.join(dev, 'bInterfaceClass')
            if not os.path.exists(iface):
                continue
            try:
                cls = open(iface).read().strip()
            except Exception:
                continue
            if cls == '03':  # HID 接口
                # 接口子目录无 vid/pid，向上找父 USB 设备（如 3-1:1.1 -> 3-1）
                parent = dev
                while parent.count(':') > 0:  # 去掉接口后缀
                    parent = parent.rsplit(':', 1)[0]
                try:
                    def _rd(p):
                        try:
                            return open(os.path.join(parent, p)).read().strip()
                        except Exception:
                            return ''
                    vid = _rd('idVendor')
                    pid = _rd('idProduct')
                    mfr = _rd('manufacturer')
                    prod = _rd('product')
                    cfg_name = _rd('configuration')
                    bcd_dev = _rd('bcdDevice')
                    bcd_usb = _rd('version')
                    dev_cls = _rd('bDeviceClass')
                    dev_sub = _rd('bDeviceSubClass')
                    dev_proto = _rd('bDeviceProtocol')
                    max_power = _rd('bMaxPower')  # 形如 "98mA"
                    usb_cfg['usb_vid'] = '0x' + vid if vid else usb_cfg['usb_vid']
                    usb_cfg['usb_pid'] = '0x' + pid if pid else usb_cfg['usb_pid']
                    usb_cfg['usb_manufacturer'] = mfr
                    usb_cfg['usb_product'] = prod
                    usb_cfg['usb_configuration'] = cfg_name
                    usb_cfg['usb_serial'] = _rd('serial')
                    if bcd_dev:
                        usb_cfg['usb_bcd_device'] = '0x' + bcd_dev
                    if bcd_usb:
                        usb_cfg['usb_bcd_usb'] = '0x' + bcd_usb.replace('.', '')
                    if dev_cls:
                        usb_cfg['usb_device_class'] = int(dev_cls, 16)
                    if dev_sub:
                        usb_cfg['usb_device_subclass'] = int(dev_sub, 16)
                    if dev_proto:
                        usb_cfg['usb_device_protocol'] = int(dev_proto, 16)
                    if max_power.endswith('mA'):
                        usb_cfg['usb_max_power'] = int(max_power[:-2])
                    # 鼠标接口(1.x)真实协议与端点 interval
                    try:
                        if _rd('bInterfaceProtocol') if False else True:
                            iface_proto = open(os.path.join(dev, 'bInterfaceProtocol')).read().strip()
                            iface_sub = open(os.path.join(dev, 'bInterfaceSubClass')).read().strip()
                            if iface_proto:
                                usb_cfg['hid_protocol'] = int(iface_proto)
                            if iface_sub:
                                usb_cfg['hid_subclass'] = int(iface_sub)
                        eps = sorted(glob.glob(os.path.join(dev, 'ep_*')))
                        for ep in eps:
                            iv = open(os.path.join(ep, 'bInterval')).read().strip()
                            if iv:
                                usb_cfg['hid_interval'] = int(iv)
                                break
                    except Exception:
                        pass
                    physical = {'device': os.path.basename(parent),
                                'interface': bdev, 'name': prod or mfr}
                    connected = True
                except Exception:
                    pass
                break
    except Exception:
        pass
    service_active = bool(hidg)
    # 服务状态查 TTBOX 自己的 usbproxy 服务（保持 Web 契约 语义：service_active=服务在跑）
    try:
        out = subprocess.check_output(['systemctl', 'is-active', 'ttbox-usbproxy'],
                                      text=True, timeout=3).strip()
        service_active = out == 'active' or bool(hidg)
    except Exception:
        pass
    service_enabled = bool(mouse.get('enabled', False)) or service_active
    requested_mode = _mouse_current_mode(mouse)
    effective_mode = _usbproxy_effective_mode() or requested_mode
    return jsonify({
        'ok': True,
        'data': {
            'config': usb_cfg,
            'config_source': 'sysfs_usb_mouse' if connected else 'default',
            'connected': connected,
            'mode': requested_mode,
            # effective_mode 是进程实际在跑的模式（读不到进程 = 没在透传）。
            # mode_degraded：1.5.62 删除合成模式后恒为 False；保留键以免老面板取不到。
            'effective_mode': effective_mode,
            'mode_degraded': False,
            'physical_mouse': physical,
            'service_active': service_active,
            'service_active_text': 'active' if service_active else 'inactive',
            'service_enabled': service_enabled,
            'service_enabled_text': 'enabled' if service_enabled else 'disabled',
            'set_config_supported': True,
            'gadget_config_path': _usbproxy_gadget_config_path(),
            'gadget_config': _usbproxy_config_for_form(_usbproxy_current_gadget_config()),
            'timing': {
                'identity_change_settle_delay_sec': float(mouse.get('identity_change_settle_delay_sec', 0.5)),
                'max_delay_sec': float(mouse.get('max_delay_sec', 30.0)),
                'mouse_settle_delay_sec': float(mouse.get('mouse_settle_delay_sec', 8.0)),
            },
        },
    })


# ── usb-proxy gadget 身份配置通道（cmd.sock，0x4F50 协议）────────────────────
# 真源 = usbproxy/mouse_control.cpp：kSetConfigReq(14) 载荷 =
#     <B apply_now><HHHH><BBB><H><BBBB><5×<H len,bytes>>
# apply_now=1 时 usb-proxy 先落盘 gadget-config.json，再 _exit(0)，由 systemd
# Restart=always 拉起新配置 —— 这就是前端那句"Windows 会重新枚举"的实现。
#
# 旧实现把 usb_* 字段塞进 RuntimeProfile 发给 Core（Core 根本没这些成员，静默
# 丢弃），却恒返 applied=True，还附一条凭空揎造的 /etc/default/ttbox-usb-proxy
# —— 该路径全仓无人读，纯装饰。
USB_PROXY_MAGIC = 0x4F50
USB_PROXY_VERSION = 1
USB_PROXY_REQ_SET_CONFIG = 14
USB_PROXY_RESP_SET_CONFIG = 15
USB_PROXY_RESP_ERROR = 3
USB_PROXY_CONFIG_KEYS = (
    'usb_vid', 'usb_pid', 'usb_bcd_usb', 'usb_bcd_device', 'usb_device_class',
    'usb_device_subclass', 'usb_device_protocol', 'usb_max_power', 'hid_protocol',
    'hid_subclass', 'hid_report_length', 'hid_interval', 'usb_manufacturer',
    'usb_product', 'usb_serial', 'usb_configuration', 'hid_report_desc_hex',
)
USB_PROXY_HEX_KEYS = ('usb_vid', 'usb_pid', 'usb_bcd_usb', 'usb_bcd_device')


def _usbproxy_gadget_config_path() -> str:
    """usb-proxy 落盘的 gadget 配置文件（unit 的 WorkingDirectory=<release>/usbproxy/）。"""
    return os.path.join(ttbox_paths.repo_root(), 'usbproxy', 'gadget-config.json')


def _usbproxy_send_set_config(cfg: dict, apply_now: bool) -> tuple:
    """把 gadget 身份配置真正下发给 usb-proxy（cmd.sock，SOCK_SEQPACKET）。

    返回 (是否被 usb-proxy 确认, 失败原因人话)。连不上 / 无回包 / 协议不符 /
    被拒绝，一律算失败 —— 不再有"无条件 applied=True"这个分支。
    """
    def u16(value) -> bytes:
        return struct.pack('<H', int(value) & 0xFFFF)

    def u8(value) -> bytes:
        return struct.pack('<B', int(value) & 0xFF)

    def ustr(value) -> bytes:
        raw = str(value).encode('utf-8')
        return u16(len(raw)) + raw

    payload = b''.join([
        u16(cfg.get('usb_vid', 0)), u16(cfg.get('usb_pid', 0)),
        u16(cfg.get('usb_bcd_usb', 0x0200)), u16(cfg.get('usb_bcd_device', 0x0100)),
        u8(cfg.get('usb_device_class', 0)), u8(cfg.get('usb_device_subclass', 0)),
        u8(cfg.get('usb_device_protocol', 0)), u16(cfg.get('usb_max_power', 250)),
        u8(cfg.get('hid_protocol', 2)), u8(cfg.get('hid_subclass', 1)),
        u8(cfg.get('hid_report_length', 4)), u8(cfg.get('hid_interval', 1)),
        ustr(cfg.get('usb_manufacturer', '')), ustr(cfg.get('usb_product', '')),
        ustr(cfg.get('usb_serial', '')), ustr(cfg.get('usb_configuration', '')),
        ustr(cfg.get('hid_report_desc_hex', '')),
    ])
    sock_path = ttbox_paths.MOUSE_CMD_SOCK_DEFAULT
    try:
        conn = socket.socket(socket.AF_UNIX, socket.SOCK_SEQPACKET)
        conn.settimeout(5.0)
        conn.connect(sock_path)
    except Exception as exc:
        return False, f'usb-proxy 未运行（{sock_path} 连不上：{exc}）'
    try:
        conn.sendall(struct.pack('<HBB', USB_PROXY_MAGIC, USB_PROXY_VERSION,
                                 USB_PROXY_REQ_SET_CONFIG) + struct.pack('<I', 1) +
                     u8(1 if apply_now else 0) + payload)
        reply = conn.recv(512)
    except Exception as exc:
        return False, f'usb-proxy 无回包：{exc}'
    finally:
        try:
            conn.close()
        except Exception:
            pass
    if len(reply) < 8:
        return False, f'usb-proxy 回包过短（{len(reply)} 字节）'
    magic, version, rtype = struct.unpack_from('<HBB', reply, 0)
    if magic != USB_PROXY_MAGIC or version != USB_PROXY_VERSION:
        return False, f'usb-proxy 回包协议不符（magic=0x{magic:04x} version={version}）'
    if rtype == USB_PROXY_RESP_ERROR:
        detail = reply[8:].decode('utf-8', 'replace').strip('\x00').strip()
        return False, f'usb-proxy 拒绝该配置：{detail or "未给出原因"}'
    if rtype != USB_PROXY_RESP_SET_CONFIG:
        return False, f'usb-proxy 回包类型异常（type={rtype}）'
    return True, ''


def _usbproxy_current_gadget_config() -> dict:
    """读 usb-proxy 实际落盘的 gadget 配置；读不到返回空 dict（不编造）。"""
    try:
        with open(_usbproxy_gadget_config_path(), encoding='utf-8') as fh:
            data = json.load(fh)
    except Exception:
        return {}
    return data if isinstance(data, dict) else {}


def _usbproxy_config_for_form(cfg: dict) -> dict:
    """把落盘配置转成前端表单格式（vid/pid/bcd 用 0xXXXX 字符串，与 GET 一致）。"""
    out = {}
    for key in USB_PROXY_CONFIG_KEYS:
        if key not in cfg:
            continue
        value = cfg[key]
        if key in USB_PROXY_HEX_KEYS and isinstance(value, int):
            value = f'0x{value:04X}'
        out[key] = value
    return out


def _usbproxy_unit_mode() -> str:
    """usb-proxy 请求的透传模式 —— 读 systemd 单元的 Environment=USB_PROXY_MODE。

    1.5.62 起单元里已不再设置这一项（合成模式删除，只有物理透传一种模式）；
    这里仍保留读取，只是为了让历史 drop-in（10-mode.conf）残留时不被误判。
    """
    out = _run_quiet(['systemctl', 'show', '-p', 'Environment', 'ttbox-usbproxy'])
    for token in out.replace('"', ' ').split():
        if token.startswith('USB_PROXY_MODE='):
            return token.split('=', 1)[1].strip()
    return ''


def _usbproxy_effective_mode() -> str:
    """usb-proxy 进程**实际**跑的模式（1.5.62 起只有物理透传一种）。

    1.5.62 删除了合成模式：命令行带 --vendor_id 才是真在透传物理鼠标。
    没找到物理鼠标时进程不会起来（启动脚本一直等），读不到就返回 ''（不编造）。
    """
    try:
        for pid in os.listdir('/proc'):
            if not pid.isdigit():
                continue
            try:
                with open(f'/proc/{pid}/cmdline', 'rb') as fh:
                    cmd = fh.read().decode('utf-8', 'replace')
            except Exception:
                continue
            if 'usb-proxy' not in cmd:
                continue
            if '--vendor_id' in cmd:
                return 'full_passthrough'
    except Exception:
        pass
    return ''


def _mouse_current_mode(mouse: dict) -> str:
    """当前生效的透传模式：以 systemd 单元为准（web 改不了它），读不到才回落 profile。

    旧实现里有一句固定映射 "proxy/synthetic/local_hid -> full_passthrough"，
    等于无论实际什么模式都报透传；GET 又只取 profile（用户请求值）而非真值。
    前端单选值的 full 对应名是 full_passthrough，此处对齐。
    """
    unit = _usbproxy_unit_mode()
    if unit:
        return 'full_passthrough' if unit == 'full' else unit
    return str(mouse.get('mode') or mouse.get('proxy_mode') or '').strip() or 'full_passthrough'


def _mouse_apply_payload(mouse=None, applied=False, apply_error=''):
    """保持 Web 契约：PUT 后返回 applied/mode/service_*/timing 结构。

    applied 由真实下发结果决定，不再恒 True；apply_error 带失败人话原因；
    config 回填实际落盘的身份字段，让表单显示"已应用什么"而不是用户刚输入的。
    """
    import glob
    hidg = sorted(glob.glob('/dev/hidg*'))
    mouse = mouse or {}
    service_active = bool(hidg)
    try:
        out = subprocess.check_output(['systemctl', 'is-active', 'ttbox-usbproxy'],
                                      text=True, timeout=3).strip()
        service_active = out == 'active' or bool(hidg)
    except Exception:
        pass
    gadget_cfg = _usbproxy_current_gadget_config()
    return {
        'applied': bool(applied),
        'apply_error': apply_error,
        'gadget_config_path': _usbproxy_gadget_config_path(),
        'gadget_config': _usbproxy_config_for_form(gadget_cfg),
        'config': _usbproxy_config_for_form(gadget_cfg),
        'mode': _mouse_current_mode(mouse),
        'service_active': service_active,
        'service_active_text': 'active' if service_active else 'inactive',
        'service_enabled': service_active,
        'service_enabled_text': 'enabled' if service_active else 'disabled',
        'set_config_supported': True,
        'timing': {
            'identity_change_settle_delay_sec': float(mouse.get('identity_change_settle_delay_sec', 0.5)),
            'max_delay_sec': float(mouse.get('max_delay_sec', 30.0)),
            'mouse_settle_delay_sec': float(mouse.get('mouse_settle_delay_sec', 8.0)),
        },
    }


@app.put('/api/hardware/mouse')
@_config_write_serialized
def update_mouse_hardware():
    body = request.get_json(silent=True) or {}
    prof = _get_runtime_profile()
    mouse = prof.get('mouse') or {}
    # 前端字段：enabled/mode/config
    if 'mode' in body:
        mouse['mode'] = body['mode']
        mouse['proxy_mode'] = body['mode']
    if 'enabled' in body:
        # ★ 强转 bool：字符串 "false" 是真值，原样写会把"关闭"存成"开启"。
        mouse['enabled'] = bool(body['enabled']) if isinstance(body['enabled'], bool) \
            else str(body['enabled']).strip().lower() in ('1', 'true', 'yes', 'on')
    if 'config' in body and isinstance(body['config'], dict):
        mouse.update({k: v for k, v in body['config'].items() if k in (
            'usb_vid', 'usb_pid', 'usb_manufacturer', 'usb_product', 'usb_serial',
            'usb_max_power', 'hid_report_length', 'hid_interval', 'hid_protocol',
            'hid_subclass', 'usb_bcd_device', 'usb_bcd_usb', 'usb_configuration',
            'usb_device_class', 'usb_device_subclass', 'usb_device_protocol',
        )})
    prof['mouse'] = mouse
    ok, detail = _mouse_save_or_ipc(prof, mouse)
    # 身份配置的真源是 usb-proxy 的 gadget-config.json，不是 Core 的 RuntimeProfile
    # （Core 没有 usb_*/hid_* 成员，先前发过去等于丢掉）。这里走 cmd.sock 真下发。
    gadget_cfg = {k: v for k, v in mouse.items() if k in USB_PROXY_CONFIG_KEYS}
    if gadget_cfg:
        applied, apply_error = _usbproxy_send_set_config(gadget_cfg, bool(body.get('apply_now')))
    else:
        applied, apply_error = False, '未提供任何 USB 身份字段，未下发配置'
    payload = _mouse_apply_payload(mouse, applied=applied, apply_error=apply_error)
    if detail:
        payload['_core_offline'] = True
        payload['_detail'] = detail
    # ok 取两个通道的合取：任一路没落实就不能报成功。
    both_ok = bool(ok and applied)
    return jsonify({'ok': both_ok, 'data': payload,
                    'error': '' if both_ok else (apply_error or detail)})


@app.put('/api/hardware/mouse/mode')
def update_mouse_proxy_mode():
    body = request.get_json(silent=True) or {}
    mode = str(body.get('mode') or '').strip()
    if not mode:
        return jsonify({'ok': False, 'error': 'mode is required'})
    prof = _get_runtime_profile()
    mouse = prof.get('mouse') or {}
    mouse['mode'] = mode
    mouse['proxy_mode'] = mode
    prof['mouse'] = mouse
    ok, detail = _mouse_save_or_ipc(prof, mouse)
    # 透传模式的真源是 systemd 单元的 Environment=USB_PROXY_MODE，改它要 root +
    # daemon-reload，web（ttbox 身份）做不到。旧实现把这个写进 profile 就报成功，
    # 而单元里的模式从未变过 —— 典型的假落实。如实报失败，并告知当前真实模式。
    mode_error = ('切换 USB 透传模式需修改 systemd 单元的 USB_PROXY_MODE 并以 root '
                  '重载服务，Web 无此权限；请在板端运维通道执行')
    payload = _mouse_apply_payload(mouse, applied=False, apply_error=mode_error)
    if detail:
        payload['_core_offline'] = True
        payload['_detail'] = detail
    return jsonify({'ok': False, 'error': mode_error, 'data': payload})


def _display_mode_entry(token, label, w, h, refresh, pc_khz):
    """保持 Web 契约 advertised/available modes 条目结构（含 source + hdmi_raw_gbps）。"""
    hdmi_raw_gbps = round(pc_khz * 30 / 1e6, 5)  # 保持 Web 契约：pc_khz*30/1e6
    return {
        'token': token,
        'label': label,
        'width': w, 'height': h, 'refresh': refresh,
        'pixel_clock_khz': pc_khz,
        'source': 'safe',
        'hdmi_raw_gbps': hdmi_raw_gbps,
    }


@app.get('/api/hardware/display')
def get_display_hardware():
    # 缓存 3 秒：v4l2-ctl query-dv-timing 在信号重协商时阻塞，防止 waitress 线程耗尽
    now = time.time()
    if _DISPLAY_CACHE['data'] is not None and now - _DISPLAY_CACHE['ts'] < 3:
        return jsonify({'ok': True, 'data': _DISPLAY_CACHE['data']})
    hdmi = {'connected': False, 'locked': False, 'width': 0, 'height': 0, 'refresh': 0}
    try:
        r = subprocess.run(
            ['v4l2-ctl', '-d', '/dev/video0', '--query-dv-timing'],
            capture_output=True, text=True, timeout=1.5,
        )
        txt = r.stdout
        if r.returncode == 0 and 'Active width' in txt:
            w = re.search(r'Active width:\s*(\d+)', txt)
            h = re.search(r'Active height:\s*(\d+)', txt)
            fps = re.search(r'\(([\d.]+) frames per second\)', txt)
            hdmi['connected'] = True
            hdmi['locked'] = True
            if w: hdmi['width'] = int(w.group(1))
            if h: hdmi['height'] = int(h.group(1))
            if fps: hdmi['refresh'] = float(fps.group(1))
    except Exception:
        pass
    # Web 兼容结构：前端 populateDisplayHardware 消费 available/config/display_mode
    cfg_disp = {}
    cpath = ttbox_paths.config_dir() + '/hardware_display.json'
    try:
        if os.path.exists(cpath):
            cfg_disp = json.load(open(cpath))
    except Exception:
        pass
    # 广播模式（当前 EDID 生效的模式，同 available_modes 结构）
    advertised = []
    try:
        out = subprocess.check_output(
            [TTBOX_HDMIRX_EDID, '--list'],
            text=True, timeout=5)
        in_modes = False
        for lm in out.splitlines():
            lm = lm.strip()
            if lm.startswith('Modes:'):
                in_modes = True
                continue
            if in_modes:
                if not lm:
                    break
                parts = lm.split()
                if not parts:
                    continue
                token = parts[0]
                dims = re.search(r'(\d+)x(\d+)@(\d+)', lm)
                pc = re.search(r'pixel_clock=(\d+)', lm)
                advertised.append(_display_mode_entry(
                    token,
                    f'{dims.group(1)}x{dims.group(2)}@{dims.group(3)}' if dims else token,
                    int(dims.group(1)) if dims else 0,
                    int(dims.group(2)) if dims else 0,
                    int(dims.group(3)) if dims else 0,
                    int(pc.group(1)) if pc else 0,
                ))
    except Exception:
        pass
    # available_modes（Web 结构：token/label/width/height/refresh/pixel_clock_khz）
    available_modes = []
    try:
        out2 = subprocess.check_output(
            [TTBOX_HDMIRX_EDID, '--list'],
            text=True, timeout=5)
        in_modes = False
        for lm in out2.splitlines():
            lm = lm.strip()
            if lm.startswith('Modes:'):
                in_modes = True
                continue
            if in_modes:
                if not lm:
                    in_modes = False
                    continue
                parts = lm.split()
                if not parts:
                    continue
                token = parts[0]
                dims = re.search(r'(\d+)x(\d+)@(\d+)', lm)
                pc = re.search(r'pixel_clock=(\d+)', lm)
                available_modes.append(_display_mode_entry(
                    token,
                    f'{dims.group(1)}x{dims.group(2)}@{dims.group(3)}' if dims else token,
                    int(dims.group(1)) if dims else 0,
                    int(dims.group(2)) if dims else 0,
                    int(dims.group(3)) if dims else 0,
                    int(pc.group(1)) if pc else 0,
                ))
    except Exception:
        pass
    data = dict(hdmi)
    data['available'] = hdmi.get('connected', False)
    data['config'] = cfg_disp
    # monitor 模块：真实 hdmirx RX 状态（独立于板端其它服务）
    # V-07 收口：scripts 目录已在文件头经 ttbox_paths.scripts_dir() append 进 sys.path
    # （A-PATH-3 相对派生）；此处**不再**以绝对路径 insert(0, …) 注入 ——
    # 绝对路径注入既散落字面量（A-PATH-3/5 违背），又有 insert(0) 遮蔽 stdlib 的风险。
    try:
        from edid.monitor import read_hdmirx_status
        rx = read_hdmirx_status()
        data['hdmirx'] = rx
        if not data['available'] and rx.get('connected'):
            data['available'] = True
    except Exception:
        pass
    status_text = ''
    # 真实显示器身份：从当前生效 EDID 读取（hdmirx_edid --status）
    edid_name, edid_vendor, edid_pid, edid_serial = '', '', '', ''
    edid_valid = False
    status_text = ''
    try:
        out = subprocess.check_output(
            [TTBOX_HDMIRX_EDID, '--status'],
            text=True, timeout=5)
        status_text = out
        nm = re.search(r'name=(\S+)', out)
        vd = re.search(r'vendor=(\S+)', out)
        pid = re.search(r'product=(0x[0-9a-fA-F]+)', out)
        ser = re.search(r'serial=(0x[0-9a-fA-F]+)', out)
        if nm:
            edid_name = nm.group(1)
            edid_vendor = vd.group(1) if vd else ''
            edid_pid = pid.group(1) if pid else ''
            edid_serial = ser.group(1) if ser else ''
            edid_valid = True
    except Exception:
        pass
    data['status'] = {'output': status_text or 'EDID 状态读取失败'}
    data['loopout'] = _loopout_payload()
    data['display_mode'] = {
        'loopout_enabled': bool(cfg_disp.get('loopout_enabled', False)),
        'real_monitor': {
            'connected': hdmi.get('connected', False),
            'width': hdmi.get('width', 0), 'height': hdmi.get('height', 0),
            'refresh': hdmi.get('refresh', 0),
            'name': edid_name or cfg_disp.get('name', ''),
            'vendor': edid_vendor or cfg_disp.get('vendor', ''),
            'product_id': edid_pid or cfg_disp.get('product_id', ''),
            'serial': edid_serial or cfg_disp.get('serial', ''),
            'edid_valid': edid_valid,
        },
        'advertised_modes': advertised[:16],
        'available_modes': available_modes,
    }
    _DISPLAY_CACHE['ts'] = time.time()
    _DISPLAY_CACHE['data'] = data
    return jsonify({'ok': True, 'data': data})


@app.put('/api/hardware/display')
def update_display_hardware():
    body = request.get_json(silent=True) or {}
    cfg_in = body.get('config') or {}
    apply_now = bool(body.get('apply'))
    if not cfg_in:
        return jsonify({'ok': False, 'error': '缺少 config'})
    cpath = ttbox_paths.config_dir() + '/hardware_display.json'
    try:
        cur = json.load(open(cpath)) if os.path.exists(cpath) else {}
    except Exception:
        cur = {}
    # 只合并白名单键（防注入）
    for k in ('device', 'name', 'vendor', 'product_id', 'serial',
              'native_mode', 'native_only', 'profile',
              'loopout_enabled', 'loopout_overlay_enabled',
              'loopout_pixel_format', 'loopout_overlay_thickness', 'loopout_overlay_color'):
        if k in cfg_in:
            # native_mode 保护：空/非法值不覆盖已有配置（防退化成 1080p60）
            if k == 'native_mode':
                v = str(cfg_in[k] or '').strip()
                if v and v != 'auto':
                    try:
                        # edid 包随 scripts 目录在标题头已 append 进 sys.path（A-PATH-3）
                        from edid.timing_db import mode_info
                        if mode_info(v) is None:
                            continue  # 非法 token，拒绝覆盖
                    except Exception:
                        continue
                else:
                    continue  # 空/auto 不覆盖
            cur[k] = cfg_in[k]
    # hardware_display.device 表示 HDMI-RX 输入设备，不能接受 DRM 输出节点。
    # auto/空值统一落为真实 V4L2 节点，避免界面显示与 EDID 实际注入路径漂移。
    requested_device = str(cur.get('device', 'auto') or 'auto').strip()
    if requested_device == 'auto' or not requested_device:
        cur['device'] = '/dev/video0'
    elif requested_device.startswith('/dev/dri/') or 'card' in requested_device or 'renderD' in requested_device:
        return jsonify({'ok': False, 'error': 'HDMI-RX 输入必须使用 /dev/video0，/dev/dri/card0 仅用于 loopout'}), 400
    json.dump(cur, open(cpath, 'w'), indent=2, ensure_ascii=False)

    result = {}
    if apply_now:
        apply_env = dict(os.environ)
        apply_env['TTBOX_EDID_REHANDSHAKE'] = '1'
        # V-09：不再覆写 TTBOX_EDID_REHANDSHAKE_ATTEMPTS —— 重试次数单一真源在
        # edid_apply.sh（★ 2026-09-28 起默认 2，此前 12；每次重试都要切一次 HPD = 源端黑屏一次），
        # Web 与手动路径必须同值（原 Web 私自设 6 ⇒ 行为不可预期）。
        r = subprocess.run(['bash', ttbox_paths.scripts_dir() + '/edid/edid_apply.sh'],
                           capture_output=True, text=True, timeout=60, env=apply_env)
        result = {'exit': r.returncode, 'output': (r.stdout + r.stderr).strip()[-500:]}
        if r.returncode != 0:
            return jsonify({'ok': False, 'error': f'EDID 应用失败: {r.stderr or r.stdout}'[-300:]})
        # 内核持久化（前端 patch_boot_image=true 时执行）
        if body.get('patch_boot_image'):
            pr = subprocess.run(
                ['bash', ttbox_paths.scripts_dir() + '/edid/edid_patch_boot_image.sh',
                 ttbox_paths.ttbox_prefix() + '/runtime/edid/current.bin'],
                capture_output=True, text=True, timeout=60)
            result['patch_boot_image'] = {
                'exit': pr.returncode,
                'output': (pr.stdout + pr.stderr).strip()[-300:],
            }
    # 返回 Web 兼容结构（前端 populateDisplayHardware 消费 display_mode/loopout）
    # 简化：直接返回 GET 的完整结构（含 real_monitor/available_modes/advertised）
    with app.test_request_context('/api/hardware/display'):
        pass
    gv = get_display_hardware()
    gv_data = gv.get_json().get('data', {})
    gv_data['config'] = cur
    gv_data['result'] = result
    gv_data['message'] = '显示器配置已应用'
    gv_data['loopout'] = {
        'enabled': bool(cur.get('loopout_enabled')),
        'overlay_enabled': bool(cur.get('loopout_overlay_enabled')),
        'pixel_format': cur.get('loopout_pixel_format', 'rgb888'),
        'width': 0, 'height': 0, 'refresh': 0,
        'overlay_status': '等待环出' if cur.get('loopout_overlay_enabled') else '',
    }
    return jsonify({'ok': True, 'data': gv_data})


# -- 激活/授权 --
def _license_payload() -> dict:
    """保持 Web 契约 license 结构：app_version/auto_start/core/device/license/ui/ui_brand/version。"""
    import glob as _glob
    # 真实设备指纹（保持 Web 契约 device）
    cpu_serial = ''
    try:
        with open('/proc/cpuinfo') as f:
            for line in f:
                if line.startswith('Serial'):
                    cpu_serial = line.split(':', 1)[1].strip()
                    break
    except Exception:
        pass
    macs = []
    try:
        for iface in ('enP3p49s0', 'eth0', 'end0', 'wlan0'):
            p = f'/sys/class/net/{iface}/address'
            if os.path.exists(p):
                macs.append(open(p).read().strip())
    except Exception:
        pass
    device_id = f'opi-{cpu_serial}' if cpu_serial else 'opi-ttbox-local'
    fingerprint = device_id
    # ---- T1.07b：license 子块 = core IPC 投影（单一真相源）----
    # 授权语义（activated/state/plan/is_pro/features/到期/诊断）**全部来自 core**，
    # Web 零推导；被删的伪造字段（本地 license_id / license_version / max_version /
    # 硬编码 issued_at 与"永不到期"expires_at / features 恒满配 / online_* 伪值）
    # 均无 IPC 来源 ⇒ **不得回填**（★ 注释里也不写这些字面量，否则 §4.2 全仓 grep 门禁
    # 会被自己的注释命中，门禁形同虚设）。
    # device_id / fingerprint 是真实设备读取（/proc/cpuinfo + /sys/class/net），非授权源，保留。
    # 与 /api/state 共用 _license_block()，杜绝两处投影分叉。
    license_data = dict(_license_block())
    license_data['device_id'] = device_id
    license_data['device_fingerprint_hash'] = fingerprint
    # ui 块 = 品牌表投影（唯一来源，与 /api/state 同源，杜绝两处分叉）。
    # S1-2026-09-18：无线/AP 热点已随无线页签整体移除，default_hotspot_ssid 仅剩品牌表展示字段。
    ui = _ui_block(license_data.get('ui_brand'))
    core_version = '2026.05.16'
    # 与 /api/state 同源：对外可见版本 = 系统部署版本（current 软链名），非 web 侧构建常量。
    app_version = _ota_current_version() or kAppVersion
    return {
        'app_version': app_version,
        'auto_start': _auto_start_payload(),
        'core': _core_state_payload(),
        'device': {
            'binding_hardware': {
                'board_mac_addresses': macs,
                'cpu': {'Serial': cpu_serial},
                'schema': 'orangepi-board-v4',
            },
            'device_id': device_id,
            'fingerprint_hash': fingerprint,
            'hardware': {'Serial': cpu_serial},
        },
        'license': license_data,
        'ui': ui,
        'ui_brand': ui['ui_brand'],
        'version': app_version,
        # ★ M2.07：云端字段增量（§3.3）—— expire_at 北京时间串 / 解绑余量 /
        #   心跳在线状态 / machine_code。core 投影（上方 license 子块）零改动。
        'cloud': _cloud_license_subblock(),
    }


@app.get('/api/license')
def get_license():
    return jsonify({'ok': True, 'data': _license_payload()})


@app.post('/api/license/activate')
def activate_license():
    """M2.07 激活端点（主入口 = 云端卡密 card-login；语义重接，§2.4）。

    链路（§4.1）：入参校验 → 并发锁 → CloudLicenseClient.card_login（HMAC 四头）
    → 成功落 cloud_session.json → IPC ACTIVATE_CLOUD 交 core（唯一落盘/执法点）
    → 心跳线程拉起 → 返回 license 全量投影。
    错误语义：云端 error 原文透传（§7.1，激活页直接展示）。
    判据顺序：400（入参）> 云端 400/403（原样透传）> 502（core 不可达/网络）。

    ★ D7 存量兼容：离线卡 JSON 信封（`{` 开头）保留旧 ACTIVATE_LICENSE 路径
      （Ed25519 验签，core 能力未废），但不再是主入口。
    """
    body = request.get_json(silent=True) or {}
    license_key = str(body.get('license_key') or '').strip()
    if not license_key:
        return jsonify({'ok': False, 'error': 'license_key is required'}), 400

    # ---- 存量离线卡路径（保留不调用为主；仅信封原文显式提交时走）----
    if license_key.lstrip().startswith('{'):
        resp = ipc_request('ACTIVATE_LICENSE', {'card': license_key}, timeout=10)
        if not isinstance(resp, dict) or resp.get('status') != 0:
            err = resp.get('error') if isinstance(resp, dict) else None
            return jsonify({'ok': False,
                            'error': f'激活被拒绝：{err or "IPC 无响应（Core 未运行?）"}'}), 400
        data = resp.get('data') or {}
        _invalidate_activation_cache()
        return jsonify({'ok': True, 'data': {
            'state': data.get('state', 'active'),
            'activated': bool(data.get('activated', True)),
            'plan': data.get('plan', 'none'),
            'is_pro': bool(data.get('is_pro', False)),
            'ui_brand': data.get('ui_brand', 'ttbox'),
            'features': list(data.get('features') or []),
            'expire_unix_ms': data.get('expire_unix_ms', 0),
            'license': _license_payload(),
        }})

    # ---- 云端 card-login（主入口）----
    if not _ACTIVATION_LOCK.acquire(blocking=False):
        return jsonify({'ok': False, 'error': '激活进行中，请稍候'}), 409
    try:
        machine_code = _machine_code()
        if not machine_code:
            return jsonify({'ok': False, 'error': '无法读取设备指纹（cpu_serial）'}), 500
        try:
            login = _CLOUD_CLIENT.card_login(license_key, machine_code)
        except CloudLicenseError as e:
            # 错误语义原样透传；HTTP 映射：400→400、401/403→403、网络/其它→502
            if e.status == 400:
                http_status = 400
            elif e.status in (401, 403):
                http_status = 403
            else:
                http_status = 502
            return jsonify({'ok': False, 'error': e.message or e.code}), http_status

        # 云端验证成功 ⇒ 会话落盘（0600 原子写；D8）
        expire_unix_ms = int(login['expire_unix_s']) * 1000
        session_data = {
            'card_key': license_key,
            'card_mask': card_mask(license_key),
            'client_token': login['client_token'],
            'expire_at': login['expire_at'],
            'expire_unix_ms': expire_unix_ms,
            'max_devices': int(login['max_devices']),
            'heartbeat_interval': int(login['heartbeat_interval']),
            'heartbeat_timeout': int(login['heartbeat_timeout']),
        }
        if not _CLOUD_SESSION.save(session_data):
            return jsonify({'ok': False, 'error': '云端验证成功，但会话落盘失败'}), 500

        # core ACTIVATE_CLOUD（D2：core 是唯一落盘/执法点，web 不直写 LicenseStore）
        params = {
            'expire_unix_ms': expire_unix_ms,
            'features': list(_CLOUD_FEATURES_FULL),
            'plan': 'subscription',
            'card_mask': card_mask(license_key),
            'max_devices': int(login['max_devices']),
            'source': 'cloud',
        }
        resp = ipc_request('ACTIVATE_CLOUD', params, timeout=10)
        if not isinstance(resp, dict) or resp.get('status') != 0:
            err = resp.get('error') if isinstance(resp, dict) else None
            _CLOUD_SESSION.clear()
            return jsonify({'ok': False,
                            'error': f'云端验证成功但核心激活失败：{err or "IPC 无响应（Core 未运行?）"}'}), 502

        _invalidate_activation_cache()
        _ensure_heartbeat_worker()
        data = resp.get('data') or {}
        return jsonify({'ok': True, 'data': {
            'state': data.get('state', 'valid'),
            'activated': bool(data.get('activated', True)),
            'plan': data.get('plan', 'subscription'),
            'is_pro': bool(data.get('is_pro', False)),
            'ui_brand': data.get('ui_brand', 'ttbox'),
            'features': list(data.get('features') or []),
            'expire_unix_ms': data.get('expire_unix_ms', expire_unix_ms),
            'license': _license_payload(),
        }})
    finally:
        _ACTIVATION_LOCK.release()


@app.post('/api/activation/reset-local-identity')
def reset_activation_local_identity():
    # ★ T1.07b：旧形**无条件**声称授权"正常"（未激活时也这么答 ⇒ 掩盖真实状态）。
    #   改为读真状态：已激活 ⇒ 保留"正常、拒绝破坏性操作"语义（400）；
    #   未激活 ⇒ 不得称"正常"，如实报 + 明牌本地修复下沉 T2.x（409）。
    lic = _license_block()
    if lic.get('activated'):
        return jsonify({'ok': False, 'error': '当前授权状态正常，已拒绝重置本地授权身份'}), 400
    return jsonify({'ok': False,
                    'error': f"授权未激活（state={lic.get('state')}）；本地重置下沉 T2.x"}), 409


@app.get('/api/activation/full-recovery')
def get_activation_full_recovery():
    # ★ T1.07b：旧形 `allowed`/`available` **恒 false** 且 reason 恒称"状态正常"
    #   （未激活也这么答 ⇒ 掩盖真实状态）。改为据真状态给值（不再恒 false）：
    #   · `allowed`  = **授权侧是否阻止**恢复：已激活 ⇒ 阻止（保护授权身份）；未激活 ⇒ 不阻止。
    #   · `available`= **本版本是否真有实现**：M1 无本地全量恢复实现（下沉 T2.x）⇒ **恒 false**。
    #     两者分岔是刻意的 —— 不许用 `allowed=true` 冒充"现在就能恢复"（假绿禁）。
    lic = _license_block()
    if lic.get('activated'):
        allowed = False
        reason = '当前授权和 daemon 状态正常，已拒绝本地全量恢复'
    else:
        allowed = True
        reason = f"授权未激活（state={lic.get('state')}）；授权侧不阻止恢复，但本地全量恢复下沉 T2.x"
    return jsonify({'ok': True, 'data': {
        'allowed': allowed,
        'available': False,
        'reason': reason,
        'saved_at': '',
        'size': 0,
        'source': '',
        'version': '',
    }})


@app.post('/api/activation/full-recovery')
def start_activation_full_recovery():
    # ★ T1.07b：旧形**无条件**称"授权正常"。读真状态后再答：
    #   已激活 ⇒ 保留"拒绝破坏性操作"语义（400）；未激活 ⇒ 不称"正常"，如实报（409）。
    lic = _license_block()
    if lic.get('activated'):
        return jsonify({'ok': False, 'error': '当前授权和 daemon 状态正常，已拒绝本地全量恢复'}), 400
    return jsonify({'ok': False,
                    'error': f"授权未激活（state={lic.get('state')}）；本地全量恢复下沉 T2.x"}), 409


@app.get('/api/themes')
def get_themes():
    # 保持 Web 契约：内置 default 主题结构（标题用 TTBOX 品牌）
    return jsonify({'ok': True, 'data': {
        'active_theme_id': 'default',
        'active_version': '',
        'offline': False,
        'purchase_url': '',
        'themes': [{
            'active': True,
            'compatible': True,
            'description': '系统内置主题，始终可用。',
            'id': 'default',
            'installed': True,
            'installed_version': 'built-in',
            'latest_version': 'built-in',
            'owned': True,
            'previews': [],
            'published': True,
            'title': 'TTBOX 默认主题',
            'update_available': False,
        }],
    }})


@app.get('/api/themes/<theme_id>/previews/<int:index>')
def theme_preview(theme_id: str, index: int):
    return jsonify({'ok': True, 'data': {}})


@app.post('/api/themes/redeem')
def redeem_theme():
    body = request.get_json(silent=True) or {}
    # 保持 Web 契约：无主题卡密报错
    if not str(body.get('code') or body.get('redeem_code') or '').strip():
        return jsonify({'ok': False, 'error': '请输入主题卡密'})
    return jsonify({'ok': False, 'error': 'redeem failed: invalid code'})


@app.post('/api/themes/<theme_id>/install')
def install_theme(theme_id: str):
    body = request.get_json(silent=True) or {}
    # 保持 Web 契约：install 需要 core download url
    if not str(body.get('download_url') or '').strip():
        return jsonify({'ok': False, 'error': 'core download url is required'})
    if theme_id != 'default':
        return jsonify({'ok': False, 'error': f'theme not found: {theme_id}'})
    return jsonify({'ok': True, 'data': {'installed': True, 'theme_id': 'default'}})


@app.put('/api/themes/current')
def select_theme():
    body = request.get_json(silent=True) or {}
    theme_id = str(body.get('theme_id') or 'default')
    if theme_id != 'default':
        return jsonify({'ok': False, 'error': f'theme not found: {theme_id}'})
    # 保持 Web 契约：返回 active_theme_id + active_version
    return jsonify({'ok': True, 'data': {'active_theme_id': 'default', 'active_version': ''}})


@app.get('/theme-assets/<theme_id>/<version>/<path:filename>')
def theme_asset(theme_id: str, version: str, filename: str):
    return jsonify({'ok': True, 'data': {}})


def _branding_rejected():
    # 保持 Web 契约：网页背景仅对 TTBOX 授权系统开放
    return jsonify({'ok': False, 'error': '网页背景仅对 TTBOX 授权系统开放'})


@app.get('/api/branding/background')
def get_branding_background():
    return _branding_rejected()


@app.post('/api/branding/background')
def upload_branding_background():
    return _branding_rejected()


@app.patch('/api/branding/background')
def update_branding_background():
    return _branding_rejected()


@app.delete('/api/branding/background')
def delete_branding_background():
    return _branding_rejected()


@app.get('/api/branding/background/image')
def get_branding_background_image():
    return jsonify({'ok': True, 'data': {}})


# -- 运动训练 --
def _motion_error(exc: Exception):
    message = str(exc)
    status = 409 if "session" in message or "active" in message or "lease" in message else 422
    return jsonify({'ok': False, 'error': message}), status


def _apply_personal_motion_to_core(enabled: bool, profile_id: str = '', mix: dict | None = None):
    """把 TTBOX 个人模型的启用状态写入 Core RuntimeProfile，Core 是最终运行真源。"""
    # ★ 读-改-写持配置锁（防止与用户保存/标定恢复互相整份覆盖）。
    with _CFG_WRITE_LOCK:
        prof = _get_runtime_profile()
        if not prof:
            raise MotionTrainingError('读取 TTBOX Core RuntimeProfile 失败')
        personal = prof.setdefault('mouse', {}).setdefault('personal_motion', {})
        personal['enabled'] = bool(enabled)
        if enabled:
            profile = MOTION_STORE.list_profile(profile_id)
            model = profile.get('model') or {}
            if not model.get('ready'):
                raise MotionTrainingError('model is not ready')
            values = mix or MOTION_STORE._mix()
            personal.update({
                'curve_blend': values.get('curve', 1.0),
                'speed_blend': values.get('speed', 1.0),
                'reaction_blend': values.get('reaction', 0.7),
                'max_reaction_delay_ms': values.get('max_reaction_delay_ms', 250),
                'knots': model.get('knots', []),
            })
        result = ipc_request('SET_CONFIG', {'profile': prof})
    if result.get('status') != 0:
        raise MotionTrainingError(result.get('error', 'Core 配置更新失败'))
    return prof


@app.get('/api/motion-profiles')
def list_motion_profiles():
    try:
        return jsonify({'ok': True, 'data': MOTION_STORE.list_profiles()})
    except (MotionTrainingError, OSError) as exc:
        return _motion_error(exc)


@app.post('/api/motion-profiles')
def create_motion_profile():
    # 保持 Web 契约：只支持内置 default 档案，拒绝创建新档案
    return jsonify({'ok': False, 'error': 'only the internal default motion profile is supported'})


@app.patch('/api/motion-profiles/<profile_id>')
def rename_motion_profile(profile_id: str):
    body = request.get_json(silent=True) or {}
    try:
        return jsonify({'ok': True, 'data': MOTION_STORE.rename(profile_id, body.get('name', ''))})
    except (MotionTrainingError, OSError) as exc:
        return _motion_error(exc)


@app.delete('/api/motion-profiles/<profile_id>')
def delete_motion_profile(profile_id: str):
    try:
        return jsonify({'ok': True, 'data': MOTION_STORE.delete(profile_id)})
    except (MotionTrainingError, OSError) as exc:
        return _motion_error(exc)


@app.get('/api/motion-profiles/<profile_id>/export')
def export_motion_profile(profile_id: str):
    try:
        profile = MOTION_STORE.list_profile(profile_id)
        export_path = MOTION_PROFILES_DIR / f'.motion-profile-{profile_id}.json'
        export_path.write_text(json.dumps(profile, ensure_ascii=False, indent=2), encoding='utf-8')
        return send_file(export_path, mimetype='application/json', as_attachment=True,
                         download_name=f'ttbox-motion-profile-{profile_id}.json')
    except (MotionTrainingError, OSError) as exc:
        return _motion_error(exc)


@app.post('/api/motion-training/sessions')
def start_motion_training_session():
    body = request.get_json(silent=True) or {}
    try:
        result = MOTION_STORE.start_session(str(body.get('profile_id') or ''), now=time.time())
        return jsonify({'ok': True, 'data': {
            'session_id': result['id'], 'profile_id': result['profile_id'],
            'lease_expires_at': result['lease_expires_at'],
        }})
    except (MotionTrainingError, OSError) as exc:
        return _motion_error(exc)


@app.put('/api/motion-training/sessions/<session_id>/heartbeat')
def heartbeat_motion_training_session(session_id: str):
    try:
        return jsonify({'ok': True, 'data': MOTION_STORE.heartbeat(session_id)})
    except (MotionTrainingError, OSError) as exc:
        return _motion_error(exc)


@app.post('/api/motion-training/sessions/<session_id>/samples')
def append_motion_training_sample(session_id: str):
    body = request.get_json(silent=True)
    try:
        return jsonify({'ok': True, 'data': MOTION_STORE.append_sample(session_id, body)})
    except (MotionSampleError, MotionTrainingError, OSError) as exc:
        return _motion_error(exc)


@app.delete('/api/motion-training/sessions/<session_id>')
def stop_motion_training_session(session_id: str):
    try:
        return jsonify({'ok': True, 'data': MOTION_STORE.stop_session(session_id)})
    except (MotionTrainingError, OSError) as exc:
        return _motion_error(exc)


@app.post('/api/motion-profiles/<profile_id>/train')
def train_motion_profile(profile_id: str):
    try:
        return jsonify({'ok': True, 'data': MOTION_STORE.train(profile_id)})
    except (MotionTrainingError, OSError) as exc:
        return _motion_error(exc)


@app.post('/api/motion-profiles/<profile_id>/activate')
def activate_motion_profile(profile_id: str):
    body = request.get_json(silent=True) or {}
    try:
        result = MOTION_STORE.activate(profile_id, **body)
        _apply_personal_motion_to_core(True, profile_id, result['mix'])
        return jsonify({'ok': True, 'data': result})
    except (MotionTrainingError, OSError) as exc:
        return _motion_error(exc)


@app.delete('/api/motion-profiles/active')
def deactivate_motion_profile():
    try:
        result = MOTION_STORE.deactivate()
        _apply_personal_motion_to_core(False)
        return jsonify({'ok': True, 'data': result})
    except (MotionTrainingError, OSError) as exc:
        return _motion_error(exc)


@app.delete('/api/motion-profiles/<profile_id>/samples')
def clear_motion_profile_samples(profile_id: str):
    try:
        return jsonify({'ok': True, 'data': MOTION_STORE.clear_samples(profile_id)})
    except (MotionTrainingError, OSError) as exc:
        return _motion_error(exc)


# -- 远程 --
def _remote_not_ready():
    # 保持 Web 契约：未连接 Windows 电脑时提示输入局域网 IP
    return jsonify({
        'ok': False,
        'error': '请输入 Windows 电脑局域网 IP',
    })


@app.post('/api/remote/connect')
def remote_connect():
    return _remote_not_ready()


@app.get('/api/remote/models')
def remote_models():
    return _remote_not_ready()


@app.post('/api/remote/import')
def remote_import():
    return _remote_not_ready()


@app.post('/api/remote/delete')
def remote_delete():
    return _remote_not_ready()


# -- 预览 --
# 预览流健康监控：任何 MJPEG 连接收到帧数据即刷新 last_frame_ts。
# 前端 /api/state 轮询依据 preview.alive 判断预览流是否存活（服务重启/断线时自动重建连接）。
_PREVIEW_MONITOR = {"last_frame_ts": 0.0, "active_conns": 0}


@app.get('/api/preview.mjpg')
def preview_stream():
    preview_url = os.environ.get('TTBOX_PREVIEW_URL', '').rstrip('/')
    # Socket 级 streaming proxy：直接透传 8001 的 MJPEG 字节流，
    # 不经 urllib 缓冲（urllib Response 包装引入 26% 帧率损耗 + 300ms 卡顿）。
    if preview_url:
        from urllib.parse import urlparse
        parsed = urlparse(preview_url)
        upstream_host = parsed.hostname or '127.0.0.1'
        upstream_port = parsed.port or 8001

        def proxy_stream():
            _PREVIEW_MONITOR["active_conns"] += 1
            try:
                while True:
                    try:
                        upstream = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
                        upstream.settimeout(5)
                        upstream.connect((upstream_host, upstream_port))
                        req_line = 'GET /api/preview.mjpg HTTP/1.1\r\nHost: {}:{}\r\n\r\n'.format(upstream_host, upstream_port)
                        upstream.sendall(req_line.encode())
                        # 剥掉上游 HTTP 响应头（读到第一个 CRLFCRLF），只透传 multipart body，
                        # 否则浏览器会在 multipart 流里收到嵌套的 HTTP 头而无法解析。
                        buf = b''
                        while b'\r\n\r\n' not in buf:
                            chunk = upstream.recv(4096)
                            if not chunk:
                                break
                            buf += chunk
                        if b'\r\n\r\n' in buf:
                            body = buf.split(b'\r\n\r\n', 1)[1]
                            if body:
                                _PREVIEW_MONITOR["last_frame_ts"] = time.time()
                                yield body
                        # 后续字节是纯 multipart 流，直接透传
                        while True:
                            chunk = upstream.recv(65536)
                            if not chunk:
                                break
                            _PREVIEW_MONITOR["last_frame_ts"] = time.time()
                            yield chunk
                    except (OSError, ConnectionResetError):
                        pass
                    time.sleep(0.5)  # 断线重连间隔
            finally:
                _PREVIEW_MONITOR["active_conns"] -= 1

        return Response(
            proxy_stream(),
            mimetype='multipart/x-mixed-replace; boundary=ttboxframe',
            headers={'Cache-Control': 'no-store, no-cache', 'X-Accel-Buffering': 'no'},
        )

    # Fallback：直接读 Core IPC（无 8001 时）
    def generate():
        last_seq = -1
        while True:
            r = ipc_request('GET_PREVIEW', timeout=2)
            if r.get('status') == 0:
                d = r.get('data', {})
                b64 = d.get('jpeg_base64')
                seq = d.get('seq', 0)
                if b64 and seq != last_seq:
                    px = base64.b64decode(b64)
                    if px:
                        last_seq = seq
                        yield b'--ttboxframe' + b'\r\n'
                        yield b'Content-Type: image/jpeg' + b'\r\n'
                        yield b'Content-Length: ' + str(len(px)).encode() + b'\r\n\r\n'
                        yield px
                        yield b'\r\n'
            time.sleep(0.01)  # 10ms 轮询（Core 端 ~15fps 决定实际帧率）
    return Response(generate(), mimetype='multipart/x-mixed-replace; boundary=ttboxframe')


# ====================================================================
# 入口
# ====================================================================
def main():
    # ---- M2.07 启动序 ----
    # D10：web_credentials.json 机制退役（存在则改名 .retired；代码不再读它）。
    _retire_web_credentials()
    # D3：心跳归 web 进程 —— 有存量云端会话则恢复心跳对账（无会话时 worker
    #     空转轮询，激活成功后再由 _ensure_heartbeat_worker 幂等拉起）。
    _ensure_heartbeat_worker()
    from waitress import serve
    print(f'TTBOX Web 后端启动: http://{LISTEN_HOST}:{LISTEN_PORT}')
    print(f'  模板目录: {TEMPLATE_DIR}')
    print(f'  静态目录: {STATIC_DIR}')
    print(f'  IPC Socket: {IPC_SOCKET}')
    # waitress 默认 4 线程会被 MJPEG 长连接占满（每个预览流永久占 1 线程），
    # 导致 API 请求排队（queue depth 警告）、预览流卡顿（画面/检测框卡住）、
    # 配置保存无响应。
    #
    # threads 从 16 提到 64（2026-09-16）：实测出现过**整个服务无响应**——
    # 进程活着、端口在听，但连首页都 0 字节超时，只能重启。原因是线程被占满：
    # 设计器 /designer 里嵌了一整份面板（等于同时跑两份界面），加上用户自己
    # 开的标签页，每个标签页的轮询（状态/预览/事件）会长时间占住线程。
    # 16 个线程在这种并发下会被吃光，一旦占满，后续所有请求排队到超时。
    # 64 只是抬高天花板，真正治本要给轮询类接口加超时，属后续项。
    serve(app, host=LISTEN_HOST, port=LISTEN_PORT, threads=64)


if __name__ == '__main__':
    main()
