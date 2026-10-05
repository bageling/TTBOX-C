# -*- coding: utf-8 -*-
"""V1.0.31 预览三层（对照 GitHub sunone_aimbot）的源码级护栏。

★ 为什么只能源码级：`PreviewModule` 依赖 opencv，**host 不编译**（与 RKNNEngine /
  WorkerPool / V4L2Capture / TtboxLicenseClient 同批被排除）⇒ 本机 ctest 对它零覆盖。

★ 演进（留着当反面教材）：
  - V1.0.25 我把预览改成「只画实际瞄准的那一个框」（业主当时的口径）。
  - V1.0.31 **推翻**：只画一个框 ⇒ 业主**看不到其它候选**，所以判断不了
    「它为什么选了这个」——那天框罩在头盔上（实际选中的是训练场的球）就是这样查出来的。
  - 业界（sunone_aimbot 的 debug/overlay 窗口）的做法是三层：
      ① 全部检测框（show_boxes）② 选中的框（show_target_line 之外的那层）
      ③ 中心→落点连线（show_target_line）+ FOV 圆（各类 aimbot 的 cv2.circle）
"""
from __future__ import annotations

import re
from pathlib import Path

from plugins.web.lib.paths import discover_root

REPO = Path(discover_root(__file__))
PREV_CPP = REPO / 'core' / 'src' / 'preview' / 'PreviewModule.cpp'
PREV_H = REPO / 'core' / 'src' / 'preview' / 'PreviewModule.hpp'
RUNTIME = REPO / 'core' / 'src' / 'runtime' / 'CoreRuntime.cpp'
THREAD = REPO / 'core' / 'src' / 'aim' / 'AimThread.cpp'
PP_CPP = REPO / 'core' / 'src' / 'mouse' / 'AimPointProfile.cpp'
PP_H = REPO / 'core' / 'src' / 'mouse' / 'AimPointProfile.hpp'


def _src(p: Path) -> str:
    return p.read_text(encoding='utf-8')


def _code_only(text: str) -> str:
    text = re.sub(r'//[^\n]*', '', text)
    text = re.sub(r'/\*(?:.|\n)*?\*/', '', text)
    return text


# ---------------------------------------------------------------- 三层都在
def test_preview_draws_all_three_layers():
    cpp = _src(PREV_CPP)
    for fn, why in (('draw_detections_list', '① 全部检测框（细）'),
                    ('draw_aim_box', '② 选中的框（粗）'),
                    ('draw_guides', '③ 中心→落点连线 + FOV 圆')):
        assert fn in cpp, '缺少 %s —— %s' % (fn, why)
    # 三层都要在绘制分支里被调用（顺序：全部 → 选中 → 辅助线）
    i_all = cpp.index('draw_detections_list(crop, roi.w, roi.h, crop_stride, all, roi.x, roi.y);')
    i_aim = cpp.index('draw_aim_box(crop, roi.w, roi.h, crop_stride, aim_box, roi.x, roi.y);')
    i_gd = cpp.index('draw_guides(crop, roi.w, roi.h, crop_stride, guides, roi.x, roi.y);')
    assert i_all < i_aim < i_gd, '三层绘制顺序应为 全部→选中→辅助线（先细后粗，线压在上面）'


def test_all_three_providers_are_registered():
    """三层需要三路数据，缺一层画面就少一块（业主看不到为什么选它）。"""
    rt = _src(RUNTIME)
    for setter in ('set_detections_provider', 'set_aim_box_provider', 'set_guides_provider'):
        assert setter in rt, 'CoreRuntime 没注册 %s ⇒ 那一层画不出来' % setter


def test_guides_provider_reuses_selector_fov_radius():
    """★ 画出来的圆必须**就是**约束选靶的那个圆（不能两处各算一遍）。
    调试画面最常见的骗人方式就是"画的圆"与"生效的圆"对不上。"""
    rt = _code_only(_src(RUNTIME))
    m = re.search(r'set_guides_provider\(\[this\]\(\)\s*\{(.*?)\n                \}\);', rt, re.S)
    assert m, '找不到 guides provider 的实现'
    body = m.group(1)
    assert 'fov_radius()' in body or 'fov_radius_px' in body, (
        'guides 里的 FOV 半径必须取自选靶器的实际值（fov_radius_px），不能自己重算')
    assert 'last_fov_radius' not in body, (
        'selector_ 是 AimThread 私有成员，外部拿不到；应经 AimThread::fov_radius_px() 转发')


def test_fov_radius_no_longer_doubled():
    """★ FOV 曾形同虚设：fov.radius 被多乘 2（把半径当直径算）
    ⇒ 板端 0.5 算成 1.0 ⇒ 半径 = 320 = 画面半宽 ⇒ 圆等于全屏 ⇒ 零约束。"""
    th = _code_only(_src(THREAD))
    m = re.search(r'scfg\.fov_range\s*=\s*([^;]+);', th)
    assert m, '找不到 scfg.fov_range 的赋值'
    expr = m.group(1)
    assert '* 2' not in expr and '*2' not in expr.replace(' ', ''), (
        'fov_range 又出现乘 2 ⇒ FOV 半径会翻倍回到"圆等于全屏、零约束"的老问题：%s' % expr.strip())


# ---------------------------------------------------------------- 框不再被裁
def test_box_shrinking_is_fully_retired():
    """V1.0.24~30 把框裁小（垂直砍半 + 收窄到肩宽）——业主 2026-10-04 定调
    「框完整，只偏移落点」（业界做法是 body_y_offset 在身体框内偏移，不是裁框）。"""
    for path, name in ((PP_CPP, 'AimPointProfile.cpp'), (PP_H, 'AimPointProfile.hpp')):
        code = _src(path)
        assert 'shrink_to_upper_body' not in code, (
            '%s 里还有 shrink_to_upper_body —— 裁框已整体退役' % name)
        assert 'kUpperBodyRatio' not in code, (
            '%s 里还有 kUpperBodyRatio —— 上半身比例常量随裁框一起退役' % name)
    th = _code_only(_src(THREAD))
    assert 'shrink_to_upper_body' not in th, 'AimThread 还在调用 shrink_to_upper_body'
    # 控制链用的框应当就是 selected.box（原框）
    assert re.search(r'aim_box_src\s*=\s*selected\.box', th), (
        '控制链的框应当直接是 selected.box（模型原框）')


def test_aim_point_still_inside_the_box():
    """落点仍在框内由 offset_y 定位（默认 0.24 ≈ 胸口），公式本身没变。"""
    pp = _code_only(_src(PP_CPP))
    assert re.search(r'\*oy', pp) or 'offset_y' in pp, '落点公式应仍用 offset_y'


# ---------------------------------------------------------------- 选靶不再选中球
def test_relative_geometry_guard_present():
    """选中的框罩在头盔、大小不随远近变 ⇒ 实际选中的是训练场的球（sunone 类别表 6=球）。
    修法用**相对**判据（同位置群内比高宽比），不用绝对阈值 —— 绝对阈值会误杀
    远处/小目标的正常人框（2026-10-04 本机回归当场打回 3 个测试）。"""
    cpp = _code_only(_src(PP_CPP) + _src(REPO / 'core' / 'src' / 'mouse' / 'TargetSelector.cpp'))
    assert 'looks_like_round_object' in cpp, '缺少「同位置群内比高宽比」的相对判据'
    assert 'min_aspect_h_over_w' not in cpp, (
        '又出现了绝对高宽比阈值 —— 它在远处/小目标上不成立，会误杀正常人形')
    hpp = _src(REPO / 'core' / 'src' / 'mouse' / 'TargetSelector.hpp')
    assert 'prefer_humanoid' in hpp, 'Candidate 缺少 prefer_humanoid 排序键'


def test_round_object_is_sorted_after_humanoid():
    cpp = _src(REPO / 'core' / 'src' / 'mouse' / 'TargetSelector.cpp')
    i_tag = cpp.index('out[i].prefer_humanoid = looks_like_round_object(out, i)')
    i_sort = cpp.index('std::sort(out.begin(), out.end()')
    assert i_tag < i_sort, '必须先打标再排序'
    seg = cpp[i_sort:i_sort + 400]
    assert 'a.prefer_humanoid != b.prefer_humanoid' in seg, (
        '几何判据必须是**第一排序键**（圆/方块物整体后排）')
    assert seg.index('prefer_humanoid') < seg.index('a.priority'), (
        '几何判据要排在 priority 之前 —— 否则一个高优先级类别的球还是会赢')


# ---------------------------------------------------------------- 角标 = 置信度
def test_aim_box_label_is_confidence_not_class():
    """业主 2026-10-05：框上方应是模型的置信度，不应该是类别。
    类别号在 7 类模型（class_names 为空）下没有语义；置信度才是模型直接输出。"""
    prev = _code_only(_src(PREV_CPP))
    m = re.search(r'snprintf\(label[^;]*\)', prev)
    assert m, '找不到 draw_aim_box 的角标 snprintf'
    assert 'box.score' in m.group(0), '角标应取 box.score（置信度），不是别的：%s' % m.group(0)
    assert 'box.class_id' not in m.group(0), (
        '角标不应再取 box.class_id（类别）：%s' % m.group(0))
    # 数据源必须把选中目标置信度传出来（不能再硬编码 0）
    rt = _code_only(_src(RUNTIME))
    assert 'out->score = st.target_score' in rt, (
        'aim_box_provider 应回填 st.target_score，不能写死 0')
    # AimThread 必须把选中目标置信度写进 status
    th = _code_only(_src(THREAD))
    assert 'status_.target_score = selected.valid ? selected.box.score' in th, (
        'AimThread 没把选中目标置信度写进 status')
    # Status 结构体必须有 target_score 字段
    ah = _src(REPO / 'core' / 'src' / 'aim' / 'AimThread.hpp')
    assert re.search(r'float\s+target_score\s*=', ah), 'AimThread::Status 缺 target_score 字段'
