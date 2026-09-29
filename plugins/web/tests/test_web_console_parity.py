# test_web_console_parity.py — 面板 1:1 采用「结构 + 品牌插槽 + 零外联」回归
#
# 背景：面板 = 上游 YU 控制台**整包落地**为 plugins/web/templates/index.html，
# 叠加 4 处品牌插槽（data-ui-brand / data-theme / <title> / body.ui-brand-*）。
# 本文件锁死三件事，防止「换模板/改品牌」时把整包结构或零外联约束悄悄破坏：
#   1 12 个 section.*-page 与 12 个 data-page-target 一一对应（= 参照物的 12 页签契约）；
#   2 4 处品牌插槽齐全（模板 Jinja 变量名与 _page_context() 契约一致）；
#   3 面板**零外联**：无 <link>、无 <script src>、无 http(s):// 资源（本仓无构建链、离线可用）。
#
# 运行：python -m pytest plugins/web/tests/test_web_console_parity.py -v（从仓库根）
from __future__ import annotations

import pathlib
import re

REPO_ROOT = pathlib.Path(__file__).resolve().parents[3]
TEMPLATE = REPO_ROOT / 'plugins' / 'web' / 'templates' / 'index.html'

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
    return TEMPLATE.read_text(encoding='utf-8')


def test_template_exists():
    assert TEMPLATE.is_file(), f'面板模板缺失: {TEMPLATE}'


def test_nine_pages_present():
    """9 个 section id=*-page：多一个/少一个都说明整包落地被裁剪或减法未同步。"""
    ids = re.findall(r'<section id="([a-z0-9\-]+)-page"', _src())
    assert len(ids) == 9, f'期望 9 个 section.*-page，实际 {len(ids)}: {ids}'
    assert set(ids) == set(EXPECTED_PAGES)


def test_nine_page_targets_present():
    """9 个 data-page-target（侧栏导航），与 9 个 section 一一对应。"""
    targets = re.findall(r'data-page-target="([a-z0-9\-]+)"', _src())
    assert len(targets) == 9, f'期望 9 个 data-page-target，实际 {len(targets)}: {targets}'
    assert set(targets) == {p + '-page' for p in EXPECTED_PAGES}


def test_license_gate_overlay_present():
    """面板保留上游 #licenseGateOverlay（运行期掉线/被撤销的就地兜底·第二重）。"""
    src = _src()
    assert 'id="licenseGateOverlay"' in src
    assert 'setLicenseNavigationLock' in src


def test_brand_slots_present():
    """4 处品牌插槽（模板变量名必须与 _page_context() 契约一致）。"""
    src = _src()
    assert 'data-ui-brand="{{ ui_skin }}"' in src
    assert 'data-theme="{{ default_theme }}"' in src
    assert '<title>{{ app_title }}</title>' in src
    assert 'class="license-loading ui-brand-{{ ui_skin }}"' in src


def test_zero_external_resources():
    """零外联：无 <link>、无 <script ... src=...>、无 http(s):// 资源引用。"""
    src = _src()
    assert '<link' not in src, '面板不得引入外链 <link>'
    assert re.search(r'<script[^>]*\ssrc=', src) is None, '面板不得引入 <script src=...>'
    assert re.search(r'(?:src|href)\s*=\s*["\']https?://', src) is None, '面板不得引用 http(s) 资源'
    # 内联脚本/样式是唯一形态（本仓无构建链）
    assert '<style>' in src and '</style>' in src
    assert '<script>' in src and '</script>' in src


def test_no_jinja_delimiter_leak_beyond_slots():
    """除 4 处品牌插槽外，模板不得残留其它 Jinja 变量（防止上游升级引入 {{ 冲突）。"""
    src = _src()
    stripped = (src
                .replace('{{ ui_skin }}', '')
                .replace('{{ default_theme }}', '')
                .replace('{{ app_title }}', ''))
    assert '{{' not in stripped
    assert '{%' not in stripped
    assert '{#' not in stripped


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
    ids = re.findall(r'<section id="([a-z0-9\-]+)-page"', _src())
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
