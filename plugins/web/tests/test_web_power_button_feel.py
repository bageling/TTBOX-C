# -*- coding: utf-8 -*-
"""锁定首页启停按钮的**手感契约** —— 2026-10-04（业主："动画很生硬"）。

★ 为什么需要这条测试：
  业主对 V1.0.21 的反馈是「首次点击启动会卡顿，不丝滑，动画很生硬」。
  服务端那一半的根因量到了（冷路径 start 1.07s vs 热路径 0.25s，见
  `core/tests/test_capture_open_wait_policy.py`）；**前端这一半**的根因是
  `.power-button` 上**一条 transition 都没有**：
    · 悬停的 `translateY(-2px)` 是硬跳；
    · 启动↔停止的配色切换是硬切；
    · 请求在途时用 `opacity: 0.72` + 1.1s 闪烁表示忙 —— 没有过渡，进入/退出都硬跳，
      而且把整颗按钮调暗后**连文字都跟着变糊**，看着像"坏了"而不是"在忙"。
  这些都是纯 CSS，**不会被任何功能测试发现**（页面照样能用、接口照样 200）。
  所以必须有这么一条把"手感"当契约钉住的测试。

★ 判据口径（踩过的坑，务必保住）：
  1. 取规则体**不能**用 `'.power-button {' in css` 这种前缀匹配 ——
     `.power-button:hover` / `.power-button.stop` 都含这个前缀，
     断言会变成在检查**别的规则**（经典"假绿"）。本文件按 `}` 切规则、
     再把选择器列表拆成逗号项做**精确**比较。
  2. 源序（source order）本身是要断言的东西：`.power-button.is-busy` 与
     `.power-button:hover` 特异性同为 (0,2,0)，**平局由源序决定** ——
     busy 规则一旦被挪到 hover 之前，"按下去"的形状就会被 hover 的位移顶掉。
     这个 bug 在浏览器里表现为"按下去弹回来"，改行号就能引入，必须钉住。
  3. CSS 里的 `/* */` 注释要先剥掉再断言，否则注释里提到 `is-busy` 会让
     "还原分支不含 is-busy" 这类**否定**断言变成假绿。
"""
from __future__ import annotations

import re

from plugins.web.tests import panel_src

BASE = '.power-button'
BUSY = '.power-button.is-busy'


def _strip_comments(css: str) -> str:
    return re.sub(r'/\*.*?\*/', '', css, flags=re.S)


def _css() -> str:
    return _strip_comments(panel_src.css_src())


def _rule_body(css: str, selector: str) -> str:
    """返回第一个「选择器列表里**恰好**含 selector」的规则体。

    ★ 精确比较，不用前缀匹配 —— 见文件头「判据口径 1」。
    """
    for head, body in re.findall(r'([^{}]+)\{([^{}]*)\}', css):
        sels = [s.strip() for s in head.strip().split(',')]
        if selector in sels:
            return body
    raise AssertionError('CSS 里找不到选择器 %r' % selector)


def _js() -> str:
    return panel_src.js_src()


def _slice(src: str, start_marker: str, end_marker: str) -> str:
    i = src.index(start_marker)
    j = src.index(end_marker, i)
    return src[i:j]


# ---------------------------------------------------------------------------
# 1. 过渡：按钮上必须有 transition，否则一切状态变化都是硬跳
# ---------------------------------------------------------------------------

def test_base_rule_has_transition():
    body = _rule_body(_css(), BASE)
    assert 'transition' in body, (
        '.power-button 上没有 transition ⇒ 悬停/配色/忙态全是硬跳（"动画很生硬"的根源）')


def _transition_value() -> str:
    """取 `.power-button` 那条 `transition:` 声明的**值**（到分号为止）。

    ★ 为什么不能直接 `assert 'box-shadow' in body`：规则体里**本来就有一条**
      静态的 `box-shadow:` 声明 ⇒ 把 transition 列表里的 box-shadow 那一条删掉，
      断言照样绿（假绿）。必须只在这个声明的值里查。
      （反向验证抓到的同型假绿：core 侧 `kBootSettleSec` 被注释喂饱，
        web 侧这里是被同规则里的静态声明喂饱。）
    """
    body = _rule_body(_css(), BASE)
    m = re.search(r'\btransition\s*:\s*([^;]*);', body, re.S)
    assert m, '.power-button 上找不到 transition 声明'
    return m.group(1)


def test_transition_covers_the_properties_that_visually_change():
    """去掉任意一条，对应那一种变化就又会变成硬跳。"""
    value = _transition_value()
    for prop in ('transform', 'box-shadow', 'border-color', 'background'):
        assert re.search(r'(?<![\w-])%s(?![\w-])' % re.escape(prop), value), (
            'transition 列表里没有 %s ⇒ 它仍会硬跳' % prop)


# ---------------------------------------------------------------------------
# 2. 忙态：不许掉透明度，要用"有物理感"的三个信号
# ---------------------------------------------------------------------------

def test_busy_state_does_not_dim_the_button():
    body = _rule_body(_css(), BUSY)
    assert 'opacity' not in body, (
        '忙态又在改 opacity ⇒ 文字跟着变糊、看着像"被禁用"，而不是"正在工作"')


def test_busy_state_uses_pressed_scale_and_glow():
    body = _rule_body(_css(), BUSY)
    assert 'scale(0.97)' in body, '忙态缺少"按住"的形态（scale），少了物理感'
    assert 'power-button-glow' in body, '忙态缺少光晕呼吸，只靠形状不够显眼'


def test_old_opacity_blink_is_gone():
    """旧实现（power-button-busy 的 0.72↔0.94 闪烁）不许复活。"""
    assert 'power-button-busy' not in _css(), (
        '旧的 opacity 闪烁 keyframes 又回来了 —— 它是被业主点名"生硬"的那一版')


def test_busy_rules_come_after_hover_rules():
    """特异性平局靠源序 —— busy 必须在所有 :hover 之后。见文件头「判据口径 2」。"""
    css = _css()
    assert css.index(BUSY) > css.index('.power-button:hover'), (
        '.power-button.is-busy 被写到了 .power-button:hover 之前 ⇒ '
        '平局时 hover 胜出，"按下去"的形状会被 translateY(-2px) 顶掉')
    assert css.index(BUSY) > css.index('.power-button.stop:hover'), (
        '同上：必须排在 .power-button.stop:hover 之后')


def test_press_feedback_is_immediate():
    """按下（:active）就要有反馈，不等请求 —— 这是"不卡顿"的体感关键。"""
    body = _rule_body(_css(), '.power-button:active')
    assert 'scale(0.96)' in body


def test_stop_direction_uses_red_glow():
    body = _rule_body(_css(), '.power-button.stop.is-busy')
    assert 'power-button-glow-stop' in body, (
        '"停止中"若沿用绿色光晕，会和按钮本身的红色自相矛盾')


def test_busy_speeds_up_the_ring():
    """外环平时 7s/圈（慢到看不出在转），忙时必须明显加速才有信号作用。"""
    body = _rule_body(_css(), '.power-button.is-busy .power-ring::before')
    assert 'spin-fast' in body
    # ★ 不能拿 _rule_body 去取 @keyframes：它内部还有一层 `to { ... }`，
    #   而 _rule_body 的正则是**单层**花括号 ⇒ 取不到外层块。
    fast = _at_block(_css(), '@keyframes spin-fast')
    assert 'rotate(360deg)' in fast
    assert '0.9s' in body or '0.9s' in fast, '加速后的周期没写出来'


def _at_block(css: str, header: str) -> str:
    """按花括号配平取 `@keyframes x` / `@media ...` 这类**可嵌套**的 at-rule 块。"""
    i = css.index(header)
    j = css.index('{', i)
    depth = 0
    for k in range(j, len(css)):
        if css[k] == '{':
            depth += 1
        elif css[k] == '}':
            depth -= 1
            if depth == 0:
                return css[j:k + 1]
    raise AssertionError('%s 花括号不配平' % header)


# ---------------------------------------------------------------------------
# 3. JS：点下去第一件事就切忙态；忙态要够长；退出时不残留
# ---------------------------------------------------------------------------

def test_button_has_the_ring_element():
    """加速旋转的那一圈得有实体元素，否则上面那条规则是空转。"""
    assert '<span class="power-ring"></span>' in panel_src.html_src()


def test_busy_flag_is_set_before_the_license_awaits():
    """忙态必须早于「授权恢复」的两次网络往返。

    ★ 这是业主说的"首次点击会卡顿"的前端那一半：原顺序是
      `await refreshLicenseStatus() → await refreshAll() → 才切忙态`，
      于是恰好在需要恢复授权的那一次点击，前几百毫秒界面**零变化**。
    """
    handler = _slice(_js(), 'on("startButton", "click"', 'rebootSystemButton')
    i_busy = handler.index('showRuntimeControlBusy(true')
    i_license = handler.index('await refreshLicenseStatus()')
    assert i_busy < i_license, (
        '忙态又排到授权恢复 await 后面了 ⇒ 那次点击会先卡一下再给反馈')


def test_min_busy_duration_exists():
    """请求若瞬间返回，动画刚按下去就要弹回来 —— 要等满一个最短时长。"""
    assert re.search(r'260\s*-\s*\(\s*performance\.now\(\)\s*-\s*busyStartedAt\s*\)',
                     _js()), '缺少「最短忙态时长」：短请求会让按钮抖一下'
    assert re.search(r'remaining\s*>\s*0', _js())


def test_busy_restore_does_not_leave_is_busy_class():
    """还原分支必须去掉 is-busy，否则按钮会永久保持"按下去"的形状。"""
    fn = _slice(_js(), 'function showRuntimeControlBusy(', '\nfunction renderSystemStats')
    assert 'is-busy' in fn, '忙态分支没加 is-busy（那整块 CSS 都不会生效）'
    tail = fn[fn.index('// 还原'):]
    assert 'is-busy' not in tail, '还原分支残留了 is-busy'


def test_busy_class_name_is_built_consistently():
    """忙态与还原态必须由同一套模板拼 className，否则会拼出残缺的 class。"""
    assert "power-button ${isStopAction ? \"stop\" : \"start\"} is-busy" in _js()
    assert "power-button ${isStopAction ? \"stop\" : \"start\"}`" in _js()
