# -*- coding: utf-8 -*-
"""V1.0.25「预览 = 实际瞄准框」的护栏。

★ 为什么只能源码级：`PreviewModule.cpp` **不参与本机编译**（host 无 opencv/rga，
  它与 PreviewModule/RKNNEngine/WorkerPool/V4L2Capture/TtboxLicenseClient 一起被
  排除）⇒ 本机 ctest 对它**零覆盖**，写不了端到端断言。
  真正验证它的是「WSL 交叉编译 + 板端实跑」，护栏只负责**钉住契约不被改回去**。

★ 业主定口径（2026-10-04）：「预览里本来应该要画的就是实际瞄准的框，
  预览和实际瞄准框应该一致。」
  旧实现画的是本帧**全部**检测框（红=class0 / 绿=class1 加粗假装是"头" / 黄=其余），
  两个框叠在一起看着像"又大又含头" —— 那是候选框集合，预览在骗人。
"""
from __future__ import annotations

import re
from pathlib import Path

from plugins.web.lib.paths import discover_root

REPO = Path(discover_root(__file__))

PREV_H = REPO / 'core' / 'src' / 'preview' / 'PreviewModule.hpp'
PREV_CPP = REPO / 'core' / 'src' / 'preview' / 'PreviewModule.cpp'
RUNTIME = REPO / 'core' / 'src' / 'runtime' / 'CoreRuntime.cpp'


def _src(p: Path) -> str:
    return p.read_text(encoding='utf-8')


def _code_only(text: str) -> str:
    """剥注释与字符串字面量（先 `//` 后 `"…"`，顺序不能反：中文引号会吃后半段代码）。"""
    text = re.sub(r'//[^\n]*', '', text)
    text = re.sub(r'"(?:[^"\\\n]|\\.)*"', '""', text)
    return text


# ---------------------------------------------------------------- 数据源
def test_provider_feeds_the_aim_box_not_all_detections():
    code = _code_only(_src(RUNTIME))
    assert 'set_aim_box_provider' in code, 'preview provider 必须是 aim_box_provider'
    # ★ 切片要够宽：早先用「到第一个 } 为止」，结果注入 detection_boxes 的那行落在
    #   切片外 ⇒ 断言空转假绿（2026-10-04 实测，第三次踩「作用域给太小」这个坑）。
    #   现在用固定窗口覆盖整个 lambda 体，后面即使还有别的代码也没有这两个关键字。
    raw = _src(RUNTIME)
    blk = _code_only(raw[raw.index('set_aim_box_provider'):][:1800])
    for field in ('target_x1', 'target_y1', 'target_x2', 'target_y2'):
        assert field in blk, (
            'provider 必须喂 target_*（控制链这一帧实际在用的框）而不是 detection_boxes：'
            '预览 = 瞄准框是业主定的口径')
    assert 'detection_boxes' not in blk, (
        'provider 里又出现 detection_boxes ⇒ 预览改回画全部候选框了（业主否决过）')


def test_provider_reports_false_when_no_target():
    """没有选中目标就返回 false ⇒ 画面不画框。旧逻辑会"丢失后保留 3 帧旧框"，
    那是**画出已经不瞄的框**，同样是骗人。"""
    code = _code_only(_src(RUNTIME))
    assert re.search(r'if\s*\(\s*!st\.has_target\s*\)\s*return false', code), \
        '没有选中目标时 provider 必须返回 false（不许沿用旧框）'


# ---------------------------------------------------------------- PreviewModule 契约
def test_old_multi_box_contract_is_gone():
    h = _code_only(_src(PREV_H))
    cpp = _code_only(_src(PREV_CPP))
    for sym, where in (('DetectionsProvider', 'h'), ('detections_provider_', 'h'),
                       ('set_detections_provider', 'h'), ('smooth_boxes', 'h'),
                       ('draw_boxes', 'h')):
        assert sym not in h, (
            '%s 里还有 %s —— 预览的多框契约没清干净（业主要的是"一个框"）' % (where, sym))
    for sym in ('detections_provider_', 'smooth_boxes', 'draw_boxes',
                'smooth_prev_', 'smooth_lost_count_'):
        assert sym not in cpp, 'PreviewModule.cpp 里还有 %s（多框路径残留 = 尸体代码）' % sym
    assert 'draw_aim_box' in h and 'draw_aim_box' in cpp, '缺少 draw_aim_box（单框绘制）'


def test_no_secondary_smoothing_of_the_aim_box():
    """传入的框已由 AimThread 的 One-Euro 平滑过；再过一道低通只会更滞后、
    离控制链更远 —— 那等于预览又跟控制不一致了。"""
    cpp = _src(PREV_CPP)
    branch = cpp[cpp.index('if (params_.draw_detections)'):]
    branch = branch[:branch.index('\n    }')]
    assert 'smooth' not in branch, '预览分支里还有二次平滑（与控制链脱节）'


def test_color_is_not_derived_from_class_id():
    """★ 旧实现「class 1 加粗 + 标成头色」是出厂 2 类模型（EP：0=身体 1=头）的约定。
    板端现役模型 class_count=7 且 class_names 为空 ⇒ 那个映射无从谈起，
    继续按它上色 = 在画面上编一个不存在的语义。"""
    cpp = _code_only(_src(PREV_CPP))
    fn = cpp[cpp.index('PreviewModule::draw_aim_box'):]
    fn = fn[:fn.index('\n}\n') + 3]
    assert not re.search(r'box\.class_id\s*==', fn), \
        'draw_aim_box 又按 class_id 分派颜色/线宽了（7 类模型下是瞎猜）'
    # 单框用固定醒目色
    assert re.search(r'cv::Scalar\s+\w+\s*\(\s*0\s*,\s*255\s*,\s*0\s*\)', fn), \
        '单框应使用固定醒目色（亮绿）'


def test_label_does_not_fabricate_confidence():
    """Status 没有"选中目标置信度"字段，标 score 只能填 0 —— 画面上会出现
    "cls 0 0.00" 这种假数据。角标只留类别号。"""
    cpp = _src(PREV_CPP)
    fn = cpp[cpp.index('PreviewModule::draw_aim_box'):]
    fn = fn[:fn.index('\n}\n') + 3]
    assert 'box.score' not in fn, '角标不能用 box.score（Status 无此字段，只能是假 0）'
    assert 'cls %d' in fn, '角标应显示类别号'


# ---------------------------------------------------------------- 反向提醒
def test_preview_module_is_not_covered_by_host_tests():
    """把"本机测不到它"这件事写进测试：任何人以为 ctest 绿就等于 preview 没问题，
    就会跳过板端验证。PreviewModule 依赖 opencv ⇒ host 不编译。"""
    cpp = _src(PREV_CPP)
    assert 'opencv2' in cpp, \
        '（护栏自检）PreviewModule.cpp 仍应 include opencv —— 若哪天它能在 host 编译了，' \
        '本文件的存在理由要重写（那时该补真端到端断言，而不是继续靠源码级）'
