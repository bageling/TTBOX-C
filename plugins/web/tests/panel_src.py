# -*- coding: utf-8 -*-
"""面板源码读取helper —— 2026-10-03（index.html 拆出panel.css / panel.js 配套）。

★ 为什么需要这个模块（2026-10-03 之前不存在）：
  面板原本是**单文件** `templates/index.html`（13446 行，CSS 5188+ JS 7218 全内联），
  14 个测试文件都直接 `read_text('index.html')` 然后 `assert '某串' in HTML`。
  2026-10-03 业主拍板方案 B，把 CSS 外链成 `static/panel.css`；
  同日又把 7219 行的 `static/panel.js` 按行号切成 `static/panel/*.js` 五段
  （01-const / 02-core / 03-render / 04-actions / 05-events），
  `index.html` 只剩 1042 行骨架 + 6 个外链标签。
  ⇒ 那些 `in HTML` 断言全部失效（它们要找的代码搬走了）。

  正确修法不是「把断言删掉」，而是让它们读**整个面板**——
  断言的**意图**（「这段逻辑必须在面板里」）没变，只是读取范围变���。

★ 用法（替代 `TEMPLATE.read_text()`）：
    from plugins.web.tests import panel_src
    src = panel_src.all_src()        # index.html + panel.css + panel.js 拼起来
    assert 'normalizeCropSize' in src
  只查 HTML 骨架时仍可用 `panel_src.html_src()`。
"""
from __future__ import annotations

from pathlib import Path

from plugins.web.lib.paths import repo_root as _ttbox_repo_root  # noqa: E402

REPO_ROOT = Path(_ttbox_repo_root())
TEMPLATE = REPO_ROOT / 'plugins' / 'web' / 'templates' / 'index.html'
STATIC_DIR = REPO_ROOT / 'plugins' / 'web' / 'static'
PANEL_CSS = STATIC_DIR / 'panel.css'
# ★ 2026-10-03 追加：panel.js 切成 5 段，顺序即 index.html 里的加载顺序
#★ 注意括号：`/` 的优先级高于 `%`，写成 `STATIC_DIR / 'panel' / '%s.js' % n`
#   会被解析成 `(STATIC_DIR / 'panel' / '%s.js') % n` ⇒ TypeError。
PANEL_JS_PARTS = [
    STATIC_DIR / 'panel' / ('%s.js' % n)
    for n in ['00-const', '01-home', '02-hotkey', '03-pointer', '04-assist', '05-model', '06-hardware', '07-preset', '08-license', '09-fan', 'calib-bind', '10-flow']
]


def html_src() -> str:
    """模板骨架（index.html）。只含 HTML + 两个外链标签。"""
    return TEMPLATE.read_text(encoding='utf-8')


def css_src() -> str:
    """外链样式（static/panel.css）。"""
    return PANEL_CSS.read_text(encoding='utf-8')


def js_src() -> str:
    """外链脚本（static/panel/*.js 五段，按加载顺序拼接）。"""
    return '\n'.join(p.read_text(encoding='utf-8') for p in PANEL_JS_PARTS)


def all_src() -> str:
    """整个面板 = index.html + panel.css + panel/*.js 五段。

    ★ 顺序与浏览器加载顺序一致（CSS 在前、JS 五段按序），
      免得 `assert a in src and b in src` 之类的断言被顺序影响。
    """
    return '%s\n%s\n%s' % (html_src(), css_src(), js_src())


def script_only() -> str:
    """只要 JS 五段（原来那些读 index.html 找 JS 逻辑的测试用这个）。"""
    return js_src()


def style_only() -> str:
    """只要 CSS（读 index.html 找样式规则的测试用这个）。"""
    return css_src()
