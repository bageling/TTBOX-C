# -*- coding: utf-8 -*-
"""模型 UI meta 簇 —— 从 bin/ttbox-web.py 原样搬出（2026-10-02 web 换写法 S7 第三刀）。

搬出的内容（入口 L1105-1164，逐行未改）：
    _MODEL_UI_META_KEYS                —— ui_meta.json 允许持久化的字段白名单
    _model_ui_meta_path / _read_model_ui_meta / _write_model_ui_meta
    _merge_model_ui_meta               —— 模型记录合并 UI 扩展字段
    _effective_rknn_concurrency        —— 界面并发 = Core 实际 worker 数（经 IPC）

★ 偏离清单：**0 处**。
   段外依赖处理：
     · os / json / Path / ttbox_paths —— 非补丁锚点，直接 import。
     · ipc_request —— 补丁锚点（测试 10 处 monkeypatch），经 hub.call 调用时取。
     · CoreUnavailableError —— 入口定义的异常类，经 hub.get 调用时取。
       （用 hub 而非 import 是因为它由入口 raise，lib 侧必须拿到同一个类对象，
         否则except 捕获会落空 —— 异常类身份不一致是 Python 隐蔽坑。）
"""

from __future__ import annotations

import json
import os
from pathlib import Path

from plugins.web.lib import hub
from plugins.web.lib import paths as ttbox_paths


def ipc_request(*args, **kwargs):
    """入口的 ipc_request（monkeypatch 锚点 ×10）—— 调用时取。"""
    return hub.call('ipc_request', *args, **kwargs)


def _core_unavailable_error():
    """入口的 CoreUnavailableError —— 调用时取，保证 except 捕获到同一个类。"""
    return hub.get('CoreUnavailableError')


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
            raise _core_unavailable_error()('Core 离线，无法读取全局 worker_cores')
        wc = str(r.get('data', {}).get('worker_cores') or '').strip()
    tokens = [t.strip() for t in wc.split(',') if t.strip() in ('1', '2', '4')]
    count = len(tokens)
    return count if 1 <= count <= 3 else 3
