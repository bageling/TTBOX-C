# -*- coding: utf-8 -*-
"""面板「连接状态」三条行为锁定 —— 2026-10-03 性能 A 配套。

★ 这三条都来自业主的实测反馈，不是推测：
  ① 「有时候会报 core 未连接」
     → 原因：handleLivePollError **任何一次**轮询失败就立刻把徽章刷成「未连接」，
       单次网络抖动 / core 重启瞬间就会闪一下。
       修法：连续失败 ≥ LIVE_POLL_FAIL_THRESHOLD 才改徽章；成功即清零。
  ② 页面「卡住」
     → 原因：api() 的 fetch 不带 AbortSignal ⇒ 请求挂住就一直等。
       修法：默认 8 秒超时，长耗时接口（OTA 安装）用 timeoutMs 覆盖。
  ③ 首屏 200ms 显示「未连接」
     → 原因：index.html 里写死的初始文案。
       修法：改成「连接中」。
"""
from __future__ import annotations

import re

from plugins.web.tests import panel_src

FLOW = None
TPL = None


def _flow() -> str:
    global FLOW
    if FLOW is None:
        FLOW = (panel_src.STATIC_DIR / 'panel' / '10-flow.js').read_text(encoding='utf-8')
    return FLOW


def _tpl() -> str:
    global TPL
    if TPL is None:
        TPL = panel_src.TEMPLATE.read_text(encoding='utf-8')
    return TPL


def test_badge_needs_consecutive_failures():
    """① 单次失败不得改徽章；必须有阈值。"""
    src = _flow()
    m = re.search(r'const LIVE_POLL_FAIL_THRESHOLD\s*=\s*(\d+)', src)
    assert m, '找不到 LIVE_POLL_FAIL_THRESHOLD（连续失败阈值）'
    assert int(m.group(1)) >= 2, (
        '阈值必须 ≥2：阈值=1 等于没改（单次失败仍会刷徽章），实测值=%s' % m.group(1))
    # handleLivePollError 必须在达标前提前 return
    i = src.index('function handleLivePollError')
    body = src[i:src.index('\n}', i)]
    assert 'livePollFailCount' in body, 'handleLivePollError 没有累加失败计数'
    assert 'livePollFailCount < LIVE_POLL_FAIL_THRESHOLD' in body, (
        'handleLivePollError 缺少「未达阈值就return」的短路')
    # 计数必须先于徽章写入
    assert body.index('livePollFailCount') < body.index('statusBadge'), (
        '必须先累加失败计数，再考虑改徽章')


def test_success_resets_fail_counter():
    """① 成功必须清零，否则恢复后永远显示「未连接」。"""
    src = _flow()
    i = src.index('async function pollLiveState')
    body = src[i:src.index('\n}', i)]
    assert 'livePollFailCount = 0' in body, (
        'pollLiveState 成功路径没有清零 livePollFailCount —— '
        '失败攒到阈值后会永远显示「未连接」')


def test_api_has_default_timeout():
    """② api() 必须有默认超时，且用 AbortController 真正实现。"""
    src = _flow()
    m = re.search(r'const API_DEFAULT_TIMEOUT_MS\s*=\s*(\d+)', src)
    assert m, '找不到 API_DEFAULT_TIMEOUT_MS'
    val = int(m.group(1))
    assert 3000 <= val <= 20000, '默认超时 %d ms 不在合理区间（3~20 秒）' % val
    i = src.index('async function api(')
    body = src[i:src.index('\n}', i)]
    assert 'AbortController' in body, 'api() 没有用 AbortController'
    assert 'controller.abort()' in body, 'api() 没有真正 abort'
    assert 'timedOut' in body, 'api() 没有区分「超时」与「其它网络错误」'
    # ★ 关键：常量必须真被用。故障注入发现「limit 写死 0」时测试仍全绿 ——
    #   只查常量存在不够，必须查「它被赋给超时器」。
    assert re.search(r'typeof\s+timeoutMs\s*===\s*"number"\s*\?\s*timeoutMs\s*:\s*API_DEFAULT_TIMEOUT_MS',
                     body), (
        'API_DEFAULT_TIMEOUT_MS 没有被用作默认超时（可能 limit 被写死 0 ⇒ 超时形同虚设）')
    assert re.search(r'\},\s*limit\s*\)', body), (
        'api() 的定时器没有用 limit（形如`}, limit);`）—— 传入的 timeoutMs 不会生效')
    assert re.search(r'if\s*\(\s*controller\s*&&\s*!signal\s*&&\s*limit\s*>\s*0', body), (
        'api() 没有「limit > 0 才装定时器」的守卫')


def test_long_running_calls_override_timeout():
    """② OTA 安装必须用长超时覆盖 —— core 重启时会挂几秒。"""
    src = _flow()
    n_install = src.count('api("/api/update/install"')
    assert n_install >= 2, '找不到 OTA 安装调用点（%d 处）' % n_install
    for m in re.finditer(r'api\("/api/update/install"', src):
        seg = src[m.start():m.start() + 600]
        assert 'timeoutMs' in seg, (
            'OTA 安装调用没有 timeoutMs 覆盖 —— 会被 8 秒默认超时误杀')
    # 覆盖值必须明显大于默认值
    for m in re.finditer(r'timeoutMs:\s*(\d+)', src):
        assert int(m.group(1)) >= 20000, (
            'timeoutMs 覆盖值 %s 太小，应≥ 20000ms' % m.group(1))


def test_api_passes_through_other_options():
    """② 超时实现不能吞掉调用方传的 method / headers / body。"""
    src = _flow()
    i = src.index('async function api(')
    body = src[i:src.index('\n}', i)]
    assert '...rest' in body, 'api() 用了解构但没有把剩余选项透传（...rest）'
    assert re.search(r'await fetch\(\s*path,\s*\{', body), 'api() 没有把选项传给 fetch'


def test_initial_badge_says_connecting_not_offline():
    """③ 首屏文案不得是「未连接」。"""
    html = _tpl()
    m = re.search(r'<span id="statusBadge"[^>]*>([^<]*)</span>', html)
    assert m, '找不到 statusBadge'
    text = m.group(1).strip()
    assert text != '未连接', (
        '首屏初始文案仍是「未连接」—— 等第一次 /api/state 回来才替换，'
        '实测这段窗口约 200ms，用户会以为 core 掉线')
    assert text == '连接中', '首屏文案应为「连接中」，实际=%r' % text
