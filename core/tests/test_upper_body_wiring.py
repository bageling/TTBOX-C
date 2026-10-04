# -*- coding: utf-8 -*-
"""V1.0.24「只要人物上半身主体」：**接线来源 + 无开关口径**的源码级护栏（core 侧）。

★ 为什么这层必须存在：
  ① 「贴边判定用收缩框还是全身框」在**落点层面不可观测** —— 落点等效换算
     （offset ÷k、框高 ×k）让两种取值落在同一像素，冻结又优先于外推。
     两次端到端尝试都失败（实测数据已写进 test_upper_body.cpp 文件头）。
  ② 「显示框不再做多框并集」在**有并集对象时**才有观测面；纯函数测试里
     detections 只有本体框 ⇒ 断言恒成立。所以这三条只能源码级钉。
  手法照 core/tests/test_capture_open_wait_policy.py。

★ **业主口径（2026-10-04 明确）**：不要开关、不要新增参考物。
  所以下面 `test_no_config_or_panel_switch_exists` 是**口径护栏** ——
  谁再把 upper_body 做成配置项/面板开关，这条会红。
"""
from __future__ import annotations

import re
from pathlib import Path

from plugins.web.lib.paths import discover_root

REPO = Path(discover_root(__file__))

THREAD = REPO / 'core' / 'src' / 'aim' / 'AimThread.cpp'
PROFILE = REPO / 'core' / 'src' / 'model' / 'RuntimeProfile.cpp'
TYPES = REPO / 'core' / 'src' / 'mouse' / 'MouseTypes.hpp'
POINT = REPO / 'core' / 'src' / 'mouse' / 'AimPointProfile.cpp'
POINT_H = REPO / 'core' / 'src' / 'mouse' / 'AimPointProfile.hpp'
SELECTOR = REPO / 'core' / 'src' / 'mouse' / 'TargetSelector.cpp'
PREV_CPP = REPO / 'core' / 'src' / 'preview' / 'PreviewModule.cpp'
PREV_H = REPO / 'core' / 'src' / 'preview' / 'PreviewModule.hpp'
RUNTIME = REPO / 'core' / 'src' / 'runtime' / 'CoreRuntime.cpp'


def _src(p: Path) -> str:
    return p.read_text(encoding='utf-8')


def _code_only(text: str) -> str:
    """剥掉注释与字符串字面量 —— 否则注释里的 `aim_box_src` 会喂饱断言。
    ★ 顺序不能反：先剥 `//` 注释，再剥 `"…"`。反过来时注释里的中文引号
      （如 `当"刚刚关过"`）会被当字符串起点，把后面半段代码整段吃掉。"""
    text = re.sub(r'//[^\n]*', '', text)
    text = re.sub(r'"(?:[^"\\\n]|\\.)*"', '""', text)
    return text


def _span(text: str, start_marker: str, end_marker: str, limit: int = 3000) -> str:
    i = text.index(start_marker)
    j = text.index(end_marker, i)
    assert j - i < limit, '切片 %d 字符，疑似 end_marker 失配' % (j - i)
    return text[i:j + len(end_marker)]


# ---------------------------------------------------------------- 无开关口径
def test_no_config_or_panel_switch_exists():
    """★ 口径护栏：上半身比例是**算法常量**，不得作为配置项/面板开关存在。
    业主 2026-10-04 明确：「不要乱加东西，不要开关，要校正算法/数学/几何」。"""
    for path in (TYPES, PROFILE):
        code = _code_only(_src(path))
        assert 'upper_body' not in code, (
            '%s 里出现了 upper_body 配置字段 —— 业主明确不要开关/配置项，'
            '比例只能是算法常量（kUpperBodyRatio）' % path.name)
    web = (_src(REPO / 'plugins' / 'web' / 'lib' / 'profile_translate.py')
           + _src(REPO / 'plugins' / 'web' / 'templates' / 'index.html')
           + _src(REPO / 'plugins' / 'web' / 'static' / 'panel' / '00-const.js')
           + _src(REPO / 'plugins' / 'web' / 'static' / 'panel' / '10-flow.js'))
    assert 'upper_body' not in web, 'web 侧还有 upper_body 透传/面板项 —— 业主不要开关'


def test_ratio_lives_in_the_cpp_as_a_constant():
    code = _code_only(_src(POINT))
    m = re.search(r'constexpr\s+float\s+kUpperBodyRatio\s*=\s*([\d.]+)f\s*;', code)
    assert m, '比例必须是 AimPointProfile.cpp 里的 constexpr（算法常量）'
    k = float(m.group(1))
    assert 0.05 < k <= 0.6, (
        '比例 %s 越界：人体几何依据是头顶到髋≈0.5；>0.6 会切进胸口（落点被顶高，'
        '正是 V1.0.09 修过的症状）' % k)
    assert 'float upper_body_ratio() { return kUpperBodyRatio; }' in code, \
        '对外只暴露读常量的函数'


# ---------------------------------------------------------------- 收缩接线
def test_shrink_is_called_once_per_frame():
    code = _code_only(_src(THREAD))
    hits = re.findall(
        r'shrink_to_upper_body\(\s*selected\.box\s*,\s*aim_point\s*,\s*&aim_box_src\s*,\s*&prof_ub\s*\)',
        code)
    assert len(hits) == 1, (
        'shrink_to_upper_body 的调用应恰好一处（每帧一次，喂 aim_box_src/prof_ub），'
        '实得 %d 处' % len(hits))


def test_shrink_declaration_is_before_target_ok_block():
    """收缩声明必须在 `if (target_ok)` 块**外** —— 拟人化段（set_target_radius_px）
    与 status 段（显示框）都在该块外也消费它，放块内会编译不过。"""
    code = _code_only(_src(THREAD))
    decl = code.index('DetectionBox aim_box_src = selected.box;')
    gate = code.index('if (target_ok) {')
    assert decl < gate, '收缩声明落在 target_ok 块内 ⇒ 块外消费点拿不到它'


def test_clip_judgment_reads_the_shrunk_box():
    block = _code_only(_span(_src(THREAD), 'const bool box_bottom_clipped =', ';'))
    assert 'aim_box_src' in block, '贴边判定没有读收缩框 aim_box_src'
    assert 'selected.box' not in block, '贴边判定里出现了 selected.box ⇒ 两套坐标系混了'


def test_clip_ratio_tracker_observes_the_shrunk_box():
    block = _code_only(_span(_src(THREAD), 'clip_ratio_tracker_.observe(', ');'))
    assert block.count('aim_box_src') >= 2, (
        '身高自校准比必须观察收缩框的宽/高（外推在收缩域消费）：%r' % block)
    assert 'selected.box' not in block


def test_frozen_rect_observes_the_shrunk_box():
    block = _code_only(_span(_src(THREAD), 'frozen_rect_.observe(', ');'))
    assert 'aim_box_src' in block, '冻结框记录的是全身框 ⇒ 冻结/实时两套域，落点会跳'
    assert 'selected.box' not in block


def test_aim_point_uses_mapped_profile_and_shrunk_box():
    block = _code_only(_span(_src(THREAD), 'if (!aim_point_at(', ');'))
    assert 'prof_ub' in block, (
        '落点必须用换算后的 prof_ub（offset ÷k）；用 aim_point 会让落点整体上飘 k 倍身高')
    assert re.search(r'aim_point_at\(\s*aim_box\b', block), '落点用的框必须是 aim_box（收缩/冻结域）'


def test_head_aim_constraint_uses_mapped_profile():
    block = _code_only(_span(_src(THREAD), 'constrain_aim_point_to_head(', ');'))
    assert 'prof_ub' in block, '头区约束要用换算后的 prof_ub（fraction 是相对框高的量）'


def test_box_h_and_target_radius_use_shrunk_box():
    code = _code_only(_src(THREAD))
    assert 'aim_box_src' in _span(code, 'tracker_.set_box_h(', ');'), \
        'set_box_h（滤波自适应/遥测）要与控制域同框'
    assert 'aim_box_src' in _span(code, 'personal_shader_.set_target_radius_px(', ');'), \
        '拟人化目标半径要与控制域同框'


# ---------------------------------------------------------------- 显示框
def test_display_box_uses_the_shrunk_box_not_the_union():
    """★ 显示框 = 控制用的上半身框，**不再做多框并集**。

    旧实现把同一目标身上所有框并起来（身体框 + 头框）⇒ 面板上那个"又大又含头"的框，
    正是业主说的「框是全身加头部」的来源 —— 显示与控制不是同一个几何，参考物失真。"""
    code = _code_only(_src(THREAD))
    blk = _span(code, 'float display_x1 =', 'float display_y2 = aim_box_src.y2;')
    assert blk.count('aim_box_src') == 4, (
        '显示框四条边都必须取自 aim_box_src（= 控制链在用的那个框），实得 %r' % blk)
    assert 'selected.box' not in blk, '显示框又用回原始框了'
    # 并集循环（找同目标其它框）必须整段消失：三个判据变量是它的指纹。
    whole = _code_only(_src(THREAD))
    for fingerprint in ('vertical_overlap', 'horizontal_near', 'vertical_near'):
        assert fingerprint not in whole, (
            '显示框的多框并集循环复活了（%s 还在）—— 那会让框重新变成"全身加头"'
            % fingerprint)


# ---------------------------------------------------------------- 纯函数契约
def test_shrink_function_rescales_every_ratio_relative_to_box_height():
    code = _code_only(_src(POINT))
    assert re.search(r'out_box->y2\s*=\s*box\.y1\s*\+\s*k\s*\*\s*h', code), '框底没按 k 收缩'
    for pat, why in [
        (r'out_prof->offset_y\s*/=\s*k', '默认落点比例'),
        (r'for\s*\(\s*auto&\s*c\s*:\s*out_prof->class_offsets\s*\)\s*c\.offset_y\s*/=\s*k',
         '类别偏移'),
        (r'out_prof->head_aim\.head_offset_top_fraction\s*/=\s*k', '头区顶 fraction'),
        (r'out_prof->head_aim\.head_height_fraction\s*/=\s*k', '头区高 fraction'),
        (r'out_prof->body_w_over_h\s*/=\s*k', '外推兜底宽高比'),
    ]:
        assert re.search(pat, code), '等效换算漏了：%s 未 ÷k' % why


def test_shrink_is_unconditional():
    """收缩无条件生效：函数体里不能有「开关/参数为真才收」的分支。"""
    body = _code_only(_span(_src(POINT),
                            'bool shrink_to_upper_body(', '\n}\n'))
    assert 'kUpperBodyRatio' in body, '收缩比例必须直接用算法常量'
    assert not re.search(r'prof\.[a-z_]*enabled', body), \
        '函数体里出现配置开关字段 ⇒ 又做成可关的了（业主不要开关）'
    # 判据要抓的是「跳过收缩还报成功」的洞：提前 return 只允许是 `false`（走原框），
    # `return true` 只能出现一次且在函数末尾。
    # （函数体里有两个提前 false 是对的：空指针 / 退化框 ⇒ 原样返回。）
    returns = re.findall(r'return\s+(true|false)\s*;', body)
    assert returns[-1:] == ['true'], '收缩成功路径必须在末尾：%r' % returns
    assert all(r == 'false' for r in returns[:-1]), (
        '提前 return 只能是 false（输入无效⇒原样返回），实得 %r' % returns)


# ================================================================= 口径：算法不依赖 class 语义
def test_algorithm_must_not_hardcode_class_semantics():
    """★★ 业主 2026-10-04 定的产品口径：**算法不能依赖 class 语义**。

    理由（他自己的原话）：「我们无法控制用户使用的模型，用户使用的模型不一定有
    上半身框这个东西」。TTBOX 卖的是**盒子**，模型由客户自选 ⇒ class 数与语义都不可控
    （现役 `sjzv11___1` 的 `class_names` 本来就是空的）。
    ⇒ 凡是「class 5 = 全身 / class 8 = 上半身」这种假设，换个模型就全错。

    允许：按**配置值**比较（cfg.hb_body1 / 用户的 class_offsets 表）。
    禁止：按**字面量**比较（class_id == 0 / == 1 …）—— 那是把某份数据集的约定写进算法。
    前科：V1.0.25 之前 `PreviewModule::draw_boxes` 就是「class 1 加粗 + 标成头色」
    （出厂 2 类模型 EP 的约定），7 类模型下纯误导，已删。
    """
    for path in (POINT, SELECTOR, PREV_CPP):
        code = _code_only(_src(path))
        hits = re.findall(r'class_id\s*[!=]=\s*\d+', code)
        assert not hits, (
            '%s 里有按 class_id **字面量**的比较 %s —— 业主口径：算法不得依赖 class 语义'
            '（模型由客户自选，class 数与语义都不可控）。要按类别区分请走配置项。'
            % (path.name, hits))


def test_geometry_pairing_ignores_class_id():
    """几何配对识头必须纯几何（大框+小框的包含关系），不许按 class 号区分头/身。"""
    body = _code_only(_src(POINT))
    m = re.search(r'bool resolve_head_box\(.*?\n\}', body, re.S)
    assert m, '找不到 resolve_head_box'
    fn = m.group(0)
    assert 'class_id' not in fn, (
        'resolve_head_box 读了 class_id —— 它必须纯几何判据（包含/面积/上半部），'
        '这样换任何模型都成立')
