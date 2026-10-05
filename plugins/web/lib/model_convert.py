# -*- coding: utf-8 -*-
"""模型转换簇 —— 从 bin/ttbox-web.py 原样搬出（2026-10-02 web 换写法 S8 第二刀）。

搬出的内容（两段，原入口被 list_models 路由隔开，此处按域合并）：
  A 段（原 L1144-1210）：
    _parse_model_labels_file            —— 类别标签文件解析（.txt/.names/.json/.csv）
    _save_model_preset_from_import      —— 导入预设参数到 PRESETS_DIR
  B 段（原 L1263-1396）：
    _CONVERT_STATE / _CONVERT_LOCK / _CONVERT_WORKDIR / _TB
    _CONVERTER_SCRIPT / _CONVERTER_PYTHON / _CONVERT_CALIB_DIR
    _convert_state_public               —— 转换任务对外状态
    _run_onnx_conversion                —— 调外部转换器进程
    _conversion_worker                —— 转换线程体（三步 IPC 导入事务）

★ 偏离清单：**0 处**（两段逐行原样）。
  段外依赖处理（都不是逻辑改动）：
    · 标准库 —— 直接 import。
    · PRESETS_DIR —— 补丁锚点（测试 3 处 monkeypatch），经 hub.get 调用时取。
    · ipc_request —— 补丁锚点（测试 10 处），经 hub.call 调用时取。
    · _begin_import / _end_import —— S8a 已搬到 lib.import_lock，直接 import。
    · _write_model_ui_meta —— S7c 已搬到 lib.model_ui_meta，直接 import。
    · 函数内 `import hashlib as _hashlib` 保持原样（两处各一次，本就刻意为之）。
"""

from __future__ import annotations

import json
import os
import re
import shutil
import subprocess
import sys
import threading
import time
import zipfile
from pathlib import Path

from plugins.web.lib import hub
from plugins.web.lib import paths as ttbox_paths
from plugins.web.lib.import_lock import _begin_import, _end_import
from plugins.web.lib.model_ui_meta import _write_model_ui_meta


def _presets_dir():
    """入口的 PRESETS_DIR（monkeypatch 锚点 ×3）—— 调用时取。"""
    return hub.get('PRESETS_DIR')


def ipc_request(*args, **kwargs):
    """入口的 ipc_request（monkeypatch 锚点 ×10）—— 调用时取。"""
    return hub.call('ipc_request', *args, **kwargs)


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
    """导入预设参数文件到入口的 PRESETS_DIR，返回安全预设名；不可用返回空字符串。"""
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
    d = Path(_presets_dir())
    d.mkdir(parents=True, exist_ok=True)
    (d / (safe + '.json')).write_text(json.dumps(data, ensure_ascii=False, indent=2), encoding='utf-8')
    return safe


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
_TB = ttbox_paths.prefix_path
_CONVERTER_SCRIPT = os.environ.get('TTBOX_CONVERTER_SCRIPT',
                                   _TB('tools', 'converter', 'convert_onnx_to_rknn.py'))
_CONVERTER_PYTHON = os.environ.get('TTBOX_CONVERTER_PYTHON', _TB('venv-convert', 'bin', 'python'))
_CONVERT_CALIB_DIR = os.environ.get('TTBOX_CONVERT_CALIB_DIR', _TB('calib', 'imgs'))


def convert_prerequisites() -> list:
    """返回 ONNX→RKNN 转换**缺失的依赖清单**（空列表 = 依赖齐备、可以转）。

    V1.0.39（2026-10-05）：转换链入口此前**完全不检查依赖**，板端三个依赖全缺时
    用户点了只会看到 `errno 2 No such file or directory`（FileNotFoundError 被
    `_conversion_worker` 最外层的 `except Exception` 吞掉），完全看不出原因。
    本函数把"能不能转"变成**入口处一句话就能说清**的事实。

    板端实测缺失（根因：`deploy/pack_manifest.txt` 未收录 tools/ 与 calib/）：
      · venv-convert 解释器（装 rknn-toolkit2 的那个 venv）
      · tools/converter/convert_onnx_to_rknn.py（转换器本体）
      · calib/imgs/（INT8 量化校准集；缺了量化不准，转换器也会失败）
    """
    missing: list = []
    if not os.path.isfile(_CONVERTER_PYTHON) or not os.access(_CONVERTER_PYTHON, os.X_OK):
        missing.append(f'转换器解释器 {_CONVERTER_PYTHON}')
    if not os.path.isfile(_CONVERTER_SCRIPT):
        missing.append(f'转换器脚本 {_CONVERTER_SCRIPT}')
    # 校准集：默认目录必须是目录且至少 1 张图（转换器 --dataset-count 5）
    if not os.path.isdir(_CONVERT_CALIB_DIR):
        missing.append(f'校准数据集目录 {_CONVERT_CALIB_DIR}')
    else:
        imgs = [n for n in os.listdir(_CONVERT_CALIB_DIR)
                if n.lower().rsplit('.', 1)[-1] in ('jpg', 'jpeg', 'png', 'bmp', 'webp')]
        if not imgs:
            missing.append(f'校准图片（{_CONVERT_CALIB_DIR} 内无 jpg/png）')
    return missing


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
            # ★ V1.0.39：把失败原因落进日志。原实现只 append 原始输出，
            #   转换器非零退出时日志里往往只有一行 traceback，定位要靠猜。
            log_lines.append(
                f'[attempt {attempt}] converter returncode={proc.returncode}'
                f' output_exists={output_path.exists()}'
                f' size={output_path.stat().st_size if output_path.exists() else 0}')
        except subprocess.TimeoutExpired:
            log_lines.append('converter timeout (1200s)')
        except OSError as exc:
            # ★★ V1.0.39：补OSError 捕获（原先只捕 TimeoutExpired）。
            #   解释器/脚本不存在时 subprocess 抛 FileNotFoundError（OSError 子类），
            #   原实现会一路冒到 `_conversion_worker` 最外层的 `except Exception`
            #   ⇒ 状态只显示 `errno 2 No such file or directory`，用户看不出是"转换器没装"。
            #   现在就地记成人话（依赖齐备时仍出现 ⇒ 权限/路径问题，同样说清楚）。
            log_lines.append(f'converter 无法启动: {exc}（cmd[0]={cmd[0]} cmd[1]={cmd[1]}）')
            return False
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
            # ★★ V1.0.39（2026-10-05）Bug 修复：转换链的 _incoming 落点也必须无条件删除。
            #   这里与 `api/models.py` 的直传链是**同一个泄漏模式的第二处**
            #   （V1.0.39 已在直传链修掉，转换链当时漏了）：三个 `return` 分支
            #   （IMPORT/VALIDATE/INSTALL 失败）与成功路径**都不删** incoming_rknn
            #   ⇒ 每次 ONNX 转换永久留一份 rknn（4~11 MB）。
            #   放在 _end_import 之后的 finally：三条失败分支 + 成功路径全部覆盖。
            #   （模型内容此刻已在 staging/ 与 installed/，删的是上传暂存副本。）
            incoming_rknn.unlink(missing_ok=True)
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
