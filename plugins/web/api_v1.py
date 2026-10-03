"""TTBOX Web —— /api/v1 真接线 API 层。

设计原则（与旧 /api/* 最大的区别）：
  1. **只做转发，不编数据。** 每一个 "已实现" 端点都对应一条真实 Core IPC 命令，
     响应体就是 Core 返回的 data 原样透传，Web 层不做任何"看起来像真的"补全。
  2. **Core 没有的能力必须显式标注未实现。** 返回 HTTP 200 + ok:false +
     code:"NOT_IMPLEMENTED"，前端不会抛异常，但也不会拿到假数据。
  3. **能力清单 /api/v1/capabilities 是唯一真源**，前端的"未实现"角标由它渲染。

Core IPC 契约（以 core/src/ipc/IpcServer.cpp 为准，docs/ipc-protocol.md 已过时）：
  - 传输：Unix socket（TTBOX_IPC_SOCKET；默认路径见 plugins/web/lib/paths.py::IPC_SOCKET_DEFAULT），NDJSON 行协议。
  - **一连接一请求**：Core 的 handle_connection() 只读一行、回一行就关闭 fd，
    所以本模块每次调用都 connect → send(json+"\\n") → recv 一行 → close，
    绝不复用连接、绝不流水线（复用只会拿到对端已关闭的空响应）。
  - 请求：{"id"?:str,"type":str,"params"?:obj}
    响应：{"id":str,"type":str,"status":0..4,"data":{...},"error"?:str}
  - status: 0=OK 1=BAD_REQUEST 2=NOT_FOUND 3=INTERNAL 4=UNSUPPORTED
  - Core 侧没有超时，超时必须由客户端设置（见 TIMEOUTS）。

仅依赖标准库 + flask，不引入任何新第三方包。
"""
from __future__ import annotations

import base64
import binascii
import copy
import json
import os
import re
import socket
import sys
import threading
import time
import uuid
from typing import Any, Callable

from flask import Blueprint, Flask, Response, jsonify, redirect, render_template, request
from jinja2 import TemplateNotFound

api_v1 = Blueprint('api_v1', __name__, url_prefix='/api/v1')


# ====================================================================
# IPC 传输层
# ====================================================================
# IPC socket 唯一真源（A-PATH-5）：TTBOX_IPC_SOCKET > lib/paths.py 默认（与 ttbox-web.py 一致）。
from plugins.web.lib import paths as _ttbox_paths

IPC_SOCKET = _ttbox_paths.ipc_socket()
# Windows 本机开发用：TTBOX_IPC_TCP=127.0.0.1:9100（协议与 Unix socket 完全一致）。
IPC_TCP = os.environ.get('TTBOX_IPC_TCP', '')

# Core 单次接收上限 65536 字节，超过服务端直接判失败。
MAX_RECV = 65536

DEFAULT_TIMEOUT = 5.0

# ---- 超时分级（秒）----
# Core 侧没有超时保护，全部由客户端兜底。不同命令耗时差异极大：
#   - MODEL_ACTIVATE / MODEL_VALIDATE 要加载并跑通 RKNN 模型，板端实测可到分钟级，
#     给 5s 会在模型真的在加载时误判成"Core 挂了"，必须 >=120s。
#   - MODEL_INSTALL / MODEL_IMPORT 涉及文件拷贝与校验，给 60s。
#   - RUNTIME_CONTROL（尤其 restart）要销毁并重建整条流水线，给 30s。
#   - GET_PREVIEW 是"抓一帧"，拿不到就别等（前端会立刻再轮询），给 3s。
#   - 其余轻量查询（PING/GET_STATUS/GET_CONFIG/MODEL_LIST）5s 足够。
TIMEOUTS: dict[str, float] = {
    'PING': 3.0,
    'GET_PREVIEW': 3.0,
    'RUNTIME_CONTROL': 30.0,
    'SET_CONFIG': 10.0,
    'MODEL_IMPORT': 60.0,
    'MODEL_INSTALL': 60.0,
    'MODEL_ACTIVATE': 120.0,
    'MODEL_VALIDATE': 120.0,
}

# Core 对 model_id 的硬校验：^[A-Za-z0-9_-]{1,64}$，不匹配直接 status=1。
MODEL_ID_RE = re.compile(r'^[A-Za-z0-9_-]{1,64}$')

# Core status -> HTTP 状态码（1=BAD_REQUEST 2=NOT_FOUND 3=INTERNAL 4=UNSUPPORTED）
CORE_STATUS_HTTP = {1: 400, 2: 404, 3: 502, 4: 501}
CORE_STATUS_CODE = {1: 'BAD_REQUEST', 2: 'NOT_FOUND', 3: 'CORE_INTERNAL', 4: 'UNSUPPORTED'}

# 配置读-改-写（GET_CONFIG → 深合并 → SET_CONFIG）是三步非原子操作：
# 两个并发 PUT 各自 GET 到同一份 base，后提交的会把先提交的整棵覆盖（丢更新）。
# 本模块内用一把进程内锁把整段 RMW 串行化（与 ttbox-web.py 的 _CFG_WRITE_LOCK 各管各的模块）。
_CONFIG_RW_LOCK = threading.Lock()


def timeout_for(req_type: str) -> float:
    """返回指定命令的超时秒数（未列出的命令用 DEFAULT_TIMEOUT）。"""
    return float(TIMEOUTS.get(req_type, DEFAULT_TIMEOUT))


def ipc_call(req_type: str, params: dict | None = None, timeout: float | None = None) -> dict:
    """向 Core 发一次请求并返回完整响应信封。

    严格"一连接一请求"：connect → send(json + "\\n") → recv 到一行 → close。
    Core 的 handle_connection() 读一行、回一行就关闭 fd，因此复用连接或流水线
    只会读到 EOF（空响应），表现为"莫名其妙的 IPC 无响应"。

    返回：成功/失败都返回 dict。连不上、超时、解析失败时额外带 `unreachable:True`，
    调用方据此区分 "Core 不在"（503 CORE_UNREACHABLE）与 "Core 拒绝了请求"（4xx/5xx）。
    """
    payload: dict[str, Any] = {'id': uuid.uuid4().hex[:12], 'type': req_type}
    if params:
        payload['params'] = params
    tmo = float(timeout) if timeout else timeout_for(req_type)
    use_tcp = IPC_SOCKET.startswith('tcp:') or bool(IPC_TCP)

    try:
        if use_tcp:
            spec = IPC_TCP or IPC_SOCKET[len('tcp:'):]
            host, _, port = spec.rpartition(':')
            sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
            target: Any = (host or '127.0.0.1', int(port))
        else:
            # Windows 上 socket.AF_UNIX 不存在 -> AttributeError，落到 except 分支。
            sock = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
            target = IPC_SOCKET
    except (AttributeError, OSError, ValueError) as exc:
        return {'status': 3, 'unreachable': True,
                'error': f'IPC socket 创建失败（{"TCP" if use_tcp else "Unix"}）: {exc}'}

    sock.settimeout(tmo)
    try:
        sock.connect(target)
        sock.sendall((json.dumps(payload, ensure_ascii=False) + '\n').encode('utf-8'))
        buf = b''
        # NDJSON：读到换行即一帧完整响应；Core 单次接收上限 65536，不会被截断。
        while b'\n' not in buf:
            chunk = sock.recv(MAX_RECV)
            if not chunk:
                break
            buf += chunk
        if not buf.strip():
            return {'status': 3, 'unreachable': True, 'error': 'IPC 无响应（Core 未运行或对端已关闭连接）'}
        try:
            resp = json.loads(buf.decode('utf-8'))
        except (UnicodeDecodeError, json.JSONDecodeError) as exc:
            return {'status': 3, 'unreachable': True, 'error': f'IPC 响应不是合法 JSON: {exc}'}
        if not isinstance(resp, dict):
            return {'status': 3, 'unreachable': True, 'error': 'IPC 响应不是 JSON 对象'}
        return resp
    except socket.timeout:
        return {'status': 3, 'unreachable': True, 'error': f'IPC 响应超时（{tmo:g}s）'}
    except (FileNotFoundError, ConnectionRefusedError, ConnectionResetError, OSError) as exc:
        return {'status': 3, 'unreachable': True, 'error': f'无法连接 Core IPC（{_socket_desc()}）: {exc}'}
    finally:
        try:
            sock.close()
        except OSError:
            pass


def _socket_desc() -> str:
    """给人看的 socket 描述（用于错误信息，不泄漏任何敏感内容）。"""
    return f'tcp:{IPC_TCP}' if IPC_TCP else IPC_SOCKET


# ====================================================================
# 统一响应信封：{"ok":bool,"data":...,"error"?:str,"code"?:str}
# 与 static/apiClient.js 的约定保持一致。
# ====================================================================
def _ok(data: Any = None) -> Response:
    return jsonify({'ok': True, 'data': data if data is not None else {}})


def _err(error: str, code: str, http_status: int) -> tuple[Response, int]:
    return jsonify({'ok': False, 'data': None, 'error': error, 'code': code}), http_status


def _unreachable(error: str) -> tuple[Response, int]:
    """Core 不可达：统一 503 + CORE_UNREACHABLE。"""
    return _err(error, 'CORE_UNREACHABLE', 503)


def _from_core(resp: dict) -> tuple[Response, int]:
    """把 Core 响应信封翻译成 HTTP 响应（status!=0 原样透传 error）。"""
    status = resp.get('status', 3)
    if status == 0:
        return _ok(resp.get('data') if resp.get('data') is not None else {})
    return _err(
        str(resp.get('error') or f'Core 返回 status={status}'),
        CORE_STATUS_CODE.get(status, 'CORE_ERROR'),
        CORE_STATUS_HTTP.get(status, 502),
    )


def _forward(req_type: str, params: dict | None = None, timeout: float | None = None):
    """转发一条命令到 Core 并翻译成 HTTP 响应。"""
    resp = ipc_call(req_type, params, timeout)
    if resp.get('unreachable'):
        return _unreachable(str(resp.get('error') or 'Core 不可达'))
    return _from_core(resp)


def _json_body() -> dict:
    """取 JSON 请求体；非法/缺失一律返回空 dict（由各端点自行校验必填）。"""
    body = request.get_json(silent=True)
    return body if isinstance(body, dict) else {}


def _model_id_or_error() -> tuple[str, tuple[Response, int] | None]:
    """从请求体取并校验 model_id。返回 (model_id, error_response)。"""
    body = _json_body()
    model_id = str(body.get('model_id') or '').strip()
    if not model_id:
        return '', _err('缺少必填字段 model_id', 'BAD_REQUEST', 400)
    if not MODEL_ID_RE.match(model_id):
        # 提前拦下：Core 也会拒，但自己拦能给前端一条人话错误。
        return '', _err(f'model_id 非法（需匹配 [A-Za-z0-9_-]{{1,64}}）: {model_id!r}', 'BAD_REQUEST', 400)
    return model_id, None


# ====================================================================
# 已实现：真接线端点
# ====================================================================
@api_v1.get('/health')
def health():
    """GET /api/v1/health -> PING（同时作为 Core 连通性探针）。"""
    return _forward('PING')


@api_v1.get('/status')
def status():
    """GET /api/v1/status -> GET_STATUS（含 metrics 全量指标，原样透传）。"""
    return _forward('GET_STATUS')


@api_v1.get('/config')
def get_config():
    """GET /api/v1/config -> GET_CONFIG，原样返回 {runtime_profile, config_file}。"""
    return _forward('GET_CONFIG')


def _deep_merge(base: dict, patch: dict) -> dict:
    """递归合并（深合并），返回新 dict，不改动入参。"""
    merged = copy.deepcopy(base)
    for key, value in patch.items():
        if isinstance(value, dict) and isinstance(merged.get(key), dict):
            merged[key] = _deep_merge(merged[key], value)
        else:
            merged[key] = copy.deepcopy(value)
    return merged


@api_v1.put('/config')
def put_config():
    """PUT /api/v1/config -> SET_CONFIG。

    坑点：Core 的 SET_CONFIG 是 **全量替换** runtime_profile，而不是 merge ——
    提交时缺的键会被 Core 用内置缺省值补齐，也就是说"只改一个字段"会把其余
    字段悄悄重置成默认值。所以这里先 GET_CONFIG 取整棵当前配置，合并后再整棵提交。

    为什么用**深合并**而不是浅合并：调用方通常只提交一个子对象的部分字段
    （例如 {"capture": {"width": 1280}}）。浅合并会把整个 capture 子对象顶掉，
    同级的 height/fps/... 全部丢失并被缺省值覆盖。深合并只覆盖调用方显式给出的
    叶子键，其余原样保留。
    代价：深合并无法"删除"某个键（只能覆盖），这不是本 API 的使用场景。

    另一个坑：Core **禁止**通过 SET_CONFIG 修改 profile.model_id
    （"model_id 只能通过模型激活接口修改"），所以这里直接丢弃调用方传来的
    model_id —— 保留 base 里的值，改模型请走 POST /api/v1/models/activate。
    """
    body = _json_body()
    patch = body.get('profile')
    if patch is None:
        # 容忍调用方直接提交 profile 主体（不带 "profile" 包裹）。
        patch = body
    if not isinstance(patch, dict) or not patch:
        return _err('请求体需包含非空对象 profile（或直接提交 profile 主体）', 'BAD_REQUEST', 400)

    patch = copy.deepcopy(patch)
    patch.pop('model_id', None)

    # RMW 全程持锁：GET 到 SET 之间不允许第二个并发写插进来，否则丢更新。
    with _CONFIG_RW_LOCK:
        current = ipc_call('GET_CONFIG')
        if current.get('unreachable'):
            return _unreachable(str(current.get('error') or 'Core 不可达'))
        if current.get('status') != 0:
            return _from_core(current)

        data = current.get('data') if isinstance(current.get('data'), dict) else {}
        base = data.get('runtime_profile')
        if not isinstance(base, dict):
            base = {}

        merged = _deep_merge(base, patch)
        return _forward('SET_CONFIG', {'profile': merged})


@api_v1.post('/runtime/<action>')
def runtime_control(action: str):
    """POST /api/v1/runtime/<start|stop|restart> -> RUNTIME_CONTROL。"""
    if action not in ('start', 'stop', 'restart'):
        return _err(f'不支持的 action: {action}（可选 start|stop|restart）', 'BAD_REQUEST', 400)
    return _forward('RUNTIME_CONTROL', {'action': action})


@api_v1.get('/models')
def models_list():
    """GET /api/v1/models -> MODEL_LIST。"""
    return _forward('MODEL_LIST')


@api_v1.post('/models/activate')
def model_activate():
    """POST /api/v1/models/activate {"model_id"} -> MODEL_ACTIVATE（最长 120s）。"""
    model_id, error = _model_id_or_error()
    if error:
        return error
    return _forward('MODEL_ACTIVATE', {'model_id': model_id})


@api_v1.post('/models/validate')
def model_validate():
    """POST /api/v1/models/validate {"model_id"} -> MODEL_VALIDATE（最长 120s）。"""
    model_id, error = _model_id_or_error()
    if error:
        return error
    return _forward('MODEL_VALIDATE', {'model_id': model_id})


@api_v1.post('/models/install')
def model_install():
    """POST /api/v1/models/install {"model_id"} -> MODEL_INSTALL（最长 60s）。"""
    model_id, error = _model_id_or_error()
    if error:
        return error
    return _forward('MODEL_INSTALL', {'model_id': model_id})


@api_v1.post('/models/remove')
def model_remove():
    """POST /api/v1/models/remove {"model_id"} -> MODEL_REMOVE。"""
    model_id, error = _model_id_or_error()
    if error:
        return error
    return _forward('MODEL_REMOVE', {'model_id': model_id})


@api_v1.post('/models/set-concurrency')
def model_set_concurrency():
    """POST /api/v1/models/set-concurrency {"model_id","count"} -> MODEL_SET_CONCURRENCY。

    count 取值 1~3（Core 侧接受数字或数字字符串）。
    """
    model_id, error = _model_id_or_error()
    if error:
        return error
    raw = _json_body().get('count')
    try:
        count = int(str(raw).strip())
    except (TypeError, ValueError):
        return _err(f'count 必须是 1~3 的整数，收到: {raw!r}', 'BAD_REQUEST', 400)
    if count < 1 or count > 3:
        return _err(f'count 超出允许范围（1~3）: {count}', 'BAD_REQUEST', 400)
    return _forward('MODEL_SET_CONCURRENCY', {'model_id': model_id, 'count': count})


@api_v1.post('/models/import')
def model_import():
    """POST /api/v1/models/import -> MODEL_IMPORT。

    必填：src_path（Core 侧可见的绝对路径）、model_id（[A-Za-z0-9_-]{1,64}）
    可选：label、source_format（rknn 默认 | onnx）、sha256
    """
    body = _json_body()
    src_path = str(body.get('src_path') or '').strip()
    model_id = str(body.get('model_id') or '').strip()
    if not src_path:
        return _err('缺少必填字段 src_path（Core 侧可见的模型文件绝对路径）', 'BAD_REQUEST', 400)
    if not model_id:
        return _err('缺少必填字段 model_id', 'BAD_REQUEST', 400)
    if not MODEL_ID_RE.match(model_id):
        return _err(f'model_id 非法（需匹配 [A-Za-z0-9_-]{{1,64}}）: {model_id!r}', 'BAD_REQUEST', 400)

    params: dict[str, Any] = {'src_path': src_path, 'model_id': model_id}
    label = str(body.get('label') or '').strip()
    if label:
        params['label'] = label
    source_format = str(body.get('source_format') or '').strip().lower()
    if source_format:
        if source_format not in ('rknn', 'onnx'):
            return _err(f'source_format 只支持 rknn|onnx，收到: {source_format!r}', 'BAD_REQUEST', 400)
        params['source_format'] = source_format
    sha256 = str(body.get('sha256') or '').strip()
    if sha256:
        params['sha256'] = sha256
    return _forward('MODEL_IMPORT', params)


@api_v1.get('/preview.jpg')
def preview_jpeg():
    """GET /api/v1/preview.jpg -> GET_PREVIEW，base64 解码后以 image/jpeg 输出。

    无帧（Core 返回 status=2）时返回 503 + NO_PREVIEW_FRAME —— 前端可据此显示
    "等待画面"而不要把上一帧当实时画面。
    """
    resp = ipc_call('GET_PREVIEW')
    if resp.get('unreachable'):
        return _unreachable(str(resp.get('error') or 'Core 不可达'))
    if resp.get('status') != 0:
        if resp.get('status') == 2:
            return _err('当前没有可用的预览帧（运行时未启动或首帧尚未产出）', 'NO_PREVIEW_FRAME', 503)
        return _from_core(resp)

    data = resp.get('data') if isinstance(resp.get('data'), dict) else {}
    b64 = data.get('jpeg_base64')
    if not b64:
        return _err('Core 未返回 jpeg_base64', 'NO_PREVIEW_FRAME', 503)
    try:
        payload = base64.b64decode(b64, validate=False)
    except (binascii.Error, ValueError) as exc:
        return _err(f'预览帧 base64 解码失败: {exc}', 'CORE_INTERNAL', 502)
    if not payload:
        return _err('预览帧为空', 'NO_PREVIEW_FRAME', 503)

    return Response(
        payload,
        mimetype='image/jpeg',
        headers={'Cache-Control': 'no-store, no-cache, must-revalidate', 'X-Preview-Seq': str(data.get('seq', ''))},
    )


@api_v1.get('/preview.mjpg')
def preview_mjpeg():
    """GET /api/v1/preview.mjpg —— 轮询 Core GET_PREVIEW 拼成 MJPEG 流。

    这是真实数据（每帧都来自 Core 的 GET_PREVIEW），不是占位图；
    Core 不提供推流通道，只能靠短间隔轮询，帧率受 Core 编码能力限制。
    """
    fps = 10.0
    try:
        raw_fps = request.args.get('fps', '')
        if raw_fps:
            fps = max(1.0, min(30.0, float(raw_fps)))
    except ValueError:
        fps = 10.0
    interval = 1.0 / fps
    boundary = 'ttboxframe'

    def generate():
        last_seq: Any = None
        while True:
            resp = ipc_call('GET_PREVIEW')
            if not resp.get('unreachable') and resp.get('status') == 0:
                data = resp.get('data') if isinstance(resp.get('data'), dict) else {}
                b64 = data.get('jpeg_base64')
                seq = data.get('seq')
                if b64 and seq != last_seq:
                    try:
                        payload = base64.b64decode(b64, validate=False)
                    except (binascii.Error, ValueError):
                        payload = b''
                    if payload:
                        last_seq = seq
                        yield (f'--{boundary}\r\n'
                               f'Content-Type: image/jpeg\r\n'
                               f'Content-Length: {len(payload)}\r\n\r\n').encode('utf-8')
                        yield payload
                        yield b'\r\n'
            time.sleep(interval)

    return Response(
        generate(),
        mimetype=f'multipart/x-mixed-replace; boundary={boundary}',
        headers={'Cache-Control': 'no-store, no-cache', 'X-Accel-Buffering': 'no'},
    )


# ====================================================================
# 未实现：Core 根本没有这些能力 —— 必须如实标注，严禁造假数据
# ====================================================================
# 每项：path / methods / label（中文名）/ reason（人话说明）
NOT_IMPLEMENTED_SPECS: list[dict[str, Any]] = [
    # ---- 系统 ----
    {'path': '/system/version', 'methods': ['GET'], 'label': '系统版本',
     'reason': 'Core 未提供版本查询命令（GET_STATUS 内含 version 字段，请改用 /api/v1/status）'},
    {'path': '/system/storage', 'methods': ['GET'], 'label': '存储空间',
     'reason': 'Core 未提供磁盘/存储查询命令，Web 层不臆造容量数据'},
    {'path': '/system/reboot', 'methods': ['POST'], 'label': '重启设备',
     'reason': 'Core 未提供重启命令（不存在 SHUTDOWN/RELOAD），需由宿主 systemd 提供，计划中'},
    {'path': '/system/poweroff', 'methods': ['POST'], 'label': '关机',
     'reason': 'Core 未提供关机命令，需由宿主 systemd 提供，计划中'},
    {'path': '/system/hostname', 'methods': ['GET', 'PUT'], 'label': '主机名',
     'reason': 'Core 未提供主机名读写命令，计划中'},
    # ---- 预设 ----
    {'path': '/presets', 'methods': ['GET', 'POST'], 'label': '参数预设',
     'reason': '预设是 Web 侧概念，Core IPC 无对应命令；当前未落地持久化方案'},
    {'path': '/presets/load', 'methods': ['POST'], 'label': '加载预设',
     'reason': '预设功能 Core 未提供，计划中'},
    {'path': '/presets/delete', 'methods': ['POST'], 'label': '删除预设',
     'reason': '预设功能 Core 未提供，计划中'},
    {'path': '/presets/<name>', 'methods': ['GET', 'PUT', 'DELETE'], 'label': '单个预设',
     'reason': '预设功能 Core 未提供，计划中'},
    # ---- 硬件 ----
    {'path': '/hardware', 'methods': ['GET'], 'label': '硬件总览',
     'reason': 'Core 未提供 GET_DEVICE/DEVICE_INFO 之类的硬件枚举命令'},
    {'path': '/hardware/display', 'methods': ['GET', 'PUT'], 'label': '显示输出',
     'reason': 'Core 未提供 SET_OUTPUT 等显示控制命令，计划中'},
    {'path': '/hardware/mouse', 'methods': ['GET', 'PUT'], 'label': '鼠标/串口设备',
     'reason': 'Core 未提供设备枚举与控制命令，鼠标参数请通过 /api/v1/config 的 runtime_profile 读写'},
    # ---- 网络 ----
    {'path': '/network/wifi', 'methods': ['GET'], 'label': 'WiFi 状态',
     'reason': 'Core 未提供网络命令，WiFi 由宿主 NetworkManager 管理，计划中'},
    {'path': '/network/wifi/scan', 'methods': ['POST'], 'label': 'WiFi 扫描',
     'reason': 'Core 未提供网络命令，计划中'},
    {'path': '/network/wifi/connect', 'methods': ['POST'], 'label': 'WiFi 连接',
     'reason': 'Core 未提供网络命令，计划中'},
    {'path': '/network/wifi/ap/apply', 'methods': ['POST'], 'label': 'AP 热点',
     'reason': 'Core 未提供网络命令，计划中'},
    {'path': '/network/wifi/client/activate', 'methods': ['POST'], 'label': '客户端联网',
     'reason': 'Core 未提供网络命令，计划中'},
    # ---- 授权 ----
    {'path': '/license', 'methods': ['GET'], 'label': '授权信息',
     'reason': 'Core 未提供 LICENSE_* 命令，授权体系计划中'},
    # ---- 设备枚举 ----
    {'path': '/devices', 'methods': ['GET'], 'label': '设备总览',
     'reason': 'Core 未提供设备枚举命令，计划中'},
    {'path': '/devices/<path:sub>', 'methods': ['GET'], 'label': '设备枚举',
     'reason': 'Core 未提供设备枚举命令，计划中'},
    {'path': '/makcu/devices', 'methods': ['GET'], 'label': 'Makcu 设备',
     'reason': 'Core 未提供设备枚举命令，计划中'},
    {'path': '/ferrum/devices', 'methods': ['GET'], 'label': 'Ferrum 设备',
     'reason': 'Core 未提供设备枚举命令，计划中'},
    {'path': '/kmboxb/devices', 'methods': ['GET'], 'label': 'KMBox 设备',
     'reason': 'Core 未提供设备枚举命令，计划中'},
    # ---- 诊断 ----
    {'path': '/diagnostics', 'methods': ['GET'], 'label': '诊断总览',
     'reason': 'Core 未提供诊断命令（诊断指标请读 /api/v1/status 的 metrics），计划中'},
    {'path': '/diagnostics/<path:sub>', 'methods': ['GET', 'POST'], 'label': '诊断工具',
     'reason': 'Core 未提供诊断命令，计划中'},
]


def _make_not_implemented_view(spec: dict[str, Any]) -> Callable[..., Response]:
    """生成"未实现"视图：HTTP 200 + ok:false，前端不会抛异常但拿不到假数据。"""
    reason = spec['reason']
    label = spec['label']

    def _view(**_kwargs: Any) -> Response:
        return jsonify({
            'ok': False,
            'implemented': False,
            'code': 'NOT_IMPLEMENTED',
            'data': None,
            'error': f'{label}：{reason}',
        }), 200

    return _view


def _register_not_implemented() -> None:
    """把 NOT_IMPLEMENTED_SPECS 注册成真实路由（这样前端调用会拿到结构化应答而非 404 HTML）。"""
    for index, spec in enumerate(NOT_IMPLEMENTED_SPECS):
        endpoint = f'ni_{index}'
        api_v1.add_url_rule(
            spec['path'],
            endpoint=endpoint,
            view_func=_make_not_implemented_view(spec),
            methods=list(spec['methods']),
        )


_register_not_implemented()


# ====================================================================
# 能力清单（前端"未实现"角标的唯一真源）
# ====================================================================
IMPLEMENTED_FEATURES: list[str] = ['health', 'status', 'config', 'runtime', 'models', 'preview']

IMPLEMENTED_PATHS: list[str] = [
    'GET  /api/v1/health',
    'GET  /api/v1/status',
    'GET  /api/v1/config',
    'PUT  /api/v1/config',
    'POST /api/v1/runtime/{start,stop,restart}',
    'GET  /api/v1/models',
    'POST /api/v1/models/activate',
    'POST /api/v1/models/validate',
    'POST /api/v1/models/install',
    'POST /api/v1/models/remove',
    'POST /api/v1/models/set-concurrency',
    'POST /api/v1/models/import',
    'GET  /api/v1/preview.jpg',
    'GET  /api/v1/preview.mjpg',
    'GET  /api/v1/capabilities',
]


def capabilities_payload() -> dict:
    """构造能力清单；core_connected 用一次真实 PING 实测，不靠猜。"""
    probe = ipc_call('PING')
    connected = (not probe.get('unreachable')) and probe.get('status') == 0
    error = None if connected else str(probe.get('error') or 'Core 不可达')

    not_implemented = [
        {
            'path': '/api/v1' + spec['path'],
            'label': spec['label'],
            'reason': spec['reason'],
            'methods': list(spec['methods']),
        }
        for spec in NOT_IMPLEMENTED_SPECS
    ]

    return {
        'core_connected': connected,
        'core_error': error,
        'ipc_socket': _socket_desc(),
        'implemented': list(IMPLEMENTED_FEATURES),
        'implemented_paths': list(IMPLEMENTED_PATHS),
        'not_implemented': not_implemented,
    }


@api_v1.get('/capabilities')
def capabilities():
    """GET /api/v1/capabilities —— 已实现/未实现清单 + Core 实测连通性。"""
    return _ok(capabilities_payload())


# ====================================================================
# 页面路由
# --------------------------------------------------------------------
# 这里刻意不注册任何页面路由。
#
# 产品决策：原本挂在这里的自研控制台（GET /console → templates/console.html，
# 配套 static/console.js / console.css）已被用户否定，路由已删除；其模板与静态资源
# 当时归档到 static/legacy/，2026-09-24 面板收敛时该归档目录一并删除
# （740K 死资产；回滚见 .workbuddy/artifacts/archive/legacy-static-2026-09-24/ 或 git 历史）。
# 主面板 '/' 始终由 ttbox-web.py 的 index() 提供（templates/index.html），
# 保持"只有一套 UI"的默认形态，避免两套界面互相劫持入口。
# ====================================================================


# ====================================================================
# 界面设计器：/designer 页面 + 自定义 CSS 的读写
# --------------------------------------------------------------------
# 用户在浏览器里点选元素改样式，CSS 落到 UI_CUSTOM_PATH 这个文件；
# A0-3f（2026-09-18 订正）：现役 index.html 是自包含单文件，并不加载 /ui-custom.css
# （原注释"index.html 最后加载"已过时）。本文件已移出出货包，注释随事实修正。
# 刻意不放 plugins/web/static/：那个目录会随版本整体覆盖，
# 放 config 目录可以跨部署保留用户的手工调整。
# ====================================================================
# 路径走 paths.py 单源（尊重 TTBOX_CONFIG_DIR），不再硬编码 /opt/ttbox/config；
# TTBOX_UI_CUSTOM_CSS 仍可整体覆盖。
UI_CUSTOM_PATH = os.environ.get(
    'TTBOX_UI_CUSTOM_CSS', _ttbox_paths.config_dir() + '/ui-custom.css')
UI_CUSTOM_MAX = 200_000  # 200KB 上限，防止误粘贴把磁盘写满

# 设计器开关（默认关闭）：TTBOX_ENABLE_DESIGNER=1 才放行编辑器页面与写操作。
# 与 TTBOX_ENABLE_CONSOLE 同一套写法，取值判定保持一致。
DESIGNER_ENV_FLAG = 'TTBOX_ENABLE_DESIGNER'


def designer_enabled() -> bool:
    """界面设计器是否放行。

    设计器页面会往 UI_CUSTOM_PATH 写任意 CSS，而 index.html 最后加载该文件，
    等于任何能访问 8000 端口的人都能改界面、甚至注入样式，此前是无鉴权默认开启的。
    现在默认关闭；保留 /ui-custom.css 读取（设备上的手工样式仍然生效），
    只把编辑器页面和 PUT/DELETE 写操作挡在开关后面。
    """
    return str(os.environ.get(DESIGNER_ENV_FLAG, '')).strip().lower() in ('1', 'true', 'yes', 'on')


def _read_ui_custom() -> str:
    try:
        with open(UI_CUSTOM_PATH, 'r', encoding='utf-8') as f:
            return f.read()
    except FileNotFoundError:
        return ''
    except OSError as exc:
        raise RuntimeError(f'读取失败: {exc}')


def _write_ui_custom(css: str) -> None:
    """原子写：先写临时文件再 rename，避免写一半断电把样式文件写坏。

    临时文件名带随机后缀：两个并发写共用固定 .tmp 名会互相踩（A rename 后
    B 还在往已被挪走的 tmp 写）。0600：CSS 里可能带用户不愿公开的自定义内容。
    """
    path = UI_CUSTOM_PATH
    tmp = f'{path}.{uuid.uuid4().hex[:8]}.tmp'
    parent = os.path.dirname(path)
    if parent:
        os.makedirs(parent, exist_ok=True)
    try:
        with open(tmp, 'w', encoding='utf-8') as f:
            f.write(css)
            f.flush()
            os.fsync(f.fileno())
        try:
            os.chmod(tmp, 0o600)
        except OSError:
            pass  # Windows 开发机上 chmod 语义不同，尽力而为
        os.replace(tmp, path)
    finally:
        try:
            if os.path.exists(tmp):
                os.unlink(tmp)
        except OSError:
            pass


@api_v1.get('/designer/css')
def designer_css_get():
    try:
        css = _read_ui_custom()
    except RuntimeError as exc:
        return jsonify({'ok': False, 'error': str(exc), 'code': 'READ_FAILED'}), 500
    return jsonify({'ok': True, 'data': {
        'css': css, 'path': UI_CUSTOM_PATH, 'size': len(css.encode('utf-8')),
    }})


@api_v1.put('/designer/css')
def designer_css_put():
    if not designer_enabled():
        return jsonify({'ok': False, 'code': 'DESIGNER_DISABLED',
                        'error': f'界面设计器已停用（设 {DESIGNER_ENV_FLAG}=1 可开启）'}), 403
    body = request.get_json(silent=True) or {}
    css = body.get('css')
    if not isinstance(css, str):
        return jsonify({'ok': False, 'error': 'css 必须是字符串', 'code': 'BAD_REQUEST'}), 400
    size = len(css.encode('utf-8'))
    if size > UI_CUSTOM_MAX:
        return jsonify({'ok': False, 'error': f'CSS 超过 {UI_CUSTOM_MAX // 1000}KB 上限',
                        'code': 'TOO_LARGE'}), 400
    try:
        _write_ui_custom(css)
    except OSError as exc:
        return jsonify({'ok': False, 'error': f'保存失败: {exc}', 'code': 'WRITE_FAILED'}), 500
    return jsonify({'ok': True, 'data': {'path': UI_CUSTOM_PATH, 'size': size}})


@api_v1.delete('/designer/css')
def designer_css_delete():
    """重置：删掉自定义样式文件，界面回到出厂样式。"""
    if not designer_enabled():
        return jsonify({'ok': False, 'code': 'DESIGNER_DISABLED',
                        'error': f'界面设计器已停用（设 {DESIGNER_ENV_FLAG}=1 可开启）'}), 403
    try:
        os.remove(UI_CUSTOM_PATH)
    except FileNotFoundError:
        pass
    except OSError as exc:
        return jsonify({'ok': False, 'error': f'重置失败: {exc}', 'code': 'DELETE_FAILED'}), 500
    return jsonify({'ok': True, 'data': {'path': UI_CUSTOM_PATH, 'size': 0}})


def register_designer_routes(app: Flask) -> None:
    """注册 /ui-custom.css 样式出口，并按开关决定是否挂 /designer 编辑器页面。

    安全前提：**默认关闭**。设计器可以往 UI_CUSTOM_PATH 写任意 CSS，
    而 index.html 最后加载该文件（优先级最高且无鉴权），历史上任何人访问
    8000 端口都能改界面。现在只有显式设置 TTBOX_ENABLE_DESIGNER=1 时才注册
    /designer；写接口 PUT/DELETE /api/v1/designer/css 在 blueprint 层同样检查该开关。

    /ui-custom.css 必须常驻：设备上可能已经存在手工调好的自定义样式，
    index.html 仍在引用它，删掉会让用户的样式失效（返回空串而非 404）。
    """
    if 'ui_custom_css' in app.view_functions:
        return

    @app.get('/ui-custom.css', endpoint='ui_custom_css')
    def ui_custom_css():
        try:
            css = _read_ui_custom()
        except RuntimeError:
            css = ''
        # no-store：设计器改完刷新就能看到，不被浏览器缓存挡住
        return Response(css, mimetype='text/css; charset=utf-8',
                        headers={'Cache-Control': 'no-store'})

    if not designer_enabled():
        app.logger.info('api_v1: /designer 未启用（设 %s=1 可开启）；/ui-custom.css 读取保持可用',
                        DESIGNER_ENV_FLAG)
        return

    @app.get('/designer', endpoint='designer_page')
    def designer_page():
        try:
            return render_template('designer.html', app_title='TTBOX 界面设计器')
        except TemplateNotFound:
            return Response('designer.html 尚未部署', status=503,
                            mimetype='text/plain; charset=utf-8')


# ====================================================================
# 自测入口：python -X utf8 plugins/web/api_v1.py [--json] [--live]
# ====================================================================
def _build_test_app() -> Flask:
    """构造一个只挂 api_v1 的最小 Flask app，用于本地自检。"""
    app = Flask(__name__)
    app.register_blueprint(api_v1)
    return app


def _probe_core() -> str:
    """实测 Core 连通性（一次 PING），返回给人看的一句话。"""
    resp = ipc_call('PING')
    if resp.get('unreachable'):
        return f'CORE_UNREACHABLE（{resp.get("error")}）'
    if resp.get('status') == 0:
        data = resp.get('data') if isinstance(resp.get('data'), dict) else {}
        return f'已连通（server={data.get("server", "ttbox_core")}）'
    return f'Core 返回 status={resp.get("status")}: {resp.get("error")}'


# 自检时真正会打过去的只读端点（写操作不自动触发，避免动到真机状态）。
_LIVE_PROBES: list[tuple[str, str]] = [
    ('GET', '/api/v1/health'),
    ('GET', '/api/v1/status'),
    ('GET', '/api/v1/config'),
    ('GET', '/api/v1/models'),
    ('GET', '/api/v1/capabilities'),
]


def _self_test(live: bool = False) -> int:
    """打印能力清单表格。返回进程退出码（0=正常）。"""
    out = sys.stdout
    payload = capabilities_payload()
    print('=' * 96)
    print('TTBOX /api/v1 能力自检')
    print('=' * 96)
    print(f'IPC socket   : {payload["ipc_socket"]}')
    print(f'Core 连通性  : {_probe_core()}')
    print(f'core_connected: {payload["core_connected"]}')
    print('')

    print('-- 已实现（真接线 Core IPC）---------------------------------------------------------')
    for item in IMPLEMENTED_PATHS:
        print(f'  [OK]      {item}')
    print('')
    print(f'  模型写操作（不在自检中触发）：'
          f'POST /api/v1/models/{{activate,validate,install,remove,set-concurrency,import}}')
    print('')

    print('-- 未实现（Core 无此能力，如实标注）-------------------------------------------------')
    for item in payload['not_implemented']:
        methods = ','.join(item['methods'])
        print(f'  [未实现]  {methods:12s} {item["path"]:42s} {item["label"]}')
        print(f'            └─ {item["reason"]}')
    print('')

    if live:
        print('-- 实测连通性（test_client 真实调用只读端点）---------------------------------------')
        client = _build_test_app().test_client()
        for method, path in _LIVE_PROBES:
            started = time.time()
            try:
                resp = client.open(path, method=method)
                body = resp.get_data(as_text=True)
                snippet = body.replace('\n', ' ')[:90]
                print(f'  [{resp.status_code}] {method} {path} ({time.time() - started:.2f}s) {snippet}')
            except Exception as exc:  # noqa: BLE001 —— 自检脚本，任何异常都要如实打出来
                print(f'  [ERR] {method} {path}: {exc}')
        print('')

    print('=' * 96)
    print(f'合计：已实现 {len(IMPLEMENTED_PATHS)} 个端点 / 未实现 {len(payload["not_implemented"])} 类能力')
    print('说明：未实现端点返回 HTTP 200 + {"ok":false,"code":"NOT_IMPLEMENTED"}，前端不抛异常也不拿假数据。')
    out.flush()
    return 0


def _main(argv: list[str]) -> int:
    if '--json' in argv:
        print(json.dumps({'ok': True, 'data': capabilities_payload()}, ensure_ascii=False, indent=2))
        return 0
    return _self_test(live=('--live' in argv))


if __name__ == '__main__':
    sys.exit(_main(sys.argv[1:]))
