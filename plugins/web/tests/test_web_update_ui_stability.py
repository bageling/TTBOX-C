# test_web_update_ui_stability.py — 系统更新卡片 UI 稳定化 + 页签记忆的回归锁
#
# 背景（业主 2026-09-20 反馈，两轮）：
#   第一轮：「更新完刷新会跳到预设参数那里」+「进度条一会有一会没有，乱显示，简约一点」。
#   第二轮：「刷新就跳到 07 预设参数干吗，刷新完就在本页呆着啊，老是跳来跳去的干吗」——
#           明确了要**停在当前页**，不是跳首页、更不是跳别处。
#
# 本文件锁死修复后的口径：
#   ① 页签记忆：用户点过的每一页都记（**含 08 系统状态**），刷新后停在原页；
#      只有「授权门禁强制跳转」不写记忆（否则解锁后会一直停在授权页）。
#   ② 更新成功后的自动刷新**不得**改写页签记忆（不许强制归位首页）。
#   ③ 更新面板空闲态不再 display:none 收起进度头/进度条（结构恒定，只淡化数值）。
#   ④ renderUpdateStatus 有「更新会话闩」；⑤ installUpdatePlan 提交后上闩。
#
# 运行：python -m pytest plugins/web/tests/test_web_update_ui_stability.py -v
from __future__ import annotations

import pathlib

import pytest

from plugins.web.lib.paths import repo_root as _ttbox_repo_root  # noqa: E402  路径单点真源（A-PATH-3）
REPO_ROOT = pathlib.Path(_ttbox_repo_root())
TEMPLATE = REPO_ROOT / 'plugins' / 'web' / 'templates' / 'index.html'
from plugins.web.tests import panel_src  # noqa: E402  2026-10-03 面板外链：读整个面板

HOME_PAGE_ID = 'home-page'


@pytest.fixture(scope='module')
def template_src() -> str:
    return panel_src.all_src()


def _js_function(src: str, signature: str) -> str:
    """抽出一个 JS 函数体（按大括号配对）。

    先定位参数表的右括号、再找函数体的 `{` —— 不能直接取第一个 `{`，
    因为默认参数（如 `options = {}`）里就带大括号，会截出半个函数头。
    """
    start = src.find(signature)
    assert start >= 0, '未找到函数：%s' % signature
    paren = src.find(')', start)
    assert paren >= 0, '参数表右括号缺失：%s' % signature
    brace = src.find('{', paren)
    assert brace >= 0, '函数体缺失：%s' % signature
    depth = 0
    for i in range(brace, len(src)):
        ch = src[i]
        if ch == '{':
            depth += 1
        elif ch == '}':
            depth -= 1
            if depth == 0:
                return src[start:i + 1]
    raise AssertionError('大括号不配对：%s' % signature)


# ── ① 页签记忆：刷新后停在原页 ─────────────────────────────────────────

def test_page_memory_persists_every_user_clicked_page(template_src):
    # 写入分流按「是否门禁强制跳转」，不再按「是否门禁页」——
    # 后者会让 08 系统状态永远不被记住，在 08 刷新必跳回更早的页签。
    body = _js_function(template_src, 'function initPageNavigation()')
    assert 'setItem(ACTIVE_PAGE_STORAGE_KEY, pageId)' in body
    assert 'gateForced' in body
    assert 'if (!gateForced && options.persistMemory !== false) {' in body


def test_page_memory_restore_does_not_exclude_license_page(template_src):
    body = _js_function(template_src, 'function initPageNavigation()')
    assert 'saved !== LICENSE_GATE_PAGE_ID' not in body, \
        '恢复时仍排除 08 系统状态 ⇒ 在 08 刷新必跳'
    assert 'pages.some((page) => page.id === saved)' in body


def test_gate_activation_is_marked_non_persisting(template_src):
    # 门禁锁定时不写记忆；解锁后用户自己点 08 才写。
    body = _js_function(template_src, 'function activate(pageId, options = {})')
    assert 'state.navigationLockedToLicense' in body
    assert 'persistMemory' in body


# ── ② 更新刷新不得改写页签记忆 ────────────────────────────────────────

def test_update_refresh_does_not_hijack_the_page(template_src):
    # 刷新后停在当前页 —— 不许写 home-page，也不许动页签存储。
    body = _js_function(template_src, 'function scheduleUpdatePageRefresh(')
    assert 'ACTIVE_PAGE_STORAGE_KEY' not in body, '更新刷新仍在改写页签记忆'
    assert '"%s"' % HOME_PAGE_ID not in body, '更新刷新仍在强制跳首页'
    assert 'window.location.reload()' in body


# ── ③ 进度条常驻（不再一会有一会没有）───────────────────────────────

def test_idle_panel_does_not_hide_progress_bar(template_src):
    idx = template_src.find('.update-progress-panel.is-idle')
    assert idx >= 0, '找不到 .update-progress-panel.is-idle 样式块'
    block = template_src[idx:template_src.index('}', idx) + 1]
    assert 'display: none' not in block, '空闲态仍在 display:none 收起进度头/进度条'
    assert 'update-progress-track' not in block, '空闲态仍单独隐藏进度条'


# ── ④⑤ 更新会话闩 ──────────────────────────────────────────────────

def test_render_update_status_has_session_latch(template_src):
    body = _js_function(template_src, 'function renderUpdateStatus(')
    # 收到 idle 时若在会期窗口内必须直接 return，不落到 DOM 写入。
    assert 'updateSessionDeadline' in body
    assert 'Date.now() < state.updateSessionDeadline' in body


def test_install_arms_session_latch(template_src):
    body = _js_function(template_src, 'async function installUpdatePlan(')
    assert 'state.updateSessionDeadline = Date.now() + UPDATE_SESSION_TIMEOUT_MS' in body


def test_state_initialises_session_deadline(template_src):
    assert 'updateSessionDeadline: 0,' in template_src
