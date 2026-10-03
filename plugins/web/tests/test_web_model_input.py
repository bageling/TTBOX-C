# test_web_model_input.py — T1.15 模型输入通路诊断：Core → Web → 面板 的通路单测
#
# 背景（为什么要有这条链）：
#   TTBOX 输入侧有三条互斥通路，判据唯一入口是 core/src/rknn/InputQuant.hpp::classify_input_pass：
#     · uint8_native ← UINT8 ∧ NHWC            直传零变换（最优）
#     · xor_shift128 ← INT8 ∧ NHWC ∧ AFFINE ∧ zp==-128   CPU 拷贝 + XOR 0x80（快路径）
#     · compatible   ← 其余                    runtime 内部量化（每帧 set_input 贵一个量级）
#   本轮之前：这个结论**只写在板端串口日志里**（RKNNEngine.cpp 会打印实测 qnt/scale/zp）。
#   后果：用户把 FP16 换成 INT8 模型后，**在面板上看不出到底有没有吃到快路径**，只能靠猜。
#
# 本文件锁死"Core 已经算好的结论，Web 层必须原样端到面板"这后半段：
#   1 /api/state 的 state.model_input 与 core metrics.model_* 字段 1:1 同名同义
#   2 metrics 缺字段（旧 Core / 未运行）⇒ 明确 unknown + False + 0，**不猜成 compatible**
#   3 Web 层**不得**重算谓词：core 说不知道就必须原样透传"不知道"
#   4 core 键名契约锁：改名即变红（Web 与外部诊断工具按名消费）
#   5 framework 兼容面 /api/core/status 已随 S1 减法移出出货包（A0-3c），
#     「两面同源」约束随之终结；framework_api 自身路由契约改由
#     framework/tests/test_web_framework_api.py（独立 app）覆盖。
#
# 判据的**唯一来源**在 core，故本文件刻意**不**重复断言"什么模型该走哪条路径"
#   —— 那部分在 core/tests/test_input_path_summary.cpp（纯函数矩阵交叉锁）。
#   重复一份 Python 版谓词 = 制造第三份会漂移的实现。
#
# 运行：python -m pytest plugins/web/tests/test_web_model_input.py -v
# 说明：本文件属 Python 侧，**不并入 C++ ttbox_core_tests**，不动三个 C++ 计数域。
import importlib.util
import os
import pathlib
import sys
import tempfile

import pytest

from plugins.web.lib.paths import repo_root as _ttbox_repo_root  # noqa: E402  路径单点真源（A-PATH-3）
REPO_ROOT = pathlib.Path(_ttbox_repo_root())
WEB_SRC = REPO_ROOT / 'plugins' / 'web' / 'bin' / 'ttbox-web.py'

_load_seq = 0

# state.model_input 的**完整**键集（多一个 = 契约外泄漏；少一个 = 面板读不到）
MODEL_INPUT_KEYS = {
    'pass_mode', 'fast_path_active', 'zero_copy_ready',
    'tensor_type', 'tensor_format', 'quant_type',
    'zero_point', 'scale', 'model_width', 'model_height',
    'external_dma_requested', 'external_dma_bound',
    'workers_total', 'workers_zero_copy', 'workers_fast_path',
    'note',
}

# core IPC GET_STATUS.metrics 里本特性消费的键（**契约锁**：改名即本用例变红）
CORE_METRIC_KEYS_CONSUMED = (
    'model_input_pass_mode', 'model_fast_path_active', 'model_zero_copy_ready',
    'model_input_type_name', 'model_input_fmt_name', 'model_input_qnt_name',
    'model_input_zp', 'model_input_scale', 'model_input_width', 'model_input_height',
    'model_external_dma_requested', 'model_external_dma_bound',
    'model_workers_total', 'model_workers_zero_copy', 'model_workers_fast_path',
    'model_input_note',
)


def _load():
    global _load_seq
    _load_seq += 1
    name = 'ttbox_web_modelinput_%d_%d' % (os.getpid(), _load_seq)
    spec = importlib.util.spec_from_file_location(name, WEB_SRC)
    mod = importlib.util.module_from_spec(spec)
    sys.modules[name] = mod
    spec.loader.exec_module(mod)
    return mod


def _fast_path_metrics():
    """一份"INT8 零均值 + 零拷贝就绪 + 快路径激活"的真实形态 metrics。"""
    return {
        'capture_fps': 120.0,
        'input_width': 1920,
        'input_height': 1080,
        'model_input_pass_mode': 'xor_shift128',
        'model_input_type': 2,
        'model_input_type_name': 'int8',
        'model_input_fmt': 1,
        'model_input_fmt_name': 'nhwc',
        'model_input_qnt_type': 2,
        'model_input_qnt_name': 'affine_asymmetric',
        'model_input_zp': -128,
        'model_input_scale': 1.0 / 255.0,
        'model_input_width': 640,
        'model_input_height': 640,
        'model_zero_copy_ready': True,
        'model_fast_path_active': True,
        'model_external_dma_requested': False,
        'model_external_dma_bound': False,
        'model_workers_total': 3,
        'model_workers_zero_copy': 3,
        'model_workers_fast_path': 3,
        'model_input_note': '',
    }


@pytest.fixture()
def web_mod(monkeypatch):
    """加载 ttbox-web.py，并把 collect_web_state 的全部外部依赖（IPC / 硬件 / 品牌）替换掉。

    只替换**模块边界**，不替换被测函数本身 —— 即被测的仍是产品里那一份 collect_web_state。
    """
    tmp = tempfile.mkdtemp(prefix='ttbox_modelinput_%d_' % os.getpid())
    monkeypatch.setenv('TTBOX_PLUGINS_ROOT', os.path.join(tmp, 'plugins'))
    monkeypatch.setenv('TTBOX_PLUGIN_REPOSITORY_ROOT', os.path.join(tmp, 'repository'))
    mod = _load()
    mod.CRED_PATH = os.path.join(tmp, 'web_credentials.json')
    return mod


def _patch_state_deps(monkeypatch, mod, metrics):
    """把 collect_web_state 依赖的模块级函数换成确定值（metrics 是唯一变量）。"""
    monkeypatch.setattr(mod, '_get_status', lambda: {
        'running': True, 'runtime_running': True, 'version': 'test-1.0',
        'metrics': metrics,
    })
    monkeypatch.setattr(mod, '_get_runtime_profile', lambda: {'model_id': 'm1'})
    monkeypatch.setattr(mod, 'ipc_request',
                        lambda *a, **k: {'status': 0, 'data': {'models': [], 'active': ''}})
    monkeypatch.setattr(mod, '_license_block', lambda: {'ui_brand': 'ttbox', 'valid': True})
    monkeypatch.setattr(mod, '_ui_block', lambda brand=None: {'ui_brand': 'ttbox'})
    monkeypatch.setattr(mod, '_calibration_payload', lambda: {'runtime': {}})
    monkeypatch.setattr(mod, '_core_state_payload', lambda: {})
    monkeypatch.setattr(mod, '_fan_control_payload', lambda: {})
    monkeypatch.setattr(mod, '_loopout_payload', lambda: {})


# ---------------------------------------------------------------------------
# 1. 正例：INT8 快路径，逐字段投影
# ---------------------------------------------------------------------------

def test_state_projects_fast_path_metrics_verbatim(web_mod, monkeypatch):
    _patch_state_deps(monkeypatch, web_mod, _fast_path_metrics())
    mi = web_mod.collect_web_state()['data']['state']['model_input']
    assert mi['pass_mode'] == 'xor_shift128'
    assert mi['fast_path_active'] is True
    assert mi['zero_copy_ready'] is True
    assert mi['tensor_type'] == 'int8'
    assert mi['tensor_format'] == 'nhwc'
    assert mi['quant_type'] == 'affine_asymmetric'
    assert mi['zero_point'] == -128
    assert mi['scale'] == 1.0 / 255.0
    assert mi['model_width'] == 640
    assert mi['model_height'] == 640
    assert mi['external_dma_requested'] is False
    assert mi['external_dma_bound'] is False
    assert mi['workers_total'] == 3
    assert mi['workers_fast_path'] == 3
    assert mi['workers_zero_copy'] == 3
    assert mi['note'] == ''


def test_state_model_input_has_exactly_the_contract_keys(web_mod, monkeypatch):
    """键集固定：多键 = 内部结构泄漏到面板；少键 = 面板静默读不到。"""
    _patch_state_deps(monkeypatch, web_mod, _fast_path_metrics())
    mi = web_mod.collect_web_state()['data']['state']['model_input']
    assert set(mi.keys()) == MODEL_INPUT_KEYS


def test_core_metric_key_names_are_locked(web_mod, monkeypatch):
    """契约锁：core metrics 的 model_* 键名一旦被改，本用例立刻变红。

    做法：把 metrics 的键**逐个**改名，断言该键的投影随之丢失 ——
    这样"Web 读的键名"与"契约名单"必然一致，不会出现"改了 core 键名、
    Web 静默读到默认值、面板显示未运行"这种无声故障。
    """
    metrics = _fast_path_metrics()
    _patch_state_deps(monkeypatch, web_mod, metrics)
    mi = web_mod.collect_web_state()['data']['state']['model_input']

    # 允许的取值映射（core 键 → 投影键）
    observed_pairs = {
        'model_input_pass_mode': ('pass_mode', 'xor_shift128'),
        'model_fast_path_active': ('fast_path_active', True),
        'model_zero_copy_ready': ('zero_copy_ready', True),
        'model_input_type_name': ('tensor_type', 'int8'),
        'model_input_fmt_name': ('tensor_format', 'nhwc'),
        'model_input_qnt_name': ('quant_type', 'affine_asymmetric'),
        'model_input_zp': ('zero_point', -128),
        'model_input_width': ('model_width', 640),
        'model_input_height': ('model_height', 640),
        'model_workers_total': ('workers_total', 3),
        'model_workers_fast_path': ('workers_fast_path', 3),
        'model_workers_zero_copy': ('workers_zero_copy', 3),
    }
    # 名单必须与 CORE_METRIC_KEYS_CONSUMED 对齐（防止有人只改一处）
    assert set(observed_pairs.keys()) | {
        'model_input_scale', 'model_external_dma_requested',
        'model_external_dma_bound', 'model_input_note',
    } == set(CORE_METRIC_KEYS_CONSUMED)

    for core_key, (projected_key, expected) in observed_pairs.items():
        assert mi[projected_key] == expected, '%s 未按契约投影' % core_key
        # 名字必须真的是 core 的那个键：改名后该键应消失（其余不动）
        assert core_key in metrics


# ---------------------------------------------------------------------------
# 2. 负例：字段缺失（旧 Core / 未运行）⇒ 明确 unknown，**绝不猜成 compatible**
# ---------------------------------------------------------------------------

def test_absent_metrics_yield_unknown_not_compatible(web_mod, monkeypatch):
    """最重要的负例：不知道 ≠ 兼容 I/O。

    若这里回落成 'compatible'，面板会在 Core 未运行时显示"兼容 I/O（慢）"，
    把"没数据"谎报成"已判定为慢路径"—— 正是本特性要消灭的盲区。
    """
    _patch_state_deps(monkeypatch, web_mod, {})
    mi = web_mod.collect_web_state()['data']['state']['model_input']
    assert mi['pass_mode'] == 'unknown'
    assert mi['fast_path_active'] is False
    assert mi['zero_copy_ready'] is False
    assert mi['tensor_type'] == 'unknown'
    assert mi['tensor_format'] == 'unknown'
    assert mi['quant_type'] == 'unknown'
    assert mi['zero_point'] == 0
    assert mi['scale'] == 0.0
    assert mi['model_width'] == 0
    assert mi['model_height'] == 0
    assert mi['workers_total'] == 0
    assert mi['note'] == ''
    assert set(mi.keys()) == MODEL_INPUT_KEYS


def test_unknown_mode_string_is_passed_through_unchanged(web_mod, monkeypatch):
    """Web 层不得重算谓词：core 给了不认识的模式名就原样透传。

    若 Web"聪明地"把它归一到 compatible，面板会把未知通路显示成已知的慢路径 ——
    用户再也看不到"core 与 Web 版本不匹配"这个真实信号。
    """
    metrics = _fast_path_metrics()
    metrics['model_input_pass_mode'] = 'some_future_mode'
    _patch_state_deps(monkeypatch, web_mod, metrics)
    mi = web_mod.collect_web_state()['data']['state']['model_input']
    assert mi['pass_mode'] == 'some_future_mode'


def test_truthy_ints_are_coerced_to_bool(web_mod, monkeypatch):
    """JSON 侧可能是 0/1 而不是 true/false（不同序列化路径），必须归一为 bool。"""
    metrics = _fast_path_metrics()
    metrics['model_fast_path_active'] = 1
    metrics['model_zero_copy_ready'] = 0
    metrics['model_external_dma_bound'] = 1
    _patch_state_deps(monkeypatch, web_mod, metrics)
    mi = web_mod.collect_web_state()['data']['state']['model_input']
    assert mi['fast_path_active'] is True
    assert mi['zero_copy_ready'] is False
    assert mi['external_dma_bound'] is True


def test_fallback_note_is_surfaced_verbatim(web_mod, monkeypatch):
    """回落原因是 core 算好的人话，Web 必须原样端到面板（不得丢弃/改写）。"""
    metrics = _fast_path_metrics()
    metrics['model_input_pass_mode'] = 'compatible'
    metrics['model_input_type_name'] = 'fp16'
    metrics['model_fast_path_active'] = False
    metrics['model_zero_copy_ready'] = False
    metrics['model_input_note'] = '输入为 FP16：快路径只支持 INT8(零均值) 或 UINT8 原生…'
    _patch_state_deps(monkeypatch, web_mod, metrics)
    mi = web_mod.collect_web_state()['data']['state']['model_input']
    assert mi['pass_mode'] == 'compatible'
    assert mi['fast_path_active'] is False
    assert mi['note'].startswith('输入为 FP16')


def test_worker_skew_is_surfaced(web_mod, monkeypatch):
    """3 worker 只有 2 个吃到快路径：计数必须原样透传，供面板显式报偏斜。"""
    metrics = _fast_path_metrics()
    metrics['model_workers_total'] = 3
    metrics['model_workers_fast_path'] = 2
    metrics['model_workers_zero_copy'] = 2
    _patch_state_deps(monkeypatch, web_mod, metrics)
    mi = web_mod.collect_web_state()['data']['state']['model_input']
    assert mi['workers_total'] == 3
    assert mi['workers_fast_path'] == 2
    assert mi['workers_zero_copy'] == 2
    assert mi['fast_path_active'] is True  # "至少一个"语义（不是"全部"）


# ---------------------------------------------------------------------------
# 3. /api/models/select 契约：补齐参照物 applySelectedModel 消费的 4 键
#    （result.config / result.models / result.presets / result.model）。
#    缺任一键 ⇒ 切换模型后「配置表单/模型卡片/预设列表/选中项」静默不刷新。
# ---------------------------------------------------------------------------

def test_select_model_returns_reference_contract_keys(web_mod, monkeypatch, tmp_path):
    monkeypatch.setattr(web_mod, '_license_block',
                        lambda: {'activated': True, 'valid': True, 'ui_brand': 'ttbox'})
    web_mod._ACTIVATION_CACHE['ts'] = 0.0
    monkeypatch.setattr(web_mod, 'PRESETS_DIR', str(tmp_path))
    (tmp_path / 'presetA.json').write_text('{}', encoding='utf-8')

    def fake_ipc(req_type, params=None, timeout=5):
        if req_type == 'MODEL_LIST':
            return {'status': 0, 'data': {
                'selected_model_id': 'm2', 'running_model_id': 'm2', 'state': 'ready',
                'models': [
                    {'model_id': 'm1', 'label': 'One', 'input_width': 640, 'input_height': 640},
                    {'model_id': 'm2', 'label': 'Two', 'input_width': 416, 'input_height': 416},
                ],
            }}
        return {'status': 0, 'data': {}}

    monkeypatch.setattr(web_mod, 'ipc_request', fake_ipc)
    monkeypatch.setattr(web_mod, '_get_runtime_profile', lambda: {'model_id': 'm2'})
    web_mod.app.config['TESTING'] = False
    client = web_mod.app.test_client()

    resp = client.post('/api/models/select', json={'model_id': 'm2'})
    assert resp.status_code == 200, resp.get_data(as_text=True)[:200]
    data = resp.get_json()['data']
    for key in ('config', 'models', 'presets', 'model'):
        assert key in data, f'/api/models/select 缺少参照物所需键: {key}'
    assert [m['id'] for m in data['models']] == ['m1', 'm2']
    assert data['presets'] == ['presetA']
    assert data['model'] and data['model']['id'] == 'm2'
    assert data['config'].get('model_id') == 'm2'
