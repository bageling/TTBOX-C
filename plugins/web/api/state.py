# -*- coding: utf-8 -*-
"""api/state.py —— 运行状态与配置域 Blueprint。

从 bin/ttbox-web.py 搬出（2026-10-02 web 换写法 S9-b）。
搬出 4 条路由，**URL 一字未改**。

运行状态（/api/state）、配置读写（/api/config）、事件流（/api/events）。

`get_state` 是**唯一把整个状态聚合器直接暴露给前端的出口** ——
`collect_web_state` 在 lib/state_snapshot.py（17 个 hub 依赖，全层最重）。
★ `update_config` 带 `@_config_write_serialized` 装饰器，
  该装饰器**从 lib.locks 直接 import**（装饰器在 import 期求值，
  那时 hub.bind 还没跑 ⇒ 走 hub 转发必然 RuntimeError）。

★ 非路由装饰器 `_config_write_serialized` 从 `lib.locks` **直接 import**：
  装饰器在被装饰函数的 def 行执行（import 期）就被应用，
  那时入口的 hub.bind() 还没跑 ⇒ 走 hub 转发必然 RuntimeError。
段外依赖处理：
    · ConfigValidationError —— 经 hub.get 调用时取。
    · _get_runtime_profile / collect_web_state / ipc_request —— 经 hub.call 调用时取。
    · jsonify / request —— flask，直接 import。
    · normalize_profile_capture_size(from capture_geometry), profile_to_web(from profile_translate), web_body_to_profile(from profile_translate) —— lib里已有，直接 import。

★ **禁止在 except 子句里调 hub**（求值时机是函数定义时，此时 hub.bind 还没执行 ⇒ 永久绑成 None ⇒ 该 except 永不匹配）。
  需要按异常类型分流时，抽 `_is_xxx(exc)` 函数在调用时才取类。
★ 非路由装饰器 `_config_write_serialized` 从 lib.locks **直接 import**（不走 hub）：
  装饰器在 def 行执行（import 期）就被应用，那时 hub.bind 还没跑。
"""

from __future__ import annotations

from flask import Blueprint, jsonify, request

from plugins.web.lib import hub
from plugins.web.lib.capture_geometry import normalize_profile_capture_size
from plugins.web.lib.profile_translate import profile_to_web
from plugins.web.lib.profile_translate import web_body_to_profile
from plugins.web.lib.locks import config_write_serialized as _config_write_serialized

from plugins.web.lib.profile_translate import ConfigValidationError

bp = Blueprint('state', __name__)

def _deep_merge_profile(*args, **kwargs):
    """入口的 _deep_merge_profile —— 调用时取。"""
    return hub.call('_deep_merge_profile', *args, **kwargs)


def _get_runtime_profile(*args, **kwargs):
    """入口的 _get_runtime_profile —— 调用时取（monkeypatch 锚点）。"""
    return hub.call('_get_runtime_profile', *args, **kwargs)


def collect_web_state(*args, **kwargs):
    """入口的 collect_web_state —— 调用时取（monkeypatch 锚点）。"""
    return hub.call('collect_web_state', *args, **kwargs)


def ipc_request(*args, **kwargs):
    """入口的 ipc_request —— 调用时取（monkeypatch 锚点）。"""
    return hub.call('ipc_request', *args, **kwargs)

@bp.get('/api/state')
def get_state():
    return jsonify(collect_web_state())


@bp.put('/api/config')
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


@bp.get('/api/config')
def get_config_web():
    prof = _get_runtime_profile()
    return jsonify({'ok': True, 'data': profile_to_web(prof)})


@bp.get('/api/events')
def get_events():
    return jsonify({'ok': True, 'data': {'events': []}})
