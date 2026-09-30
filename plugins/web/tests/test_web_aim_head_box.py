# test_web_aim_head_box.py — 「瞄头部小框」的前后端映射与面板接线（几何配对识头）
#
# 背景（业主 2026-09-30 指令二）：落点改为瞄「小框(头)正中心」，靠几何配对不靠类别号。
# 三处必须同时成立：
#   ① 后端 profile_to_web 把 mouse.aim_at_head_box 暴露给面板（ctrl 层）；
#   ② 后端 web_body_to_profile 把面板勾选写回 mouse.aim_at_head_box；
#   ③ 前端有 checkbox 控件 + populateForm 回填 + collectConfig 收集 + 默认值。
import os


def _read(rel):
    here = os.path.dirname(os.path.abspath(__file__))
    for base in (os.getcwd(), os.path.dirname(os.path.dirname(here))):
        p = os.path.join(base, rel)
        if os.path.exists(p):
            return open(p, encoding='utf-8').read()
    raise AssertionError('找不到文件: ' + rel)


WEB = _read('plugins/web/bin/ttbox-web.py')
HTML = _read('plugins/web/templates/index.html')


def test_profile_to_web_exposes_flag():
    assert "'aim_at_head_box': mouse.get('aim_at_head_box', False)" in WEB


def test_web_body_to_profile_writes_flag():
    assert "mouse['aim_at_head_box'] = bool(ctrl['aim_at_head_box'])" in WEB


def test_panel_has_checkbox():
    assert 'id="controller_aim_at_head_box"' in HTML
    assert 'data-config type="checkbox"' in HTML.split('controller_aim_at_head_box')[1][:120]


def test_populate_form_fills_checkbox():
    assert 'setCheckbox("controller_aim_at_head_box", controller.aim_at_head_box' in HTML


def test_collect_config_reads_checkbox():
    assert 'aim_at_head_box: getCheckbox("controller_aim_at_head_box")' in HTML


def test_defaults_off_zero_change():
    # 默认关 = 零变化（与 core 结构体 bool aim_at_head_box = false 一致）
    assert 'aim_at_head_box: false,' in HTML


def test_reset_defaults_includes_flag():
    # 「恢复默认」也要覆盖到这个勾选，否则重置漏项
    assert 'controller_aim_at_head_box: controller.aim_at_head_box' in HTML
