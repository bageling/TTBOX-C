# test_web_fitts_param_surface.py — Fitts 控制器参数面四层接线护栏（V1.0.43）
#
# 背景（这个文件是因为一次真实事故写的）：V1.0.43 发了 Fitts 控制器，但**只改了 core 层**，
# 面板（模板/JS/白名单/翻译层）一行没动 ⇒ 上板后
#   ① 控制器确实切到了 fitts（controller_type 缺省），但面板显示的还是 pid1 的旧参数；
#   ② 改那些旧参数对 Fitts 完全无效（core 不消费）；
#   ③ Fitts 的四个参数在面板上一个入口都没有，改不了。
#   业主反馈「感觉没有调参页面没有变化」—— 正是这个缺口。
#
# 本文件把「core ↔ 面板」四层接线钉死，任何一层漏掉都会红：
#   ① 模板 index.html：有控件 + data-config（能被配置链路收走）
#   ② 00-const.js：取值范围 + 默认值（范围防手滑填出离谱值，默认值须与 core 一致）
#   ③ 10-flow.js：collect（面板→core 提交）与回填（core→面板）双向都接
#   ④ 后端：白名单（web→core 不被丢弃）+ 翻译层（core→web 有投影）
import json
import re
import sys
from pathlib import Path

import pytest

# 路径单点真源（A-PATH-3）：不写 parents[N] 层级硬编码（门禁第⑩项会 FAIL）
from plugins.web.lib.paths import repo_root as _ttbox_repo_root  # noqa: E402
REPO = Path(_ttbox_repo_root())
if str(REPO) not in sys.path:
    sys.path.insert(0, str(REPO))

FITTS_FIELDS = ('fitts_a_ms', 'fitts_b_ms', 'fitts_deadzone_px', 'fitts_ff_gain')
# 出厂默认值：必须与 core/src/mouse/MouseTypes.hpp 的 MouseProfile 逐个一致
FITTS_DEFAULTS = {
    'fitts_a_ms': 20.0,
    'fitts_b_ms': 20.0,
    'fitts_deadzone_px': 3.0,
    'fitts_ff_gain': 0.6,
}
CONTROLLER_TYPE_DEFAULT = 'fitts'


@pytest.fixture(scope='module')
def html():
    return (REPO / 'plugins/web/templates/index.html').read_text(encoding='utf-8')


@pytest.fixture(scope='module')
def const_js():
    return (REPO / 'plugins/web/static/panel/00-const.js').read_text(encoding='utf-8')


@pytest.fixture(scope='module')
def flow_js():
    return (REPO / 'plugins/web/static/panel/10-flow.js').read_text(encoding='utf-8')


# ---------------------------------------------------------------- ① 模板
def test_panel_has_controller_selector(html):
    """面板必须有控制器下拉（fitts/pid1 二选一），否则用户无法回退。"""
    assert 'id="controller_type"' in html, '面板缺少控制器选择（无法在 fitts/pid1 间切换）'
    seg = html[html.index('id="controller_type"') - 200: html.index('id="controller_type"') + 600]
    assert 'value="fitts"' in seg, '下拉缺少 fitts 选项'
    assert 'value="pid1"' in seg, '下拉缺少 pid1 备选项'
    assert 'data-config' in seg, '下拉缺 data-config（配置链路收不到）'


@pytest.mark.parametrize('field', FITTS_FIELDS)
def test_panel_has_fitts_input_with_data_config(html, field):
    """Fitts 四参各要有输入框 + data-config（漏 data-config ⇒ 面板改了不提交）。"""
    m = re.search(r'id="%s"[^>]*data-config' % field, html)
    assert m, f'面板缺少 #{field} 输入框，或它没有 data-config'


@pytest.mark.parametrize('field', FITTS_FIELDS)
def test_panel_fitts_input_has_range_step(html, field):
    """输入框要有 step（否则没法微调小数；ff_gain 是 0.05 粒度）。"""
    m = re.search(r'id="%s"[^>]*type="number"[^>]*step="([^"]+)"' % field, html)
    assert m, f'#{field} 缺 type=number/step'


def test_panel_still_has_pid1_fields_as_fallback(html):
    """老 pid 五参必须保留（切回 pid1 时要能用），不能因为改面板就删掉。"""
    for fid in ('controller_kp', 'controller_kd', 'controller_predict',
                'controller_predict_y', 'controller_rate', 'controller_smooth'):
        assert f'id="{fid}"' in html, f'面板丢失老 pid 参数 #{fid}（切回 pid1 就没得调了）'


# ---------------------------------------------------------------- ② 常量
@pytest.mark.parametrize('field', FITTS_FIELDS)
def test_const_js_has_range(const_js, field):
    """必须有取值范围：范围缺失 ⇒ 前端不做夹取，core 侧 clamp 又更宽，脏值直通。"""
    m = re.search(r'%s:\s*\[([^\]]+)\]' % field, const_js)
    assert m, f'00-const.js 缺 {field} 的取值范围'


@pytest.mark.parametrize('field,expect', sorted(FITTS_DEFAULTS.items()))
def test_const_js_default_matches_core(const_js, field, expect):
    """默认值必须与 core MouseProfile 一致（口径不一处 ⇒ 面板显示的值 ≠ 板端生效值）。"""
    m = re.search(r'\b%s:\s*([0-9.]+)' % field, const_js)
    assert m, f'00-const.js 的 CONTROLLER_DEFAULTS 缺 {field}'
    assert float(m.group(1)) == pytest.approx(expect), (
        f'{field} 面板默认值 {m.group(1)} ≠ core 出厂 {expect}')


def test_const_js_controller_type_default(const_js):
    m = re.search(r'controller_type:\s*"(\w+)"', const_js)
    assert m, 'CONTROLLER_DEFAULTS 缺 controller_type'
    assert m.group(1) == CONTROLLER_TYPE_DEFAULT, (
        f'controller_type 面板默认 {m.group(1)} ≠ core 缺省 {CONTROLLER_TYPE_DEFAULT}')


def test_const_js_ff_gain_cap_matches_core_hard_clamp(const_js):
    """ff 上限必须是 0.85（FittsAimController 内的硬钳）。

    若面板上限比它大，用户填 1.0 看着"生效了"，实际被 core 悄悄钳回 0.85
    ⇒ 又是一次「改了没反应」。"""
    m = re.search(r'fitts_ff_gain:\s*\[([0-9.]+),\s*([0-9.]+)\]', const_js)
    assert m, '00-const.js 缺 fitts_ff_gain 范围'
    assert float(m.group(2)) == pytest.approx(0.85), (
        f'面板 ff 上限 {m.group(2)} ≠ core 硬钳 0.85 ⇒ 超出的值会被 core 静默改回')


# ---------------------------------------------------------------- ③ 收发
@pytest.mark.parametrize('field', FITTS_FIELDS)
def test_flow_collects_fitts_field(flow_js, field):
    """collect（面板→core）必须读该字段：漏了 ⇒ 面板填的值不提交。"""
    m = re.search(r'%s:\s*getNumber\(\s*"%s"' % (field, field), flow_js)
    assert m, f'10-flow.js collectConfig 未收集 {field}（面板改了不提交）'


def test_flow_collects_controller_type(flow_js):
    assert re.search(r'controller_type:\s*getString\(\s*"controller_type"', flow_js), \
        '10-flow.js collectConfig 未收集 controller_type（下拉选了不提交）'


@pytest.mark.parametrize('field', FITTS_FIELDS)
def test_flow_populates_fitts_field(flow_js, field):
    """回填（core→面板）必须写该字段：漏了 ⇒ 永远显示默认值，看不到真实生效值。"""
    m = re.search(r'setValue\(\s*"%s"' % field, flow_js)
    assert m, f'10-flow.js 未回填 {field}（面板看不到板端真实值）'


def test_flow_populates_controller_type(flow_js):
    assert re.search(r'setValue\(\s*"controller_type"', flow_js), \
        '10-flow.js 未回填 controller_type'


# ---------------------------------------------------------------- ④ 后端
def test_backend_whitelist_keeps_fitts_fields():
    """web→core 白名单：不在表里的键会被**静默丢弃**（面板改了不生效的经典坑）。"""
    from plugins.web.lib.controller_params import CONTROLLER_NUMS
    for f in FITTS_FIELDS:
        assert f in CONTROLLER_NUMS, f'CONTROLLER_NUMS 缺 {f} ⇒ 面板填的值到不了 core'


def test_backend_whitelist_declares_controller_type():
    """controller_type 是字符串，必须单独白名单化（不能塞进数值表）。"""
    from plugins.web.lib.controller_params import CONTROLLER_STRINGS
    assert 'controller_type' in CONTROLLER_STRINGS, \
        'CONTROLLER_STRINGS 缺 controller_type ⇒ 面板选了控制器不提交'


def test_web_to_core_profile_carries_fitts_fields():
    """端到端：面板 body → core profile，五个键都要在（真跑翻译层，不做正则猜测）。"""
    from plugins.web.lib.profile_translate import web_body_to_profile
    body = {
        'ai': {'controller': {
            'controller_type': 'fitts',
            'fitts_a_ms': 12,
            'fitts_b_ms': 28,
            'fitts_deadzone_px': 1.5,
            'fitts_ff_gain': 0.75,
        }},
    }
    prof = web_body_to_profile(body)
    mouse = (prof.get('mouse') or {})
    assert mouse.get('controller_type') == 'fitts'
    assert mouse.get('fitts_a_ms') == pytest.approx(12.0)
    assert mouse.get('fitts_b_ms') == pytest.approx(28.0)
    assert mouse.get('fitts_deadzone_px') == pytest.approx(1.5)
    assert mouse.get('fitts_ff_gain') == pytest.approx(0.75)


def test_web_to_core_rejects_dirty_controller_type():
    """非法 controller_type 必须回退 'fitts'，不能透传成第三态（core 会静默不工作）。"""
    from plugins.web.lib.profile_translate import web_body_to_profile
    for dirty in ('', 'PID_ONE', 'cascade', 'legacy', '123'):
        prof = web_body_to_profile({'ai': {'controller': {'controller_type': dirty}}})
        got = (prof.get('mouse') or {}).get('controller_type')
        assert got == 'fitts', f'controller_type={dirty!r} 应回退 fitts，实得 {got!r}'


def test_core_to_web_profile_exposes_fitts_fields():
    """core→web 投影：GET_CONFIG 回来必须带这五个键，否则面板显示默认值。"""
    from plugins.web.lib.profile_translate import profile_to_web
    prof = {
        'mouse': {
            'controller_type': 'fitts',
            'fitts_a_ms': 20, 'fitts_b_ms': 20,
            'fitts_deadzone_px': 3, 'fitts_ff_gain': 0.6,
        },
    }
    web = profile_to_web(prof)
    ctrl = (web.get('ai') or {}).get('controller') or {}
    assert ctrl.get('controller_type') == 'fitts'
    for f in FITTS_FIELDS:
        assert f in ctrl, f'profile_to_web 漏投影 {f} ⇒ 面板永远显示默认值'
        assert ctrl[f] == pytest.approx(FITTS_DEFAULTS[f])


def test_fitts_core_defaults_match_panel_defaults():
    """core 出厂值 vs 面板默认值：逐字段比对（这是「面板显示 = 板端生效」的唯一保证）。

    直接读 core 源码里的 MouseProfile 缺省字面量 —— core 改默认值而面板没跟，
    这条会红。"""
    src = (REPO / 'core/src/mouse/MouseTypes.hpp').read_text(encoding='utf-8')
    for field, expect in FITTS_DEFAULTS.items():
        m = re.search(r'float\s+%s\s*=\s*([0-9.]+)f' % field, src)
        assert m, f'MouseTypes.hpp 找不到 {field} 的缺省值（core 侧被删/改名了？）'
        assert float(m.group(1)) == pytest.approx(expect), (
            f'core {field}={m.group(1)} ≠ 面板默认 {expect} ⇒ 面板显示的不是板端生效值')
    m = re.search(r'std::string\s+controller_type\s*=\s*"(\w+)"', src)
    assert m, 'MouseTypes.hpp 找不到 controller_type 缺省'
    assert m.group(1) == CONTROLLER_TYPE_DEFAULT
