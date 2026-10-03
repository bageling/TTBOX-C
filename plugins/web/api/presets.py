# -*- coding: utf-8 -*-
"""api/presets.py —— 预设参数域 Blueprint。

从 bin/ttbox-web.py 搬出（2026-10-02 web 换写法 S9-b）。
搬出 5 条路由，**URL 一字未改**。

预设参数 CRUD + 导入导出。
★ `PRESETS_DIR`（3 处）与 `ConfigValidationError`（异常类）都经 hub.get —
  后者绝不能出现在 except 子句（定义时求值，那时 hub.bind 还没跑）。

★ 非路由装饰器 `_config_write_serialized` 从 `lib.locks` **直接 import**：
  装饰器在被装饰函数的 def 行执行（import 期）就被应用，
  那时入口的 hub.bind() 还没跑 ⇒ 走 hub 转发必然 RuntimeError。
段外依赖处理：
    · ConfigValidationError / PRESETS_DIR —— 经 hub.get 调用时取。
    · _get_runtime_profile / _is_preset_name / _preset_names / ipc_request —— 经 hub.call 调用时取。
    · io / json / re —— 标准库，直接 import。
    · jsonify / request / send_file —— flask，直接 import。
    · _RESERVED_PRESET_PREFIX(from settings), normalize_profile_capture_size(from capture_geometry), profile_to_web(from profile_translate), web_body_to_profile(from profile_translate) —— lib里已有，直接 import。

★ **禁止在 except 子句里调 hub**（求值时机是函数定义时，此时 hub.bind 还没执行 ⇒ 永久绑成 None ⇒ 该 except 永不匹配）。
  需要按异常类型分流时，抽 `_is_xxx(exc)` 函数在调用时才取类。
★ 非路由装饰器 `_config_write_serialized` 从 lib.locks **直接 import**（不走 hub）：
  装饰器在 def 行执行（import 期）就被应用，那时 hub.bind 还没跑。
"""

from __future__ import annotations

import io
import json
import re
from pathlib import Path

from flask import Blueprint, jsonify, request, send_file

from plugins.web.lib import hub
from plugins.web.lib.settings import _RESERVED_PRESET_PREFIX
from plugins.web.lib.capture_geometry import normalize_profile_capture_size
from plugins.web.lib.profile_translate import profile_to_web
from plugins.web.lib.profile_translate import web_body_to_profile
from plugins.web.lib.locks import config_write_serialized as _config_write_serialized

from plugins.web.lib.profile_translate import ConfigValidationError

bp = Blueprint('presets', __name__)

def _deep_merge_profile(*args, **kwargs):
    """入口的 _deep_merge_profile —— 调用时取。"""
    return hub.call('_deep_merge_profile', *args, **kwargs)


def PRESETS_DIR():
    """入口的 PRESETS_DIR —— 调用时取（补丁锚点 / 单例 / 异常类，身份须唯一）。"""
    return hub.get('PRESETS_DIR')


def _get_runtime_profile(*args, **kwargs):
    """入口的 _get_runtime_profile —— 调用时取（monkeypatch 锚点）。"""
    return hub.call('_get_runtime_profile', *args, **kwargs)


def _is_preset_name(*args, **kwargs):
    """入口的 _is_preset_name —— 调用时取（monkeypatch 锚点）。"""
    return hub.call('_is_preset_name', *args, **kwargs)


def _preset_names(*args, **kwargs):
    """入口的 _preset_names —— 调用时取（monkeypatch 锚点）。"""
    return hub.call('_preset_names', *args, **kwargs)


def ipc_request(*args, **kwargs):
    """入口的 ipc_request —— 调用时取（monkeypatch 锚点）。"""
    return hub.call('ipc_request', *args, **kwargs)

@bp.get('/api/presets')
def list_presets():
    # ★ 排除 `_` 保留名（见 _preset_names 注释）
    return jsonify({'ok': True, 'data': {'presets': _preset_names()}})


@bp.post('/api/presets')
def save_or_delete_preset():
    body = request.get_json(silent=True) or {}
    name = str(body.get('name', '')).strip()
    action = body.get('action', 'save')
    if not name:
        return jsonify({'ok': False, 'error': '缺少预设名'})
    d = Path(PRESETS_DIR())
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


@bp.post('/api/presets/load')
@_config_write_serialized
def load_preset():
    body = request.get_json(silent=True) or {}
    name = str(body.get('name', '')).strip()
    safe = re.sub('[^\\w\\-]', '_', name)[:64]
    pf = Path(PRESETS_DIR()) / (safe + '.json')
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


@bp.post('/api/presets/import')
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
    d = Path(PRESETS_DIR())
    d.mkdir(parents=True, exist_ok=True)
    try:
        (d / (safe + '.json')).write_text(json.dumps(data, ensure_ascii=False, indent=2))
    except OSError as exc:
        return jsonify({'ok': False, 'error': f'预设写入失败：{exc}'}), 500
    return jsonify({'ok': True, 'data': {'name': safe, 'preset': data}})


@bp.get('/api/presets/<name>/export')
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
    pf = Path(PRESETS_DIR()) / (safe + '.json')
    if not pf.exists():
        return jsonify({'ok': False, 'error': f'failed to open {pf}'})
    try:
        data = json.loads(pf.read_text())
    except Exception as exc:
        return jsonify({'ok': False, 'error': f'preset is damaged: {exc}'})
    body = json.dumps(data, ensure_ascii=False, indent=2).encode('utf-8')
    return send_file(io.BytesIO(body), mimetype='application/json',
                     as_attachment=True, download_name=f'{safe}.json')
