# test_web_pid_smooth_wiring.py — PID smooth 面板↔Core 通路单测（V1.0.38）
#
# 背景（业主令「pid 以 pid1 为准完全移植」）：
#   V1.0.13~V1.0.37 期间 core 删掉了 smooth 字段并把"削弱倍率"折算进 kp，
#   面板因此**没有** smooth 输入框、翻译层也没有这两个键。
#   V1.0.38 回归 pid1 完全移植后，smooth 重新成为 Pid1Controller 第 5 参
#   （core/src/aim/AimThread.cpp:274-277），于是面板必须**四处**同时补齐：
#     ① index.html      有 #controller_smooth 输入框
#     ② 00-const.js     有 controller_smooth 的取值范围 + 默认值
#     ③ 10-flow.js      回填(setValue) + 写回(collectConfig) 都带 smooth
#     ④ 翻译层/后端白名单 profile_translate.py / controller_params.py 都有 smooth_x/y
#   ★ 这四处任缺其一，面板就会"能填但存不进去"或"回填永远空"——而且**静默**：
#     pytest 全绿、面板也不报错，只表现为"改了没反应"。
#   本文件把这四处逐条钉死。
#
# 有效性契约（必须与 core 一致，见 core/src/model/RuntimeProfile.cpp）：
#   smooth ∈ [0,9999]；9900 = soft-limit 压到 1%（pid1 出厂值）；0 = 关闭保护。
#   smooth 与 kp 的关系：生效 = 名义 × (10000-smooth)/10000。
import re
from pathlib import Path

import pytest

from plugins.web.lib.paths import repo_root

REPO = Path(repo_root())
HTML = REPO / 'plugins' / 'web' / 'templates' / 'index.html'
CONST_JS = REPO / 'plugins' / 'web' / 'static' / 'panel' / '00-const.js'
FLOW_JS = REPO / 'plugins' / 'web' / 'static' / 'panel' / '10-flow.js'
POINTER_JS = REPO / 'plugins' / 'web' / 'static' / 'panel' / '03-pointer.js'
TRANSLATE_PY = REPO / 'plugins' / 'web' / 'lib' / 'profile_translate.py'
CTRL_PARAMS_PY = REPO / 'plugins' / 'web' / 'lib' / 'controller_params.py'
MOUSE_TYPES = REPO / 'core' / 'src' / 'mouse' / 'MouseTypes.hpp'
RUNTIME_PROFILE = REPO / 'core' / 'src' / 'model' / 'RuntimeProfile.cpp'
AIM_THREAD = REPO / 'core' / 'src' / 'aim' / 'AimThread.cpp'


def _src(p: Path) -> str:
    return p.read_text(encoding='utf-8')


# ---------------------------------------------------------------- ① 输入框存在
def test_panel_has_smooth_input():
    """面板必须有 #controller_smooth 输入框，且带 data-config（能被配置链路收走）。"""
    html = _src(HTML)
    assert 'id="controller_smooth"' in html, '面板缺少 smooth 输入框'
    seg = html[html.index('id="controller_smooth"') - 200: html.index('id="controller_smooth"') + 200]
    assert 'data-config' in seg, 'smooth 输入框缺 data-config'


def test_smooth_hint_explains_zero_disables_protection():
    """★ smooth=0 会**关掉** pid1 的大误差保护，这是最危险的误操作，必须写在提示里。"""
    html = _src(HTML)
    i = html.index('id="controller_smooth"')
    hint = html[i:i + 900]
    assert '0' in hint and ('关' in hint or '保护' in hint), (
        'smooth 提示必须说明「调到 0 会关闭大误差保护」')


# ---------------------------------------------------------------- ② 范围与默认值
def test_controller_smooth_has_range_and_default():
    js = _src(CONST_JS)
    m = re.search(r'controller_smooth:\s*\[([^\]]+)\]', js)
    assert m, '00-const.js 缺 controller_smooth 的取值范围'
    lo, hi = (float(x) for x in m.group(1).split(','))
    assert lo == 0.0
    # 上限必须与 core 的 [0,9999] 校验同界（RuntimeProfile.cpp:validate）
    assert hi == 9999.0, f'smooth 上限应与 core 校验一致(9999)，实际 {hi}'

    d = re.search(r'CONTROLLER_DEFAULTS\s*=\s*\{(.*?)\n\}', js, re.S)
    assert d, '找不到 CONTROLLER_DEFAULTS'
    assert re.search(r'\bsmooth:\s*9900', d.group(1)), (
        'CONTROLLER_DEFAULTS.smooth 必须是 pid1 出厂值 9900')


def test_panel_kp_range_is_nominal_domain():
    """★ kp 已是**名义值**（出厂 25、标定写回约 10.2），上限不能还是折算口径的 1.0。"""
    js = _src(CONST_JS)
    m = re.search(r'controller_kp:\s*\[([^\]]+)\]', js)
    assert m, '找不到 controller_kp 范围'
    hi = float(m.group(1).split(',')[1])
    assert hi >= 10.0, (
        f'controller_kp 上限 {hi} 太小：标定会写回约 10.2、pid1 出厂 25，都会被卡死')


def test_panel_defaults_match_core_struct_defaults():
    """★ 面板默认值必须与 core MouseProfile 结构体默认值一字不差（口径不同源=首次回填显示另一套）。"""
    js = _src(CONST_JS)
    mt = _src(MOUSE_TYPES)
    pairs = [
        ('kp: ', r'float kp_x\s*=\s*([\d.]+)f'),
        ('kd: ', r'float kd_x\s*=\s*([\d.]+)f'),
        ('predict: ', r'float predict_x\s*=\s*([\d.]+)f'),
        ('smooth: ', r'float smooth_x\s*=\s*([\d.]+)f'),
    ]
    for js_key, core_re in pairs:
        jm = re.search(re.escape(js_key.strip()) + r'\s*([\d.]+)', js)
        cm = re.search(core_re, mt)
        assert jm, f'00-const.js 缺 {js_key.strip()} 默认值'
        assert cm, f'MouseTypes.hpp 找不到 {core_re}'
        assert float(jm.group(1)) == float(cm.group(1)), (
            f'{js_key.strip()} 面板={jm.group(1)} 与 core={cm.group(1)} 不同源')


# ---------------------------------------------------------------- ③ 前端读写
def test_flow_js_reads_and_writes_smooth():
    """10-flow.js 必须**双向**带 smooth：回填 setValue + 写回 collectConfig，缺一即半残。"""
    js = _src(FLOW_JS)
    assert re.search(r'setValue\(\s*"controller_smooth"', js), '缺 smooth 回填（GET_CONFIG 后面板永远空）'
    assert re.search(r'smooth_x:\s*getNumber\(\s*"controller_smooth"', js), '缺 smooth 写回（填了存不进去）'
    assert re.search(r'smooth_y:\s*getNumber\(\s*"controller_smooth"', js), (
        'smooth_y 未共用同一个输入框 —— 与 kp/kd/rate 的两轴共用口径不一致')


def test_restore_defaults_includes_smooth():
    """「恢复默认」也必须带上 smooth，否则恢复后输入框残留旧值。"""
    js = _src(POINTER_JS)
    assert re.search(r'setValue\(\s*"controller_smooth"', js), (
        'setMovementControlDefaultsToForm 漏了 smooth —— 恢复默认后输入框会残留旧值')


# ---------------------------------------------------------------- ④ 翻译层/后端
def test_translate_layer_carries_smooth():
    """profile_translate 的 core→web 投影必须有 smooth_x/y，否则回填拿不到值。"""
    py = _src(TRANSLATE_PY)
    assert "'smooth_x'" in py and "'smooth_y'" in py, (
        'profile_translate.py 未投影 smooth —— core 有值但面板拿不到')


def test_backend_whitelist_carries_smooth():
    """controller_params 的 web→core 搬运表必须有 smooth_x/y，否则填的值到不了 core。"""
    py = _src(CTRL_PARAMS_PY)
    assert "'smooth_x': 'smooth_x'" in py and "'smooth_y': 'smooth_y'" in py, (
        'CONTROLLER_NUMS 漏了 smooth —— 面板填的值存不进 core（静默失败）')


# ---------------------------------------------------------------- ⑤ core 侧确实在消费
def test_core_actually_consumes_smooth():
    """面板链通了还不够：core 必须真的把 smooth 喂给 Pid1Controller，否则整条链是空转。"""
    at = _src(AIM_THREAD)
    assert re.search(r'pid_x_\.configure\([^;]*smooth_x', at, re.S), (
        'AimThread 未把 smooth_x 传给 Pid1Controller 第 5 参')
    assert re.search(r'pid_y_\.configure\([^;]*smooth_y', at, re.S), (
        'AimThread 未把 smooth_y 传给 Pid1Controller 第 5 参')


def test_core_persists_smooth():
    """RuntimeProfile 必须序列化 smooth —— 不落盘则面板调整重启即丢。"""
    rp = _src(RUNTIME_PROFILE)
    assert re.search(r'm\.set\(\s*"smooth_x"', rp), 'to_json 未写 smooth_x（配置不落盘）'
    assert re.search(r'obj_num\(\s*\*m,\s*"smooth_x"', rp), 'from_json 未读 smooth_x'


@pytest.mark.parametrize('source,label', [
    (HTML, 'index.html'),
    (CONST_JS, '00-const.js'),
    (FLOW_JS, '10-flow.js'),
    (TRANSLATE_PY, 'profile_translate.py'),
    (CTRL_PARAMS_PY, 'controller_params.py'),
])
def test_no_legacy_folded_kp_wording(source, label):
    """★ 反向锁：V1.0.13 的「真实有效值 / 已折叠」措辞不许残留在面板链路里。

    口径已经翻回 pid1 原值，注释若还说"这是生效值、smooth 已删"，会误导下一个
    接手的人把 kp 又当生效值调（差 100 倍）。
    """
    text = _src(source)
    for bad in ('真实有效值', '真实有效值（count', 'smooth 从参数面删除', '已折叠进 kp'):
        assert bad not in text, (
            f'{label} 仍含 V1.0.13 折算口径的措辞「{bad}」—— 口径已翻回 pid1 名义值')
