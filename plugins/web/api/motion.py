# -*- coding: utf-8 -*-
"""api/motion.py —— 运动训练/个人运动模型域 Blueprint。

从 bin/ttbox-web.py 搬出（2026-10-02 web 换写法 S9-a，第 3 刀）。
搬出 13 条路由 + 2 个私有辅助，**URL 一字未改**：

    GET    /api/motion-profiles                                   list_motion_profiles
    POST   /api/motion-profiles                                   create_motion_profile
    PATCH  /api/motion-profiles/<profile_id>rename_motion_profile
    DELETE /api/motion-profiles/<profile_id>                      delete_motion_profile
    GET    /api/motion-profiles/<profile_id>/export               export_motion_profile
    POST   /api/motion-training/sessions                start_motion_training_session
    PUT    /api/motion-training/sessions/<session_id>/heartbeat   heartbeat_...
    POST   /api/motion-training/sessions/<session_id>/samples     append_motion_training_sample
    DELETE /api/motion-training/sessions/<session_id>             stop_motion_training_session
    POST   /api/motion-profiles/<profile_id>/train                train_motion_profile
    POST   /api/motion-profiles/<profile_id>/activate            activate_motion_profile
    DELETE /api/motion-profiles/active                            deactivate_motion_profile
    DELETE /api/motion-profiles/<profile_id>/samples              clear_motion_profile_samples

★★ **本域踩过一个隐蔽坑，抄这段代码前必读** ★★

第一版把异常类写成 `except (_motion_training_error(), OSError):`，
看起来和 `except MotionTrainingError` 等价，**其实完全不同**：

    ★ except 子句在**函数定义时求值一次**，不是每次抛出时求值。
    ★ 本模块由入口的 register_blueprints(app) 在**import 期**被拉进来，
      而 hub.bind() 在入口**文件末尾**才执行 ⇒ import 期 hub._entry 还是 None
      ⇒ 12 处 except 永久绑成 None ⇒ **该except 永不匹配**
      ⇒ 训练会话冲突本该返 409，实际会穿透成 500。

这正是 lib/hub.py 文档第3 条「lib 模块里禁止在 import 期调用 hub」，
只是 except 子句这个位置更隐蔽（它藏在函数体内，但求值时机是定义时）。

**正确写法**：把「哪些异常要转409/422」抽成一个函数，调用时才取类：

    def _is_motion_error(exc):
        return isinstance(exc, (hub.get('MotionTrainingError'),
                                hub.get('MotionSampleError'), OSError))

    #路由里：
    except Exception as exc:
        if not _is_motion_error(exc):
            raise
        return _motion_error(exc)

段外依赖处理：
    · _CFG_WRITE_LOCK —— **直接 import** lib.locks 的单点对象（不得 hub.get：
      测试用 exec + 命名空间注锁，见 lib/locks.py 的说明）。
    · MOTION_STORE / MOTION_PROFILES_DIR / MotionTrainingError /
      MotionSampleError / _get_runtime_profile / ipc_request —— 经 hub 调用时取。
    · json / time / request / jsonify / send_file —— 标准库与 flask，直接 import。

偏离清单：函数体逻辑零改动；except 子句由「类」改为「函数 + raise重抛」，
这是**修复上述隐患**所必需，行为等价（原来捕不到的异常现在会正确重抛）。
"""

from __future__ import annotations

import json
import time

from flask import Blueprint, jsonify, request, send_file

from plugins.web.lib import hub
from plugins.web.lib.locks import _CFG_WRITE_LOCK     # 单点锁，直接 import（裸名）

bp = Blueprint('motion', __name__)


# ---- 段外依赖的取数函数（**全部在调用时取**，禁止出现在模块级/except 子句） ----
def _store():
    """入口的 MOTION_STORE（有状态单例）—— 调用时取，保证同一实例。"""
    return hub.get('MOTION_STORE')


def _profiles_dir():
    """入口的 MOTION_PROFILES_DIR —— 调用时取。"""
    return hub.get('MOTION_PROFILES_DIR')


def _get_runtime_profile(*args, **kwargs):
    """入口的 _get_runtime_profile —— 调用时取。"""
    return hub.call('_get_runtime_profile', *args, **kwargs)


def ipc_request(*args, **kwargs):
    """入口的 ipc_request —— 调用时取。"""
    return hub.call('ipc_request', *args, **kwargs)


def _is_motion_error(exc) -> bool:
    """这个异常是否该转成 409/422 —— **调用时**取异常类（见文件头警示）。"""
    return isinstance(exc, (hub.get('MotionTrainingError'),
                            hub.get('MotionSampleError'), OSError))


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
            raise hub.get('MotionTrainingError')('读取 TTBOX Core RuntimeProfile 失败')
        personal = prof.setdefault('mouse', {}).setdefault('personal_motion', {})
        personal['enabled'] = bool(enabled)
        if enabled:
            profile = _store().list_profile(profile_id)
            model = profile.get('model') or {}
            if not model.get('ready'):
                raise hub.get('MotionTrainingError')('model is not ready')
            values = mix or _store()._mix()
            personal.update({
                'curve_blend': values.get('curve', 1.0),
                'speed_blend': values.get('speed', 1.0),
                'reaction_blend': values.get('reaction', 0.7),
                'max_reaction_delay_ms': values.get('max_reaction_delay_ms', 250),
                'knots': model.get('knots', []),
            })
        result = ipc_request('SET_CONFIG', {'profile': prof})
    if result.get('status') != 0:
        raise hub.get('MotionTrainingError')(result.get('error', 'Core 配置更新失败'))
    return prof


@bp.get('/api/motion-profiles')
def list_motion_profiles():
    try:
        return jsonify({'ok': True, 'data': _store().list_profiles()})
    except Exception as exc:
        if not _is_motion_error(exc):
            raise
        return _motion_error(exc)


@bp.post('/api/motion-profiles')
def create_motion_profile():
    # 保持 Web 契约：只支持内置 default 档案，拒绝创建新档案
    return jsonify({'ok': False, 'error': 'only the internal default motion profile is supported'})


@bp.patch('/api/motion-profiles/<profile_id>')
def rename_motion_profile(profile_id: str):
    body = request.get_json(silent=True) or {}
    try:
        return jsonify({'ok': True, 'data': _store().rename(profile_id, body.get('name', ''))})
    except Exception as exc:
        if not _is_motion_error(exc):
            raise
        return _motion_error(exc)


@bp.delete('/api/motion-profiles/<profile_id>')
def delete_motion_profile(profile_id: str):
    try:
        return jsonify({'ok': True, 'data': _store().delete(profile_id)})
    except Exception as exc:
        if not _is_motion_error(exc):
            raise
        return _motion_error(exc)


@bp.get('/api/motion-profiles/<profile_id>/export')
def export_motion_profile(profile_id: str):
    try:
        profile = _store().list_profile(profile_id)
        export_path = _profiles_dir() / f'.motion-profile-{profile_id}.json'
        export_path.write_text(json.dumps(profile, ensure_ascii=False, indent=2), encoding='utf-8')
        return send_file(export_path, mimetype='application/json', as_attachment=True,
                         download_name=f'ttbox-motion-profile-{profile_id}.json')
    except Exception as exc:
        if not _is_motion_error(exc):
            raise
        return _motion_error(exc)


@bp.post('/api/motion-training/sessions')
def start_motion_training_session():
    body = request.get_json(silent=True) or {}
    try:
        result = _store().start_session(str(body.get('profile_id') or ''), now=time.time())
        return jsonify({'ok': True, 'data': {
            'session_id': result['id'], 'profile_id': result['profile_id'],
            'lease_expires_at': result['lease_expires_at'],
        }})
    except Exception as exc:
        if not _is_motion_error(exc):
            raise
        return _motion_error(exc)


@bp.put('/api/motion-training/sessions/<session_id>/heartbeat')
def heartbeat_motion_training_session(session_id: str):
    try:
        return jsonify({'ok': True, 'data': _store().heartbeat(session_id)})
    except Exception as exc:
        if not _is_motion_error(exc):
            raise
        return _motion_error(exc)


@bp.post('/api/motion-training/sessions/<session_id>/samples')
def append_motion_training_sample(session_id: str):
    body = request.get_json(silent=True)
    try:
        return jsonify({'ok': True, 'data': _store().append_sample(session_id, body)})
    except Exception as exc:
        if not _is_motion_error(exc):
            raise
        return _motion_error(exc)


@bp.delete('/api/motion-training/sessions/<session_id>')
def stop_motion_training_session(session_id: str):
    try:
        return jsonify({'ok': True, 'data': _store().stop_session(session_id)})
    except Exception as exc:
        if not _is_motion_error(exc):
            raise
        return _motion_error(exc)


@bp.post('/api/motion-profiles/<profile_id>/train')
def train_motion_profile(profile_id: str):
    try:
        return jsonify({'ok': True, 'data': _store().train(profile_id)})
    except Exception as exc:
        if not _is_motion_error(exc):
            raise
        return _motion_error(exc)


@bp.post('/api/motion-profiles/<profile_id>/activate')
def activate_motion_profile(profile_id: str):
    body = request.get_json(silent=True) or {}
    try:
        result = _store().activate(profile_id, **body)
        _apply_personal_motion_to_core(True, profile_id, result['mix'])
        return jsonify({'ok': True, 'data': result})
    except Exception as exc:
        if not _is_motion_error(exc):
            raise
        return _motion_error(exc)


@bp.delete('/api/motion-profiles/active')
def deactivate_motion_profile():
    try:
        result = _store().deactivate()
        _apply_personal_motion_to_core(False)
        return jsonify({'ok': True, 'data': result})
    except Exception as exc:
        if not _is_motion_error(exc):
            raise
        return _motion_error(exc)


@bp.delete('/api/motion-profiles/<profile_id>/samples')
def clear_motion_profile_samples(profile_id: str):
    try:
        return jsonify({'ok': True, 'data': _store().clear_samples(profile_id)})
    except Exception as exc:
        if not _is_motion_error(exc):
            raise
        return _motion_error(exc)
