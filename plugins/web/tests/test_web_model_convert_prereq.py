# test_web_model_convert_prereq.py — ONNX→RKNN 转换链护栏（V1.0.39）
#
# 背景（板端实测发现，2026-10-05）：
#   面板提供「上传 ONNX → 转 RKNN」功能，但**板端三个依赖全部缺失**：
#     · /opt/ttbox/venv-convert/bin/python            （转换器解释器）
#     · /opt/ttbox/current/tools/converter/convert_onnx_to_rknn.py（转换器本体）
#     · /opt/ttbox/calib/imgs/                         （INT8 量化校准集）
#   根因：`deploy/pack_manifest.txt` **没有收录 tools/ 与 calib/**。
#
#   原行为（两个真 bug）：
#     ① 入口只查「有没有文件 / 有没有别的转换在跑」，**不查依赖** ⇒ 用户点了之后
#        线程里 subprocess 抛 FileNotFoundError → 被最外层 `except Exception` 吞成
#        `errno 2 No such file or directory` ⇒ 完全看不出是"转换器没装"。
#     ② `_run_onnx_conversion` 只捕TimeoutExpired，**OSError 不捕** ⇒ 同上。
#
# 本文件钉死：入口必须 fail-fast 且说人话；OSError 必须就地捕获。
import ast
import sys
from pathlib import Path

import pytest

from plugins.web.lib.paths import repo_root as _ttbox_repo_root  # noqa: E402

REPO = Path(_ttbox_repo_root())
if str(REPO) not in sys.path:
    sys.path.insert(0, str(REPO))

CONVERT_PY = REPO / 'plugins' / 'web' / 'lib' / 'model_convert.py'
MODELS_API = REPO / 'plugins' / 'web' / 'api' / 'models.py'
MANIFEST = REPO / 'deploy' / 'pack_manifest.txt'


def _src(p: Path) -> str:
    return p.read_text(encoding='utf-8')


# ------------------------------------------------------------ 依赖探测函数
def test_prerequisites_function_exists_and_is_public():
    """必须有 convert_prerequisites() 供入口调用（依赖清单是入口 fail-fast 的依据）。"""
    from plugins.web.lib.model_convert import convert_prerequisites
    assert callable(convert_prerequisites)


def test_prerequisites_checks_all_three_dependencies():
    """★ 三个依赖都要检查：解释器、转换器脚本、校准图片。缺任一项都转不了。

    判据用**常量名**（函数体里用的是 `_CONVERTER_PYTHON` 等，不是字面路径）——
    拿路径字面量去匹配会假红。
    """
    src = _src(CONVERT_PY)
    fn = src[src.index('def convert_prerequisites'):]
    fn = fn[:fn.index('\ndef ')] if '\ndef ' in fn else fn
    assert '_CONVERTER_PYTHON' in fn, '没检查转换器解释器'
    assert '_CONVERTER_SCRIPT' in fn, '没检查转换器脚本'
    assert '_CONVERT_CALIB_DIR' in fn, '没检查校准数据集目录'
    # 解释器还要判可执行（存在但无 +x 同样跑不起来）
    assert 'os.access' in fn and 'X_OK' in fn, (
        '解释器只判存在不判可执行 —— 存在但无 +x 时仍会失败')
    # 校准目录存在但空 ⇒ 同样算缺（转换器 --dataset-count 5 要有图）
    for ext in ('jpg', 'png', 'jpeg', 'bmp', 'webp'):
        assert ext in fn, f'校准图片格式判定缺 {ext}'


def test_prerequisites_returns_list_of_names():
    """返回缺失项名称列表（供 API 逐条展示），不是 bool（否则只能说"不行"）。"""
    from plugins.web.lib.model_convert import convert_prerequisites
    r = convert_prerequisites()
    assert isinstance(r, list), f'应返回 list，实际 {type(r)}'
    for item in r:
        assert isinstance(item, str) and item, '每项应是非空字符串（要能展示给用户）'


# ------------------------------------------------------------ 入口 fail-fast
def test_import_onnx_endpoint_calls_prerequisites():
    """★ import_onnx 入口必须调 convert_prerequisites() 并在缺依赖时拒绝（不排队、不落盘）。"""
    src = _src(MODELS_API)
    i = src.index('def import_onnx')
    body = src[i:src.index('\ndef ', i + 5)] if '\ndef ' in src[i + 5:] else src[i:]
    assert 'convert_prerequisites()' in body, '入口未调用 convert_prerequisites()'
    assert 'missing' in body, '入口未使用缺失清单（应把 missing 放进 error 消息）'
    # 必须在拿锁/起线程**之前**拒绝，否则照样占资源
    assert body.index('convert_prerequisites()') < body.index('_CONVERT_LOCK'), (
        '依赖检查必须早于加锁，否则缺依赖时仍会占住转换锁')


def test_import_onnx_reports_missing_by_name():
    """错误消息必须点名缺哪个（用户要能据此行动），且带 supported=False 标志。"""
    src = _src(MODELS_API)
    i = src.index('def import_onnx')
    body = src[i:src.index('\ndef ', i + 5)] if '\ndef ' in src[i + 5:] else src[i:]
    assert "'、'.join(missing)" in body, "错误消息未把缺失项逐条列出（应 join 后展示）"
    assert "'supported': False" in body, '未提供 supported=False 标志（前端要据此提示改走 RKNN 上传）'


def test_api_imports_prerequisites():
    """web 侧必须真的 import 到它（漏 import 会在运行时 NameError）。"""
    assert 'from plugins.web.lib.model_convert import convert_prerequisites' in _src(MODELS_API)


# ------------------------------------------------------------ 异常捕获完整性
def test_run_conversion_catches_oserror():
    """★ `_run_onnx_conversion` 必须捕 OSError（解释器不存在 ⇒ FileNotFoundError）。"""
    src = _src(CONVERT_PY)
    i = src.index('def _run_onnx_conversion')
    body = src[i:src.index('\ndef ', i + 5)] if '\ndef ' in src[i + 5:] else src[i:]
    assert 'except subprocess.TimeoutExpired' in body, '丢了 TimeoutExpired 捕获'
    assert 'except OSError' in body, (
        '★ 缺 OSError 捕获：解释器/脚本不存在时 FileNotFoundError 会冒到最外层，'
        '用户只看到 errno 2')


def test_run_conversion_logs_reason():
    """失败原因必须落进 log_lines（否则状态只剩一句"转换失败"，无法定位）。"""
    src = _src(CONVERT_PY)
    i = src.index('def _run_onnx_conversion')
    body = src[i:src.index('\ndef ', i + 5)] if '\ndef ' in src[i + 5:] else src[i:]
    assert 'log_lines.append' in body, '失败时未记录日志'
    assert 'returncode' in body, '未记录转换器 returncode'


# ------------------------------------------------------------ 磁盘泄漏（转换链）
def test_convert_chain_unlinks_incoming():
    """★ 转换链的 _incoming 落点必须无条件删除（与直传链 models.py 同模式）。"""
    src = _src(CONVERT_PY)
    i = src.index('def _conversion_worker')
    body = src[i:] if '\ndef ' not in src[i:] else src[i:src.index('\ndef ', i + 5)]
    assert 'incoming_rknn.unlink' in body, (
        '★ 转换链未删除 _incoming 落点 ⇒ 每次 ONNX 转换永久留 4~11MB rknn'
        '（V1.0.39 已在直传链修掉，此处是同一模式的第二处）')
    # 必须在 finally 里（覆盖三条 return 失败分支 + 成功路径）
    fi = body.find('finally:')
    assert fi != -1, '转换链的 import 事务没有 finally'
    tail = body[fi:]
    assert 'incoming_rknn.unlink' in tail, 'unlink 不在 finally 里 ⇒ 失败分支仍会泄漏'


# ------------------------------------------------------------ 缺口登记（防遗忘）
def test_manifest_gap_is_documented_in_comments():
    """★ 打包清单缺 tools/ 与 calib/ 是**已知缺口**，必须在代码里留登记。

    将来若有人把 tools/converter + calib/imgs 收进 pack_manifest，本测试
    仍应通过（登记是"我们知道这个缺口"的事实陈述，不是断言缺口永远存在）。
    """
    src = _src(CONVERT_PY) + _src(MODELS_API)
    assert 'pack_manifest' in src, (
        '未登记打包缺口（tools/ 与 calib/ 未收录）—— 后人不知道板端为何转不了')
