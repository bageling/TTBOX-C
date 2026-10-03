# test_web_console_parity.py — 面板 1:1 采用「结构 + 品牌插槽 + 同源零外联」回归
#
# 背景：面板 = 上游 YU 控制台**整包落地**为 plugins/web/templates/index.html，
# 叠加 4 处品牌插槽（data-ui-brand / data-theme / <title> / body.ui-brand-*）。
# 本文件锁死三件事，防止「换模板/改品牌」时把整包结构或离线约束悄悄破坏：
#   1 12 个 section.*-page 与 12 个 data-page-target 一一对应（= 参照物的 12 页签契约）；
#   2 4 处品牌插槽齐全（模板 Jinja 变量名与 _page_context() 契约一致）；
#   3 面板**离线自足**：无 http(s):// 资源、无 CDN、无 <script src=…> 外部脚本。
#     允许**同源** <link href="/static/…"> 与 <script src="/static/…">。
#
#★ 约束演变（2026-10-03，业主拍板方案 B）：
#   原约束 = **零外联**（连 <link> 都不许），理由是「本仓无构建链、离线可用」。
#   现改为 = **同源 + 离线**。放宽的是「同源外链」，收紧的仍保留 http(s)禁令。
#   放宽原因：面板已涨到 13446 行（CSS 5188 + JS 7218 单块），单文件不可维护；
#   而「离线可用」这个**真正意图**用「无 http(s):// / 无 CDN」表达更准确
#   —— 同源 <link> 依然零网络依赖。
#   ⇒ 本条断言从「禁止一切外链」变成「禁止非同源外链」，**离线这条底线没丢**。
#
# 运行：python -m pytest plugins/web/tests/test_web_console_parity.py -v（从仓库根）
from __future__ import annotations

import pathlib
import re

from plugins.web.lib.paths import repo_root as _ttbox_repo_root  # noqa: E402  路径单点真源（A-PATH-3）
from plugins.web.tests import panel_src  # noqa: E402  2026-10-03 面板外链：读整个面板
REPO_ROOT = pathlib.Path(_ttbox_repo_root())
TEMPLATE = REPO_ROOT / 'plugins' / 'web' / 'templates' / 'index.html'
STATIC_DIR = REPO_ROOT / 'plugins' / 'web' / 'static'

# S1 减法（2026-09-18）后的 9 页签 section id（DOM 内 section 出现顺序）
# （07 Hailo / 08 键鼠盒子 / 09 无线 三个页签整段移除）
EXPECTED_PAGES = [
    'home', 'profiles', 'control', 'assist', 'model',
    'hardware', 'preset', 'license', 'fan',
]

# [布局冻结] 侧栏页签顺序 = 模板中 data-page-target 的出现顺序 = 01..09 展示顺序
EXPECTED_TAB_TARGETS = [
    'home-page', 'profiles-page', 'control-page', 'assist-page', 'model-page',
    'hardware-page', 'preset-page', 'license-page', 'fan-page',
]

# [布局冻结] 侧栏页签文案（编号 + 名称），逐字锁死
EXPECTED_TAB_LABELS = [
    '01总览', '02热键控制', '03移动控制', '04辅助功能', '05模型库', '06显示与鼠标',
    '07预设参数', '08系统状态', '09风扇控制',
]

# [布局冻结 → 2026-09-29 响应式解冻] .app-shell 宽度**上限**基线（px）。
# 2026-09-18 业主把布局定死成固定 1660px；2026-09-29 业主指令解冻
# （「控制台 UI 要跟随浏览器大小」「电脑端也加上缩放」），宽度改回自适应
# `min(1660px, calc(100vw - 36px))` —— 1660px 仍是**上限**：
# 视口 ≥1696px 时逐像素与冻结期一致，更窄时收缩，不再出横向滚动条。
FROZEN_APP_SHELL_MAX_WIDTH = 1660
# [响应式解冻] 启用中的 4 个断点（= 模板 <style> 段出现顺序）。
# 上游原值 1180/920/480/560 中三个必须抬高：外壳变流体后总览三栏在 ~1380px
# 就会挤爆（1180→1400）、平板/手机切换点（920→1024），另两个随手抬到常用档
# （480→560、560→768 分类偏移弹层）。
RESPONSIVE_BREAKPOINTS = (1400, 1024, 560, 768)
# [响应式解冻] 侧栏宽上限：宽屏（视口 ≥1682px）必须取到 286px，
# 主内容区才是冻结期的 1348px。
FROZEN_SIDEBAR_MAX_WIDTH = 286
LAYOUT_FREEZE_DOC = (
    REPO_ROOT / 'docs' / 'handover' / '2026-09-17' / '控制台布局冻结基线-2026-09-18.md'
)


def _src() -> str:
    """整个面板源码 = index.html + panel.css + panel.js。

    ★ 2026-10-03 面板外链后，本文件的断言大部分找的是 **CSS 规则或 JS 逻辑**
      （布局宽度、响应式断点、页签顺序……），它们随 CSS/JS 搬到了 static/。
      ⇒ 读的面板必须是**全部文件的合集**，否则全是假红。
      需要只看骨架时用 _html()。

    ★ 2026-10-03 追加：panel.js（7219 行）又按行号切成 5 段
      panel/01-const.js … 05-events.js ⇒ 合集变成 1 + 1 + 5 = 7 个文件。
    """
    return panel_src.all_src()


def _html() -> str:
    """只看 index.html 骨架。"""
    return panel_src.html_src()


def test_template_exists():
    assert TEMPLATE.is_file(), f'面板模板缺失: {TEMPLATE}'


def test_nine_pages_present():
    """9 个 section id=*-page：多一个/少一个都说明整包落地被裁剪或减法未同步。"""
    ids = re.findall(r'<section id="([a-z0-9\-]+)-page"', _html())
    assert len(ids) == 9, f'期望 9 个 section.*-page，实际 {len(ids)}: {ids}'
    assert set(ids) == set(EXPECTED_PAGES)


def test_nine_page_targets_present():
    """9 个 data-page-target（侧栏导航），与 9 个 section 一一对应。"""
    targets = re.findall(r'data-page-target="([a-z0-9\-]+)"', _html())
    assert len(targets) == 9, f'期望 9 个 data-page-target，实际 {len(targets)}: {targets}'
    assert set(targets) == {p + '-page' for p in EXPECTED_PAGES}


def test_license_gate_overlay_present():
    """面板保留上游 #licenseGateOverlay + 锁导航的 JS（运行期掉线/被撤销的就地兜底·第二重）。

    ★ 2026-10-03 外链后：DOM 标记在 index.html，函数在 static/panel.js。
    """
    assert 'id="licenseGateOverlay"' in _html()
    assert 'setLicenseNavigationLock' in panel_src.js_src()


def test_brand_slots_present():
    """4 处品牌插槽（模板变量名必须与 _page_context() 契约一致）。"""
    src = _html()
    assert 'data-ui-brand="{{ ui_skin }}"' in src
    assert 'data-theme="{{ default_theme }}"' in src
    assert '<title>{{ app_title }}</title>' in src
    assert 'class="license-loading ui-brand-{{ ui_skin }}"' in src


def test_zero_external_resources():
    """同源 + 离线：无 http(s):// 资源、无 CDN、无非同源脚本。

    ★ 2026-10-03 业主拍板方案 B 后从「零外联」放宽为「同源零外联」：
      放宽的只是**同源** <link href="/static/…"> / <script src="/static/…">；
      **离线这条底线原样保留** —— 任何 http(s):// / 协议相对 URL 仍禁止。
      理由：面板 13446 行单文件不可维护，而「离线可用」用
      「无 http(s) 资源」表达比用「无 link」表达更准确。
    """
    src = _html()          # ★ 只看骨架：那两条查的是模板自身的 Jinja 槽位
    # ① 离线底线：一条都不许有 http(s) / 协议相对 / data: 之外的外部引用
    assert re.search(r'''(?:src|href)\s*=\s*["']https?://''', src) is None, \
        '面板不得引用 http(s) 资源（离线底线）'
    assert re.search(r'''(?:src|href)\s*=\s*["']//''', src) is None, \
        '面板不得用协议相对 URL（离线底线）'
    # ② 同源外链只允许指向本仓 static/
    for m in re.finditer(r'''(?:src|href)\s*=\s*["']([^"']+)["']''', src):
        url = m.group(1)
        if url.startswith(('{{', '#', 'data:')):
            continue
        assert url.startswith('/static/'), \
            '外链只允许 /static/（同源离线），实际: %s' % url
    # ③ 模板里不得再有内联 <style>/<script>（已全部外链，见 2026-10-03 ADR）
    assert '<style>' not in src, '样式应外链到 static/panel.css'
    assert '<script>' not in src, '脚本应外链到 static/panel/*.js'


def test_panel_assets_exist():
    """★ 外链出去的 panel.css 与 panel/*.js 十个必须真的存在（防路径写错白屏）。

    ★ 02-hotkey.js 与 04-assist.js 允许只有注释（无代码）：这两页的控件全部由
      00-shared.js 的通用逻辑处理（它们的控件都是「配置项」，没有专属逻辑）。
      纯注释的文件是**有意的占位**，不是截断 —— 它标记了「这一页签预留了位置」。
      ⇒ 判据：这两个文件**不许有代码**（去掉注释与空行后应为空）。
    """
    names = ['panel.css'] + ['panel/%s.js' % n for n in
                             ('00-const', '01-home', '02-hotkey', '03-pointer', '04-assist', '05-model', '06-hardware', '07-preset', '08-license', '09-fan', 'calib-bind', '10-flow')]
    placeholders = ('02-hotkey.js', '04-assist.js')
    for name in names:
        p = STATIC_DIR / name
        assert p.is_file(), '面板外链资源缺失: static/%s' % name
        if name.endswith(placeholders):
            # 必须是「只有注释、没有代码」
            code = [ln for ln in p.read_text(encoding='utf-8').split('\n')
                    if ln.strip() and not ln.strip().startswith('//')]
            assert not code, ('static/%s 是占位文件，不该有代码：%s'
                              % (name, code[:2]))
            continue
        # ★ 不按固定字节数判大小：01-home(39 行) / 03-pointer(15 行) 本身就很小，
        #   但 00-shared / 05-model 是大文件。统一用「去注释后有代码」判定，
        #   再对**大文件**额外要求 > 1KB（防真截断）。
        text = p.read_text(encoding='utf-8')
        code = [ln for ln in text.split('\n')
                if ln.strip() and not ln.strip().startswith('//')]
        assert code, 'static/%s 没有任何代码' % name
        if len(code) > 100:
            assert p.stat().st_size > 1024, 'static/%s 太小，可能截断' % name


def test_panel_assets_are_local_not_empty():
    """外链资源非空且有实际内容（CSS 有规则、JS 合并后有函数）。"""
    css = (STATIC_DIR / 'panel.css').read_text(encoding='utf-8')
    js = '\n'.join(
        (STATIC_DIR / 'panel' / ('%s.js' % n)).read_text(encoding='utf-8')
        for n in ('00-const', '01-home', '02-hotkey', '03-pointer', '04-assist', '05-model', '06-hardware', '07-preset', '08-license', '09-fan', 'calib-bind', '10-flow')
    )
    assert ':root' in css, 'panel.css 缺 :root 变量'
    assert css.count('{') > 100, 'panel.css 规则过少，疑似截断'
    assert 'DOMContentLoaded' in js, 'panel.js 缺入口监听'
    assert 'function' in js, 'panel.js 无函数定义'
    # ★ 搬出时不能残留 style/script 标签（否则嵌套后浏览器不认）
    assert '</style' not in css.lower()
    assert '</script' not in js.lower()


def test_no_jinja_delimiter_leak_beyond_slots():
    """除品牌槽位 + 外链的 url_for 外，模板不得残留其它 Jinja 变量。

    ★ 2026-10-03 外链后新增的合法 Jinja 表达式（外链路径）：
        {{ url_for('static', filename='panel.css') }}
        {{ url_for('static', filename='panel/01-const.js') }} … （共 5 个）
      它们是**本仓自己的** Jinja 调用（不是上游模板带来的变量），
      与「防止上游升级引入 {{ 冲突」的原意不冲突，故列入白名单。
    """
    src = _html()          # ★ 只看骨架：CSS/JS 里不该有 Jinja，但那是另一条断言
    allowed = [
        '{{ ui_skin }}', '{{ default_theme }}', '{{ app_title }}',
        "{{ url_for('static', filename='panel.css') }}",
    ] + ["{{ url_for('static', filename='panel/%s.js') }}" % n
         for n in ['00-const', '01-home', '02-hotkey', '03-pointer', '04-assist', '05-model', '06-hardware', '07-preset', '08-license', '09-fan', 'calib-bind', '10-flow']]
    for token in allowed:
        src = src.replace(token, '')
    assert '{{' not in src
    assert '{%' not in src
    assert '{#' not in src
    # ★ panel/*.js 是静态文件不经 Jinja，其内部一个 {{ 都不许有（另一条断言）


def test_assets_have_no_jinja():
    """★ CSS / JS 里**一个 Jinja 标记都不许有**（它们不走 Jinja 渲染）。

    面板资源是 Flask 的静态文件，不经 Jinja。若将来有人在 panel/*.js 里写
    {{ }}，它会原样出现在浏览器里 ⇒ 静默失效。
    """
    names = ['panel.css'] + ['panel/%s.js' % n for n in
                             ['00-const', '01-home', '02-hotkey', '03-pointer', '04-assist', '05-model', '06-hardware', '07-preset', '08-license', '09-fan', 'calib-bind', '10-flow']]
    for name in names:
        s = (STATIC_DIR / name).read_text(encoding='utf-8')
        assert '{{' not in s, 'static/%s 不该含 Jinja {{' % name
        assert '{%' not in s, 'static/%s 不该含 Jinja {%%' % name


# ===================================================================
# [布局冻结] 现阶段定死该控制台布局（2026-09-18）
#   · 视觉：.app-shell 固定宽度 1660px，不再随窗口自适应；
#   · 基线：12 页签「顺序 + 文案」冻结，4 个上游响应式断点全部停用。
#   任何一处漂移 ⇒ 本段断言失败（= 改布局即测试失败）。
#   依据文档：docs/handover/2026-09-17/控制台布局冻结基线-2026-09-18.md
# ===================================================================


def _nav_block() -> str:
    """侧栏导航 <nav class="module-tabs">…</nav> 片段（页签顺序/文案的唯一来源）。"""
    m = re.search(r'<nav class="module-tabs".*?</nav>', _src(), re.S)
    assert m, '侧栏导航 <nav class="module-tabs"> 缺失'
    return m.group(0)


def test_nav_tab_order_is_frozen():
    """[布局冻结] 侧栏页签顺序锁死：data-page-target 出现顺序必须与基线完全一致。"""
    targets = re.findall(r'data-page-target="([a-z0-9\-]+)"', _nav_block())
    assert targets == EXPECTED_TAB_TARGETS, f'页签顺序漂移: {targets}'


def test_nav_tab_labels_are_frozen():
    """[布局冻结] 侧栏页签文案锁死：编号 + 名称必须逐字与基线一致。"""
    pairs = re.findall(r'<span>(\d{2})</span>([^<\n]+)', _nav_block())
    labels = [num + name.strip() for num, name in pairs]
    assert labels == EXPECTED_TAB_LABELS, f'页签文案漂移: {labels}'


def test_section_ids_match_tab_targets():
    """[布局冻结] 9 个 section id=*-page 的集合必须与页签契约一一对应（不多不少）。"""
    ids = re.findall(r'<section id="([a-z0-9\-]+)-page"', _html())
    expected_ids = {t[: -len('-page')] for t in EXPECTED_TAB_TARGETS}
    assert len(ids) == 9, f'期望 9 个 section.*-page，实际 {len(ids)}: {ids}'
    assert set(ids) == expected_ids, f'section id 集合与页签契约不一致: {sorted(set(ids))}'


def test_layout_width_is_fluid_with_cap():
    """[响应式解冻] `.app-shell` = `min(1660px, calc(100vw - 36px))`：自适应 + 1660px 上限。

    2026-09-18 的冻结写法是 `width: 1660px; min-width: 1660px;`（配 `min(1660px`
    反例断言）。2026-09-29 业主解冻后语义反过来：**必须**是 `min(1660px, ...)`，
    **必须没有**定值 `min-width`（留着它宽度就被锁死，等于没解冻）。
    上限 1660px 本身仍是硬线——改小/改大即失败。
    """
    m = re.search(r'\.app-shell\s*\{([^}]*)\}', _src())
    assert m, '.app-shell 基础规则缺失'
    block = m.group(1)
    width_m = re.search(r'(?m)^\s*width:\s*([^;]+);\s*$', block)
    assert width_m, f'.app-shell 缺少 width 声明: {block!r}'
    expr = width_m.group(1).strip()
    assert expr == f'min({FROZEN_APP_SHELL_MAX_WIDTH}px, calc(100vw - 36px))', \
        f'.app-shell 宽度漂移: {expr!r}（期望 min({FROZEN_APP_SHELL_MAX_WIDTH}px, calc(100vw - 36px))）'
    # 定值 min-width 必须绝迹（行首锚定，规避注释里提到 min-width 的误报）
    assert re.search(r'(?m)^\s*min-width:\s*[0-9.]+px\s*;\s*$', block) is None, \
        f'.app-shell 不得再有定值 min-width（宽度会被锁死，解冻失效）: {block!r}'
    assert 1550 <= FROZEN_APP_SHELL_MAX_WIDTH <= 1750, '上限基线本身异常'


def test_sidebar_width_is_fluid_with_286_cap():
    """[响应式解冻] `--sidebar-width` 必须是 `clamp(..., 286px)`：流体且上限不变。

    宽屏（视口 ≥1682px）取到上限 286px ⇒ 主内容区仍是冻结期的 1348px；
    窗口变窄时侧栏先收缩。上限一旦漂移，宽屏外观就不再与冻结基线逐像素一致。
    """
    m = re.search(r'(?m)^\s*--sidebar-width:\s*([^;]+);', _src())
    assert m, '--sidebar-width 定义缺失'
    expr = m.group(1).strip()
    cm = re.fullmatch(r'clamp\(([^)]*)\)', expr)
    assert cm, f'--sidebar-width 必须是 clamp(下限, 流体值, 上限) 形式: {expr!r}'
    parts = [p.strip() for p in cm.group(1).split(',')]
    assert len(parts) == 3, f'clamp 参数个数异常: {expr!r}'
    assert parts[2] == f'{FROZEN_SIDEBAR_MAX_WIDTH}px', \
        f'侧栏宽上限漂移: {parts[2]}（期望 {FROZEN_SIDEBAR_MAX_WIDTH}px）'


def test_responsive_breakpoints_enabled():
    """[响应式解冻] 4 个断点必须按新阈值启用，且不留 `max-width: 0px` 死条件。

    2026-09-18 冻结期断言「所有 @media 条件 == max-width: 0px」；解冻后反过来：
    条件必须全是**数值宽度** = 基线 4 档，且顺序与模板一致（1400 必须排在 1024 前
    ——断点是后写覆盖先写，顺序错会让窄屏规则被宽屏规则盖掉）。
    """
    src = _src()
    conditions = re.findall(r'@media\s*\(([^)]*)\)', src)
    widths = []
    for cond in conditions:
        mm = re.fullmatch(r'\s*max-width:\s*(\d+)px\s*', cond)
        assert mm, f'断点条件必须是纯 max-width: Npx（不许 min-width / 复合条件）: {cond!r}'
        widths.append(int(mm.group(1)))
    assert tuple(widths) == RESPONSIVE_BREAKPOINTS, (
        f'断点档位漂移: 期望 {RESPONSIVE_BREAKPOINTS}，实际 {tuple(widths)}'
    )
    assert 'max-width: 0px' not in src, '不得残留已停用的 max-width: 0px 断点'
    # 只允许 max-width 断点（移动优先的反向写法会破坏本项目「宽屏定尺」的优先级）
    assert re.search(r'@media\s*\(min-width', src) is None, '不得使用 min-width 断点'


def test_layout_freeze_doc_exists():
    """[布局冻结 + 解冻] 基线文档固化：既留冻结记录，也留解冻记录。

    冻结期只断言 `'1660' in text`（太弱：解冻后把文档改回旧文也能过）。
    解冻起加严：① 必须有 `## 8. 2026-09-29 解冻` 记录节；② **在该节内**抄录了
    外壳新宽度表达式与侧栏 clamp；③ 四个断点宽度逐个出现在该节内。
    """
    assert LAYOUT_FREEZE_DOC.is_file(), f'布局冻结基线文档缺失: {LAYOUT_FREEZE_DOC}'
    text = LAYOUT_FREEZE_DOC.read_text(encoding='utf-8')
    assert '1660' in text, '基线文档必须记录 1660px 上限'
    assert 'max-width: 0px' in text, '基线文档必须保留冻结期的停用写法（回溯用）'
    # 解冻记录必须自成 §8 一节，且把新写法逐个抄录在**该节内**。
    # （不能只查全文：§4 冻结期表格本来就含 560px 等旧值，全文匹配会掩盖漏记。）
    assert '## 8. 2026-09-29 解冻' in text, '基线文档缺 §8「2026-09-29 解冻」记录节'
    sec8 = text.split('## 8. 2026-09-29 解冻', 1)[1]
    assert 'min(1660px, calc(100vw - 36px))' in sec8, '§8 必须抄录外壳新宽度写法'
    assert 'clamp(240px, 17vw, 286px)' in sec8, '§8 必须抄录侧栏宽 clamp 写法'
    for w in RESPONSIVE_BREAKPOINTS:
        assert f'{w}px' in sec8, f'§8 必须记录断点 {w}px'
