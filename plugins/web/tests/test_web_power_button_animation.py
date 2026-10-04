# -*- coding: utf-8 -*-
"""真浏览器量启停按钮的**实际动画曲线** —— 2026-10-04（业主："动画很生硬"）。

★ 为什么源码断言（`test_web_power_button_feel.py`）不够，非要开真浏览器：
  源码断言只能证明"transition 写对了"，**证明不了它真的在动**。常见的反向情形：
    · 写了 transition，但被更高特异性的规则覆盖 ⇒ 源码断言全绿、浏览器里硬跳；
    · 写了 transition，但属性名不在过渡集（如写了 `background-color` 而实际改的是
      `background` 简写的渐变）⇒ 同样硬跳；
    · `is-busy` 被 JS 里另一处 `className =` 冲掉 ⇒ 类加了但立刻没了。
  这些只有 **DOM + computed style + rAF 采样**才看得见。

★ 本文件量的是"丝滑"这个词的**可观测代理**：
  1. 给按钮加 `is-busy`，用 requestAnimationFrame 连续采 `transform` 的 scale，
     要求采到**多个不同的中间值**（硬跳只会得到 1~2 个值）。
  2. 首次采样的 scale 必须仍接近 1（说明是"过渡"而不是"瞬间到位"）。
  3. 末次采样必须收敛到 0.97。
  4. 忙态**不许**改 opacity（业主点名"看着像坏了"的那一版就是靠 opacity）。
  5. 外环的 animation-duration 必须从 7s 变成 0.9s。
  6. 点下按钮 → 文字变「启动中…」的延迟必须很短（业主说的"点了没反应/卡顿"）。
  7. 忙态至少维持 260ms（短请求下不许"抖一下"）。

★ 依赖全部打桩，**不碰真硬件**（复用冒烟测试那套 app_client 桩）。
"""
from __future__ import annotations

import json
import sys
import threading
from pathlib import Path

import pytest

from plugins.web.lib.paths import discover_root

REPO = Path(discover_root(__file__))
if str(REPO) not in sys.path:
    sys.path.insert(0, str(REPO))

pytest.importorskip('playwright.sync_api', reason='需要 playwright')

# 复用冒烟测试的 Flask 桩（_activation_ok / ipc_request 都已打桩）
from plugins.web.tests.test_web_panel_smoke import _stub  # noqa: E402

# 面板期望的最短忙态时长（与 10-flow.js 里的常量同值；改一处必须改两处）
MIN_BUSY_MS = 260


def _stub2(url: str) -> dict:
    """在冒烟桩之上补两处：
      · /api/state 要带 license.valid=true，否则 renderRuntimePowerButton 会把
        启动按钮置 disabled（disabled 的按钮在 Chromium 里**不派发 click**）；
      · /api/control/start|stop 返回带 license 的运行态，贴近真实响应形状。
    """
    p = url.split('?')[0]
    d = _stub(url)
    valid = {'valid': True, 'status': 'active', 'activated': True}
    if p == '/api/state':
        d = json.loads(json.dumps(d))
        d['data']['license'] = valid
    elif p in ('/api/control/start', '/api/control/stop'):
        running = p.endswith('/start')
        d = {'ok': True, 'data': {'running': running,
                                  'status': 'running' if running else 'stopped',
                                  'license': valid,
                                  'core': {'loaded': True, 'status': 'loaded'}}}
    return d


@pytest.fixture(scope='module')
def page():
    """真 Chromium 打开面板（起真 HTTP server，理由见冒烟测试的 fixture 注释）。"""
    import importlib.util

    from werkzeug.serving import make_server

    from playwright.sync_api import sync_playwright

    spec = importlib.util.spec_from_file_location(
        'ttbox_web_anim', REPO / 'plugins' / 'web' / 'bin' / 'ttbox-web.py')
    web = importlib.util.module_from_spec(spec)
    sys.modules['ttbox_web_anim'] = web
    spec.loader.exec_module(web)
    web._activation_ok = lambda *a, **k: True
    web.ipc_request = lambda cmd, payload=None, **k: {'status': 0, 'data': {}, 'error': ''}

    srv = make_server('127.0.0.1', 0, web.app, threaded=True)
    base = 'http://127.0.0.1:%d' % srv.server_port
    threading.Thread(target=srv.serve_forever, daemon=True).start()

    errors = []
    with sync_playwright() as pw:
        browser = pw.chromium.launch(headless=True)
        ctx = browser.new_context(viewport={'width': 1600, 'height': 1000})
        pg = ctx.new_page()
        pg.on('pageerror', lambda e: errors.append('pageerror: %s' % e))
        pg.on('console', lambda m: errors.append('console.error: %s' % m.text)
              if m.type == 'error' else None)

        def handler(route):
            url = route.request.url
            if '/api/' in url:
                route.fulfill(status=200, content_type='application/json',
                              body=json.dumps(_stub2(url.replace(base, ''))))
            elif url.endswith('.mjpg'):
                route.fulfill(status=200,
                              content_type='multipart/x-mixed-replace', body=b'')
            else:
                route.continue_()
        pg.route('**/*', handler)
        try:
            pg.goto(base + '/desktop?mode=desktop')
            pg.wait_for_timeout(2500)
            pg.errors = errors
            yield pg
        finally:
            browser.close()
            srv.shutdown()


# ---------------------------------------------------------------- 前置
def test_panel_loads_without_js_error(page):
    real = [e for e in page.errors if 'favicon' not in e.lower()]
    assert not real, 'JS 报错：\n%s' % '\n'.join(real[:10])


def test_start_button_is_clickable(page):
    """按钮必须可用 —— 否则后面的点击测量全是空跑（假绿）。"""
    state = page.evaluate("""() => {
        const el = document.getElementById('startButton');
        return {exists: !!el, disabled: el && el.disabled,
                label: el && el.querySelector('strong').textContent};
    }""")
    assert state['exists'], '找不到 #startButton'
    assert state['disabled'] is False, '按钮是 disabled（license 桩没生效）'
    assert state['label'] == '启动', '初始文案不是「启动」（实得 %r）' % state['label']


# ---------------------------------------------------------------- 1. 过渡真的在跑
def test_transform_transition_is_smooth_not_a_jump(page):
    """过渡真的在跑？—— 主判据问浏览器要**过渡对象本体**，rAF 中间态只作辅证。

    ★ 为什么主判据不是 rAF 采样：headless 无 GPU 时帧投放会抖，实测偶发整段 180ms
      只投 2 帧 ⇒ 中间态只剩 1 个 ⇒ **假红**。`getAnimations()` 返回的是浏览器
      为这次样式变化**真正创建**的 CSSTransition 对象，属性名 / 时长 / 播放态
      全都确定性可读，不受帧率影响。
    ★ 为什么还要 rAF：`getAnimations()` 证明"过渡被建起来了"，rAF 采样证明
      "它是渐进的、不是一步到位"。硬跳时中间态为 0 个，仍然会红。
    """
    got = page.evaluate("""async () => {
        const el = document.getElementById('startButton');
        const scale = () => {
            const m = new DOMMatrixReadOnly(getComputedStyle(el).transform);
            return Number(m.a.toFixed(4));
        };
        const ran = [];
        const onRun = (e) => ran.push(e.propertyName);
        el.addEventListener('transitionrun', onRun);
        const out = [];
        const t0 = performance.now();
        el.classList.add('is-busy');
        const anims = el.getAnimations().map((a) => ({
            type: a.constructor.name,
            prop: a.transitionProperty === undefined ? null : a.transitionProperty,
            dur: a.effect.getComputedTiming().duration,
            state: a.playState,
        }));
        await new Promise((done) => {
            const tick = () => {
                out.push([Math.round(performance.now() - t0), scale()]);
                if (performance.now() - t0 < 320) requestAnimationFrame(tick);
                else done();
            };
            requestAnimationFrame(tick);
        });
        el.classList.remove('is-busy');
        el.removeEventListener('transitionrun', onRun);
        return {samples: out, ran, anims};
    }""")
    assert 'transform' in got['ran'], (
        '浏览器压根没为 transform 派发过渡事件（transitionrun=%s）⇒ 属性名不在'
        '过渡集，或被更高特异性的规则覆盖' % got['ran'])
    trans = [a for a in got['anims']
             if a['type'] == 'CSSTransition' and a['prop'] == 'transform']
    assert trans, '浏览器没为 transform 建 CSSTransition（实得 %s）' % got['anims']
    assert abs(trans[0]['dur'] - 180) < 1, (
        'transform 过渡时长是 %s ms，不是面板声明的 180ms ⇒ 手感会变' % trans[0]['dur'])
    samples = got['samples']
    assert len(samples) >= 2, 'rAF 只采到 %d 个点，采样不足' % len(samples)
    values = [v for _, v in samples]
    assert values[0] > 0.9705, (
        '首个采样就已经是末态 %s ⇒ 一步到位，没有过渡' % values[0])
    assert values[-1] == pytest.approx(0.97, abs=0.004), '末态不是 0.97（实得 %s）' % values[-1]
    mids = {v for v in values if 0.9705 < v < 0.9995}
    assert len(mids) >= 1, (
        '过渡中间态一个都没采到（%s）⇒ 观感是硬跳' % sorted(values))


def test_transition_is_actually_declared_in_computed_style(page):
    """源码写了 transition 不等于浏览器认 —— 可能被更高特异性的规则覆盖。"""
    css = page.evaluate("""() => {
        const s = getComputedStyle(document.getElementById('startButton'));
        return {prop: s.transitionProperty, dur: s.transitionDuration};
    }""")
    assert 'transform' in css['prop'], 'computed transitionProperty 里没有 transform：%s' % css
    durs = [d for d in css['dur'].split(',')]
    assert any(d not in ('0s', '0ms') for d in durs), (
        'computed transitionDuration 全是 0 ⇒ 过渡被覆盖掉了：%s' % css['dur'])


# ---------------------------------------------------------------- 2. 忙态观感
def test_busy_state_does_not_dim_the_button(page):
    op = page.evaluate("""() => {
        const el = document.getElementById('startButton');
        el.classList.add('is-busy');
        const v = getComputedStyle(el).opacity;
        el.classList.remove('is-busy');
        return v;
    }""")
    assert float(op) == pytest.approx(1.0, abs=0.01), (
        '忙态把按钮调暗了（opacity=%s）⇒ 文字发糊、看着像"坏了"（业主点名的那版）' % op)


def test_busy_state_speeds_up_the_ring(page):
    got = page.evaluate("""() => {
        const el = document.getElementById('startButton');
        const ring = el.querySelector('.power-ring');
        const read = () => getComputedStyle(ring, '::before').animationDuration;
        const before = read();
        el.classList.add('is-busy');
        const after = read();
        el.classList.remove('is-busy');
        return {before, after, restored: read()};
    }""")
    assert got['before'] == '7s', '空闲时外环不是 7s（实得 %s）' % got['before']
    assert got['after'] == '0.9s', '忙态外环没加速（实得 %s）' % got['after']
    assert got['restored'] == '7s', '移除忙态后外环没还原（实得 %s）' % got['restored']


# ---------------------------------------------------------------- 3. 点击响应
def test_click_gives_instant_feedback_and_holds_busy_long_enough(page):
    """点下去 → 「启动中…」的延迟 + 忙态最短时长。这是"不卡顿/不抖一下"的量。

    ★ 文案必须在**忙态出现的那一刻**记（`rec.label`），不能在结束时读 ——
      结束时忙态已清、按钮已按真实状态渲染成「停止」，读到的必然是「停止」。
    """
    log = page.evaluate("""async () => {
        const el = document.getElementById('startButton');
        const label = el.querySelector('strong');
        const rec = {busyAt: null, textAt: null, clearedAt: null, label: null,
                     t0: performance.now()};
        const obs = new MutationObserver(() => {
            const now = performance.now();
            if (rec.busyAt === null && el.classList.contains('is-busy')) rec.busyAt = now;
            if (rec.textAt === null && label.textContent.includes('中')) {
                rec.textAt = now;
                rec.label = label.textContent;
            }
            if (el.classList.contains('is-busy') === false && rec.busyAt !== null
                && rec.clearedAt === null) {
                rec.clearedAt = now;
                obs.disconnect();
                done();
            }
        });
        let done = null;
        const finished = new Promise((r) => { done = r; });
        obs.observe(el, {attributes: true, attributeFilter: ['class']});
        obs.observe(label, {childList: true, characterData: true, subtree: true});
        rec.t0 = performance.now();
        el.click();
        const guard = new Promise((r) => setTimeout(r, 5000));
        await Promise.race([finished, guard]);
        obs.disconnect();
        return {busyIn: rec.busyAt - rec.t0, textIn: rec.textAt - rec.t0,
                busyFor: rec.clearedAt - rec.busyAt, label: rec.label,
                endLabel: label.textContent};
    }""")
    assert log['busyIn'] is not None and log['busyIn'] < 60, (
        '点下去到出现忙态用了 %s ms ⇒ 体感"点了没反应"' % log['busyIn'])
    assert log['textIn'] is not None and log['textIn'] < 60, (
        '点下去到文字变「启动中…」用了 %s ms' % log['textIn'])
    assert log['busyFor'] >= MIN_BUSY_MS, (
        '忙态只持续 %s ms（应 >= %d）⇒ 短请求下按钮会"抖一下"'
        % (log['busyFor'], MIN_BUSY_MS))
    assert log['label'] and '中' in log['label'], (
        '忙态文案不对（忙态那刻实得 %r，结束时 %r）' % (log['label'], log['endLabel']))


def test_busy_class_is_cleared_after_click(page):
    """点完之后不许残留 is-busy —— 残留会让按钮永远保持"按下去"的形状。"""
    page.wait_for_timeout(500)
    got = page.evaluate("""() => {
        const el = document.getElementById('startButton');
        const s = getComputedStyle(el);
        return {busy: el.classList.contains('is-busy'),
                disabled: el.disabled,
                label: el.querySelector('strong').textContent,
                transform: s.transform, opacity: s.opacity};
    }""")
    assert got['busy'] is False, '点完仍残留 is-busy'
    assert got['disabled'] is False, '点完按钮还禁用着'
    m = page.evaluate("""() => new DOMMatrixReadOnly(
        getComputedStyle(document.getElementById('startButton')).transform).a""")
    assert float(m) == pytest.approx(1.0, abs=0.005), '点完按钮没弹回原尺寸（scale=%s）' % m
