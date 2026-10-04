# -*- coding: utf-8 -*-
"""V1.0.23 上半身收缩：**接线来源**的源码级护栏（core 侧）。

★ 为什么这层必须存在，而端到端做不到：
  「贴边判定用收缩框还是全身框」在落点层面**不可观测** —— 落点等效换算
  （offset ÷k、框高 ×k）让两种取值落在同一像素；冻结又优先于外推。
  实测记录（2026-10-04，两种尝试都失败）：
    · frozen_for 优先：关/开 都冻结在远景帧框上 ⇒ 落点同为 510，无法区分；
    · 外推路径：关 500+0.5·600=800、开 500+1.0·300=900 ⇒ **本该不同，实测同为 900**。
  这处接线的真实影响落在 measurement_valid（压枪有效量测门控）与冻结记录域上，
  本机没有板端硬件可测 ⇒ 只能用源码级断言钉住「判定取自哪个变量」。
  手法照 core/tests/test_capture_open_wait_policy.py（V4L2Capture 不参与 host 编译时同款）。

★ 断言作用域纪律（今天踩过三次假绿）：每条断言都先把**那一段**切出来再查，
  绝不在整文件里 `in` 一次了事 —— 否则别处的同名文本会替你站岗。
"""
from __future__ import annotations

import re
from pathlib import Path

import pytest

from plugins.web.lib.paths import discover_root

REPO = Path(discover_root(__file__))

THREAD = REPO / 'core' / 'src' / 'aim' / 'AimThread.cpp'
PROFILE = REPO / 'core' / 'src' / 'model' / 'RuntimeProfile.cpp'
TYPES = REPO / 'core' / 'src' / 'mouse' / 'MouseTypes.hpp'
POINT = REPO / 'core' / 'src' / 'mouse' / 'AimPointProfile.cpp'


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
    """切出 start_marker 到其后第一个 end_marker 之间的片段（含标记行）。"""
    i = text.index(start_marker)
    j = text.index(end_marker, i)
    assert j - i < limit, '切片 %d 字符，疑似 end_marker 失配' % (j - i)
    return text[i:j + len(end_marker)]


# ---------------------------------------------------------------- 收缩调用点
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
    在该块外也消费 aim_box_src，放块内会编译不过（已实测踩到：'not declared in scope'）。"""
    code = _code_only(_src(THREAD))
    decl = code.index('DetectionBox aim_box_src = selected.box;')
    gate = code.index('if (target_ok) {')
    assert decl < gate, '收缩声明落在 target_ok 块内 ⇒ 块外消费点拿不到它'


# ---------------------------------------------------------------- 贴边判定
def test_clip_judgment_reads_the_shrunk_box():
    block = _code_only(_span(_src(THREAD), 'const bool box_bottom_clipped =', ';'))
    assert 'aim_box_src' in block, (
        '贴边判定没有读收缩框 aim_box_src —— 收缩模式仍按全身框判截断，'
        '近身会误触发冻结/外推（本文件头记录：落点层面测不到这处，只能源码钉）')
    assert 'selected.box' not in block, (
        '贴边判定里出现了 selected.box ⇒ 拿全身框判收缩域的截断，两套坐标系混了')


def test_clip_ratio_tracker_observes_the_shrunk_box():
    block = _code_only(_span(_src(THREAD), 'clip_ratio_tracker_.observe(', ');'))
    assert block.count('aim_box_src') >= 2, (
        '身高自校准比必须观察收缩框的宽/高（外推在收缩域消费，观测端也要同域）：%r' % block)
    assert 'selected.box' not in block


def test_frozen_rect_observes_the_shrunk_box():
    block = _code_only(_span(_src(THREAD), 'frozen_rect_.observe(', ');'))
    assert 'aim_box_src' in block, '冻结框记录的是全身框 ⇒ 冻结/实时两套域，落点会跳'
    assert 'selected.box' not in block


# ---------------------------------------------------------------- 落点链
def test_aim_point_uses_mapped_profile_and_shrunk_box():
    block = _code_only(_span(_src(THREAD), 'if (!aim_point_at(', ');'))
    assert 'prof_ub' in block, (
        '落点必须用换算后的 prof_ub（offset ÷k）；用 aim_point 会让开关一开落点整体上飘 k 倍身高')
    assert re.search(r'aim_point_at\(\s*aim_box\b', block), '落点用的框必须是 aim_box（收缩/冻结域）'


def test_head_aim_constraint_uses_mapped_profile():
    block = _code_only(_span(_src(THREAD), 'constrain_aim_point_to_head(', ');'))
    assert 'prof_ub' in block, '头区约束要用换算后的 prof_ub（它的 fraction 是相对框高的量）'


def test_box_h_and_target_radius_use_shrunk_box():
    code = _code_only(_src(THREAD))
    blk = _span(code, 'tracker_.set_box_h(', ');')
    assert 'aim_box_src' in blk, 'set_box_h（滤波自适应/遥测）要与控制域同框'
    blk2 = _span(code, 'personal_shader_.set_target_radius_px(', ');')
    assert 'aim_box_src' in blk2, '拟人化目标半径要与控制域同框'


# ---------------------------------------------------------------- 显示框
def test_display_box_is_shrunk_with_the_same_ratio():
    code = _code_only(_src(THREAD))
    blk = _span(code, 'const float k_disp = upper_body_shrink_ratio(aim_point);', '}')
    assert re.search(r'display_y2\s*=\s*display_y1\s*\+\s*k_disp\s*\*', blk), (
        '显示框必须按同一比例收缩（所见即所控）：%r' % blk)
    assert 'k_disp < 1.0f' in blk, '收缩必须只在 k<1 时生效（k=1 是"不收缩"）'


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


def test_shrink_ratio_domain_is_fail_closed():
    code = _code_only(_src(POINT))
    m = re.search(r'float\s+upper_body_shrink_ratio\([^)]*\)\s*\{(.*?)\n\}', code, re.S)
    assert m, '找不到 upper_body_shrink_ratio 实现'
    body = m.group(1)
    assert re.search(r'k\s*>\s*0\.05f\s*&&\s*k\s*<=\s*1\.0f', body), (
        '比例有效域必须是 (0.05, 1.0]；放宽或收紧都会让坏配置静默改变行为：%r' % body)
    assert re.search(r'!\s*prof\.upper_body_enabled\s*\)\s*return\s*1\.0f', body), (
        '关闭时必须返回 1.0（=不收缩），不能看 ratio 就收缩')


# ---------------------------------------------------------------- 配置序列化
def test_profile_serializes_both_keys_both_directions():
    # ★ 这里**不能**用 _code_only：JSON 键名本身就是字符串字面量，剥掉就查不到了。
    #   改用带右括号的精确串（`m.set("k",` 只可能出现在 to_json）。
    #   另：注释里若出现同样的键名会喂饱断言 —— 当前注释不含这些精确串。
    code = _src(PROFILE)
    for key in ('upper_body_enabled', 'upper_body_ratio'):
        assert 'm.set("%s",' % key in code, 'to_json 缺 %s' % key
        assert re.search(r'aim_point\.%s\s*=\s*[^;]*?obj_(bool|num)\(' % key, code), \
            'from_json 缺 %s' % key


def test_defaults_are_off_with_ratio_half():
    types = _code_only(_src(TYPES))
    m = re.search(r'bool\s+upper_body_enabled\s*=\s*(\w+)\s*;', types)
    assert m, 'AimPointProfile 里找不到 upper_body_enabled'
    assert m.group(1) == 'false', '默认必须是 false（默认开 = 未验证就改所有用户的瞄准行为）'
    m2 = re.search(r'float\s+upper_body_ratio\s*=\s*([\d.]+)f', types)
    assert m2 and abs(float(m2.group(1)) - 0.5) < 1e-6, '默认比例应是 0.5（头顶到髋）'
