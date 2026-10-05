# -*- coding: utf-8 -*-
"""api/models.py —— 模型库域 Blueprint。

从 bin/ttbox-web.py 搬出（2026-10-02 web 换写法 S9-b）。
搬出 16 条路由，**URL 一字未改**。

模型库域 —— 本项目最大的一块（16 路由，含远端模型 4 条）。
实现体分布：转换器 lib/model_convert.py（S8）、ui_meta lib/model_ui_meta.py、
导入事务锁 lib/import_lock.py、模型列表 lib/state_snapshot.py。
本文件只做编排。
段外依赖处理：
    · _get_runtime_profile / _preset_names / ipc_request —— 经 hub.call 调用时取。
    · os / re / threading / time —— 标准库，直接 import。
    · jsonify / request —— flask，直接 import。
    · _CONVERT_LOCK(from model_convert), _CONVERT_STATE(from model_convert), _CONVERT_WORKDIR(from model_convert), _begin_import(from import_lock), _conversion_worker(from model_convert), _convert_state_public(from model_convert), _effective_rknn_concurrency(from model_ui_meta), _end_import(from import_lock), _merge_model_ui_meta(from model_ui_meta), _models_view(from state_snapshot), _parse_model_labels_file(from model_convert), _save_model_preset_from_import(from model_convert), _wait_import_done(from import_lock), _write_model_ui_meta(from model_ui_meta), profile_to_web(from profile_translate) —— lib里已有，直接 import。

★ **禁止在 except 子句里调 hub**（求值时机是函数定义时，此时 hub.bind 还没执行 ⇒ 永久绑成 None ⇒ 该 except 永不匹配）。
  需要按异常类型分流时，抽 `_is_xxx(exc)` 函数在调用时才取类。
"""

from __future__ import annotations

import os
import re
import threading
import time

from flask import Blueprint, jsonify, request

from plugins.web.lib import hub
from plugins.web.lib.model_convert import _CONVERT_LOCK
from plugins.web.lib.model_convert import _CONVERT_STATE
from plugins.web.lib.model_convert import _CONVERT_WORKDIR
from plugins.web.lib.import_lock import _begin_import
from plugins.web.lib.model_convert import _conversion_worker
from plugins.web.lib.model_convert import _convert_state_public
from plugins.web.lib.model_ui_meta import _effective_rknn_concurrency
from plugins.web.lib.import_lock import _end_import
from plugins.web.lib.model_ui_meta import _merge_model_ui_meta
from plugins.web.lib.state_snapshot import _models_view
from plugins.web.lib.model_convert import _parse_model_labels_file
from plugins.web.lib.model_convert import _save_model_preset_from_import
from plugins.web.lib.import_lock import _wait_import_done
from plugins.web.lib.model_ui_meta import _write_model_ui_meta
from plugins.web.lib.profile_translate import profile_to_web
from pathlib import Path
from plugins.web.lib import paths as ttbox_paths

bp = Blueprint('models', __name__)

def _models_patch_response(*args, **kwargs):
    """入口的 _models_patch_response —— 调用时取。"""
    return hub.call('_models_patch_response', *args, **kwargs)


def _remote_not_ready(*args, **kwargs):
    """入口的 _remote_not_ready —— 调用时取。"""
    return hub.call('_remote_not_ready', *args, **kwargs)


def _get_runtime_profile(*args, **kwargs):
    """入口的 _get_runtime_profile —— 调用时取（monkeypatch 锚点）。"""
    return hub.call('_get_runtime_profile', *args, **kwargs)


def _preset_names(*args, **kwargs):
    """入口的 _preset_names —— 调用时取（monkeypatch 锚点）。"""
    return hub.call('_preset_names', *args, **kwargs)


def ipc_request(*args, **kwargs):
    """入口的 ipc_request —— 调用时取（monkeypatch 锚点）。"""
    return hub.call('ipc_request', *args, **kwargs)

@bp.get('/api/models')
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


@bp.get('/api/models/convert-status')
def convert_status():
    with _CONVERT_LOCK:
        return jsonify({'ok': True, 'data': _convert_state_public()})


@bp.post('/api/models/import-onnx')
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


@bp.get('/api/models/device-code')
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


@bp.post('/api/models/import')
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
    # ★★ V1.0.39（2026-10-05）Bug 修复（补第三层兜底）：_incoming **总量配额**。
    #   上面的 finally 已保证"上传即删、不留痕"，但那只能防新泄漏；板端实测
    #   **历史已残留 5 个文件 23.4 MB**（repro2/repro3/conc1 等），且本目录
    #   **既无配额也无启动清理**。面板是免密无鉴权的（ttbox-web.py「D1 免密全拆」），
    #   单请求上限 256 MB ⇒ 反复上传即可把磁盘写满。
    #   ⇒ 加目录总量上限，超了直接拒并**回滚本次落盘**（fail-closed，不静默接受）。
    #   阈值 512 MB：现役模型 4~11 MB，够放~50 个未清理的残留又不至于撑爆分区。
    _INCOMING_QUOTA_BYTES = 512 * 1024 * 1024
    try:
        _used = sum(p.stat().st_size for p in incoming.iterdir() if p.is_file())
    except OSError:
        _used = 0
    if _used > _INCOMING_QUOTA_BYTES:
        return jsonify({'ok': False,
                        'error': (f'_incoming 目录已占用 {_used // (1024 * 1024)} MB，'
                                  f'超过配额 {_INCOMING_QUOTA_BYTES // (1024 * 1024)} MB，'
                                  f'拒绝上传（磁盘可能被占满）')}), 507
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
        # ★★ V1.0.39（2026-10-05）Bug 修复：_incoming 的上传落点**必须无条件删除**。
        #   原实现只在 `r1 失败` 那一支 unlink ⇒ 校验失败、安装失败、以及
        #   **成功导入**三条路径都会把 4~11 MB 的 .rknn 永久留在 _incoming/。
        #   板端实测残留 5 个文件 = 23.4 MB（含 repro2/repro3/conc1 等历史垃圾）。
        #
        #   ★ 为什么这条更该修（安全视角）：面板是**免密无鉴权**的（见 ttbox-web.py
        #     「D1 免密全拆」），而 _incoming 是无认证上传的落点、单请求上限 256 MB。
        #     修复前反复上传即可把磁盘写满 ⇒ **未鉴权远程可触发磁盘耗尽**。
        #     修完每次请求不留痕（模型已在 installed/），无法靠此路径占空间。
        #     注：MODEL_IMPORT 已把内容复制进 staging/，此处删的是"上传暂存副本"，
        #     不影响导入结果（staging 副本由 core 侧 install 成功后自行清理）。
        dst.unlink(missing_ok=True)
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


@bp.post('/api/models/delete')
def delete_model():
    body = request.get_json(silent=True) or {}
    model_id = str(body.get('model_id') or '').strip()
    if not model_id:
        return jsonify({'ok': False, 'error': 'missing field: model_id'}), 400
    r = ipc_request('MODEL_REMOVE', {'model_id': model_id})
    if r.get('status') != 0:
        return jsonify({'ok': False, 'error': r.get('error', '删除失败')})
    return jsonify({'ok': True, 'data': {'message': '已删除'}})


@bp.post('/api/models/select')
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


@bp.post('/api/models/bind-preset')
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


@bp.post('/api/models/remote-frame-format')
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


@bp.post('/api/models/rknn-concurrency')
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


@bp.post('/api/models/hailo-pipeline-depth')
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


@bp.post('/api/models/class-names')
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


@bp.post('/api/remote/connect')
def remote_connect():
    return _remote_not_ready()


@bp.get('/api/remote/models')
def remote_models():
    return _remote_not_ready()


@bp.post('/api/remote/import')
def remote_import():
    return _remote_not_ready()


@bp.post('/api/remote/delete')
def remote_delete():
    return _remote_not_ready()
