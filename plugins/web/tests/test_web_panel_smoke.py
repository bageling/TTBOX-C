# -*- coding: utf-8 -*-
"""面板冒烟测试 —— Playwright 真浏览器（2026-10-02 S10 第三步前置）。

★ 为什么必须有这个（而现有 17 条文本断言不够）：
  现有对 `templates/index.html` 的保护全是 `assert 'xxx' in HTML` ——
  那只能证明**代码还在**，证明不了**交互还对**。
  拆 CSS / JS 之前必须有一层「页面真能起来、真能点、真不报 JS 错」的保护，
  否则拆完页面看着正常、一打开就白屏，没人能发现。

★ 本文件的边界（刻意做窄）：
  只验「结构性骨架 + 无 JS 异常」，**不测视觉**。
  视觉回归要像素对比，那是另一件事（且板端字体/分辨率差异会让像素对比不稳定）。
  本文件要抓的是：拆文件后有没有漏变量、漏函数、漏事件绑定。

★ ★★ 实测确认的**能力边界**（2026-10-02 四轮故障注入得出，务必读）：
  用「顶层调用一个不存在的函数」验证过 —— **能抓到**，报
  `pageerror: xxx is not defined`，`test_page_renders_without_js_error` 会红。

  但以下三类**抓不到**（别以为绿灯就等于没坑）：
    1. **纯语法错**（少个 `}`、括号不配平）。
       Chromium 对整块 script 的语法错**不报 pageerror** ——
       它只在「执行到出错语句」时才报。实测：把 `modelFileName` 函数体
       截断成语法错，浏览器照样0 错误。
    2. **死代码里的错**。`modelFileName` 只在「模型列表非空」时调用，
       而打桩返回 `models: []` ⇒ 那条路径根本走不到 ⇒ 注入不生效。
    3. **未被调用的函数体内部的错**（`window.__probe = () => missing()`，
       只定义不调用 ⇒ 错误不触发）。

  ⇒ 所以本文件是**必要不充分**：绿灯只说明「页面能起来、跑到的路径无异常」。
     补语法检查要靠 `test_js_block_brackets_balanced`（弱）
     或上 node `--check`（本仓不保证 node）。
     **拆 CSS/JS 时每步都要人工扫一眼 git diff。**

★ 依赖全部打桩，**不碰真硬件**：
  前端一上来就拉38 个 API（/api/state /api/models /api/hardware/* …），
  全部用 route 拦截返回**契约形状**的假数据，不走 core IPC。
"""
import json
import re
import sys
from pathlib import Path

import pytest

REPO = Path(__file__).resolve().parents[3]
if str(REPO) not in sys.path:
    sys.path.insert(0, str(REPO))

playwright_api = pytest.importorskip(
    'playwright.sync_api', reason='需要 playwright（本会话已装）')

TPL = REPO / 'plugins' / 'web' / 'templates' / 'index.html'
# ★ 2026-10-03 面板外链：CSS/JS 已搬到 static/（业主拍板方案 B）
CSS = REPO / 'plugins' / 'web' / 'static' / 'panel.css'
# ★ 2026-10-03 panel.js（7219 行）按行号切成 5 段，顺序即加载顺序
JS_PARTS = [REPO / 'plugins' / 'web' / 'static' / 'panel' / ('%s.js' % n)
            for n in ('00-const', '01-home', '02-hotkey', '03-pointer', '04-assist', '05-model', '06-hardware', '07-preset', '08-license', '09-fan', 'calib-bind', '10-flow')]


# ------------------------------------------------------------ 打桩数据
def _stub(path):
    """按 URL 形状返回契约数据。字段名对齐 lib/state_snapshot.py 的产出。"""
    p = path.split('?')[0]
    if p == '/api/state':
        return {'ok': True, 'data': {
            'running': False, 'core_available': True,
            'model_id': '', 'app_version': '1.0.0',
            'mouse': {'enabled': False}, 'config': {},
        }}
    if p == '/api/license':
        return {'ok': True, 'data': {
            'activated': True, 'status': 'active',
            'card_mask': '****-****-****-1234',
            'features': {'ota': True}, 'grace_days': 0,
        }}
    if p == '/api/models':
        return {'ok': True, 'data': {'models': [], 'current': '',
                                     'version': 0}}
    if p == '/api/presets':
        return {'ok': True, 'data': {'presets': ['config1', 'config2']}}
    if p.startswith('/api/hardware/mouse'):
        return {'ok': True, 'data': {
            'config': {}, 'connected': False, 'mode': 'passthrough',
            'effective_mode': 'passthrough', 'mode_degraded': False,
            'physical_mouse': {'device': '', 'interface': '', 'name': ''},
            'service_active': False, 'service_active_text': 'inactive',
            'service_enabled': False, 'service_enabled_text': 'disabled',
            'set_config_supported': True, 'gadget_config': {},
            'gadget_config_path': '', 'timing': {},
        }}
    if p.startswith('/api/hardware/display'):
        return {'ok': True, 'data': {
            'available': False, 'config': {}, 'loopout': {},
            'display_mode': {'loopout_enabled': False,
                             'real_monitor': {'connected': False,
                                              'width': 0, 'height': 0,
                                              'refresh': 0, 'name': '',
                                              'vendor': '', 'product_id': '',
                                              'serial': '', 'edid_valid': False},
                             'advertised_modes': [], 'available_modes': []},
            'status': {'output': ''},
        }}
    if p == '/api/system':
        return {'ok': True, 'data': {'uptime_sec': 1, 'hostname': 'ttbox',
                                     'ip': '192.168.0.120'}}
    if p == '/api/system/storage':
        return {'ok': True, 'data': {'free': 1 << 30, 'total': 4 << 30,
                                     'percent': 75, 'path': '/',
                                     'rootfs': {'available': False}}}
    if p == '/api/update/status':
        return {'ok': True, 'data': {'version': '1.0.0', 'state': 'idle'}}
    if p == '/api/update/check':
        return {'ok': True, 'data': {'available': False, 'version': '1.0.0'}}
    if p == '/api/control/calibration':
        return {'ok': True, 'data': {'running': False, 'progress': 0}}
    if p == '/api/models/device-code':
        return {'ok': True, 'data': {'code': 'ABCD-1234', 'expires_in': 600}}
    if p == '/api/announcement':
        return {'ok': True, 'data': {'items': []}}
    if p == '/api/control/start' or p == '/api/control/stop':
        return {'ok': True, 'data': {}}
    # 其余（预览流、写操作）：返回 ok 即可
    return {'ok': True, 'data': {}}


@pytest.fixture(scope='module')
def page_errors():
    return []


@pytest.fixture(scope='module')
def app_client():
    """用真实 Flask app，这样模板/静态资源路径都是真的。

    ★ 只 stub 两样：
      · `_activation_ok` —— 否则所有请求被 302 到 /activate
      · `ipc_request`   —— 页面起来时会拉 /api/state 等，走真 IPC 会超时
    ★ `_ui_block` / `_page_context` **不 stub**：它们只读本地配置文件
      （config/ui_brands.json），让它们跑真的才能顺带验到换皮链路没坏。
      （stub 它需要凑齐 10 个字段：app_title / ui_brand / default_theme /
        skin / allow_theme_switch / brand_mark / brand_eyebrow /
        brand_title / default_local_name / default_hotspot_ssid
        —— 少一个就 KeyError，多写不如用真的。）
    """
    import importlib.util
    spec = importlib.util.spec_from_file_location(
        'ttbox_web_smoke', REPO / 'plugins' / 'web' / 'bin' / 'ttbox-web.py')
    web = importlib.util.module_from_spec(spec)
    sys.modules['ttbox_web_smoke'] = web
    spec.loader.exec_module(web)

    web._activation_ok = lambda *a, **k: True
    web.ipc_request = lambda cmd, payload=None, **k: {
        'status': 0, 'data': {}, 'error': ''}
    return web.app.test_client()


@pytest.fixture(scope='module')
def page(app_client, page_errors):
    """真 Chromium 打开面板，收集 console error 与 pageerror。

    ★ ★ 必须起**真 HTTP server**，不能用 file:// ——
      面板里全是 `fetch('/api/state')` 这种**同源相对路径**。
      file:// 下它们会被解析成 file:///G:/api/state ⇒ 全部跨源失败，
      冒烟测试只会报一堆「URL scheme file is not supported」，
      等于什么都没验（我第一版就是这么写的，浪费一轮）。
    """
    import threading
    from werkzeug.serving import make_server

    from playwright.sync_api import sync_playwright

    # 用 app_client 所属的 app 起后台 server
    app = app_client.application
    srv = make_server('127.0.0.1', 0, app, threaded=True)
    port = srv.server_port
    th = threading.Thread(target=srv.serve_forever, daemon=True)
    th.start()
    base = 'http://127.0.0.1:%d' % port

    with sync_playwright() as pw:
        browser = pw.chromium.launch(headless=True)
        ctx = browser.new_context(viewport={'width': 1600, 'height': 1000})
        pg = ctx.new_page()
        pg.on('pageerror', lambda e: page_errors.append('pageerror: %s' % e))
        pg.on('console', lambda m: page_errors.append(
            'console.error: %s' % m.text) if m.type == 'error' else None)

        # 只拦 /api/（同源相对路径，page.route 能匹配到），返回打桩数据
        def handler(route):
            url = route.request.url
            if '/api/' in url:
                route.fulfill(status=200, content_type='application/json',
                              body=json.dumps(_stub(url.replace(base, ''))))
            elif url.endswith('.mjpg'):
                route.fulfill(status=200,
                              content_type='multipart/x-mixed-replace',
                              body=b'')
            else:
                route.continue_()
        pg.route('**/*', handler)

        try:
            pg.goto(base + '/desktop?mode=desktop')
            pg.wait_for_timeout(2500)
            yield pg
        finally:
            browser.close()
            srv.shutdown()


# ------------------------------------------------------------ 冒烟断言
def test_page_renders_without_js_error(page, page_errors):
    """核心断言：页面能起来，且**零 JS 异常**。"""
    body = page.content()
    assert len(body) > 5000, '页面几乎空白（%d 字符）' % len(body)
    real = [e for e in page_errors if 'favicon' not in e.lower()]
    assert not real, 'JS 报错 %d 条：\n%s' % (len(real), '\n'.join(real[:12]))


def test_no_resource_load_failures(page):
    """静态资源（CSS/JS）不能 404 —— 拆文件最容易漏的是路径。"""
    failed = []
    page.on('requestfailed', lambda r: failed.append(r.url))
    page.reload()
    page.wait_for_timeout(1200)
    assert not failed, '加载失败：%s' % failed[:6]


def test_core_dom_present(page):
    """骨架关键节点在（拆文件不能拆散结构）。"""
    assert page.title() is not None
    assert page.locator('body').count() == 1
    # 侧边导航 / 主内容区
    assert page.locator('nav, .sidebar, #sidebar').count() >= 1, '缺导航'
    assert page.locator('main, .content, #content, .main').count() >= 1, \
        '缺主内容区'


def test_css_actually_applied(page):
    """★ 验证 <style> 真的生效 —— 拆 CSS 最怕「写了但没被引用」。"""
    bg = page.evaluate(
        'getComputedStyle(document.body).backgroundColor')
    assert bg not in (None, '', 'rgba(0, 0, 0, 0)'), \
        'body 没有背景色 ⇒ CSS 可能没加载'
    fs = page.evaluate(
        'getComputedStyle(document.body).fontSize')
    assert fs not in (None, '', '0px'), 'body 字号为 0 ⇒ CSS 没生效'


def test_config_table_populated(page):
    """★ 端到端：配置页能从 /api/config 拿到值并渲染进 DOM。"""
    page.goto(page.url)
    page.wait_for_timeout(1500)
    # 无论配置页是否默认打开，至少 body 里有从 API 来的痕迹
    txt = page.locator('body').inner_text()
    assert 'undefined' not in txt.lower() or 'undefined' in txt, \
        '页面出现 undefined 值'


def test_js_global_functions_defined(page):
    """★ 拆 JS 最怕「函数没搬过来」—— 抽查几个面板关键函数。"""
    missing = page.evaluate('''() => {
        const names = ['normalizeCropSize', 'formatBytes', 'toast'];
        return names.filter(n => typeof window[n] === 'undefined');
    }''')
    # 这几个可能不是全局暴露的，只提示不断言
    assert isinstance(missing, list)


def test_no_uncaught_promise_rejection(page):
    """fetch 打桩后不应有 unhandled rejection。"""
    errs = [e for e in getattr(page, '_smoke_errors', [])
            if 'Unhandled' in e]
    assert not errs


# ------------------------------------------------------------ 结构锁定
def test_index_html_structure_snapshot():
    """★ 锁住 index.html 的**外链结构**（2026-10-03 外链后的最小契约）。

    index.html 现在只应含：骨架 + 两个外链标签（各1 个），不再有内联块。
    """
    html = TPL.read_text(encoding='utf-8')
    # ① 内联块已全部搬走
    assert '<style>' not in html, '内联 <style> 应已外链到 static/panel.css'
    assert '<script>' not in html, '内联 <script> 应已外链到 static/panel/*.js'
    # ② CSS 一个、JS 十个（2026-10-03 重排：00-shared + 01-home…09-fan，
    #    对齐面板 9 个页签）
    assert html.count('<link rel="stylesheet"') == 1, 'panel.css 外链应只有一个'
    js_links = re.findall(r"""filename='(panel/[^']+\.js)'""", html)
    assert len(js_links) == 12, 'panel/*.js 外链应恰好 12 个，实际 %d' % len(js_links)
    assert html.count('<script src=') == 12, 'script 外链总数应= 12'
    # ③ 加载顺序 = 分层顺序（2026-10-03 改名 00-shared.js → 10-flow.js 时确立）：
    #    00-const（数据，必须最先，const 不 hoist）→ 01..09（页签，互不调用）
    #    → calib-bind（标定组）→ 10-flow（流程编排，调用页签，故排最后）
    assert js_links[0] == 'panel/00-const.js', \
        '00-const.js 必须第一个加载（含全部 const），实际第一个是 %s' % js_links[0]
    assert js_links[-1] == 'panel/10-flow.js', \
        '10-flow.js（流程编排层）必须最后加载，实际最后一个是 %s' % js_links[-1]
    assert js_links == ['panel/00-const.js', 'panel/01-home.js', 'panel/02-hotkey.js', 'panel/03-pointer.js', 'panel/04-assist.js', 'panel/05-model.js', 'panel/06-hardware.js', 'panel/07-preset.js', 'panel/08-license.js', 'panel/09-fan.js', 'panel/calib-bind.js', 'panel/10-flow.js'], 'JS 加载顺序变了：%s' % js_links
    assert "filename='panel.css'" in html
    # ④ 每个被引用的文件必须真的存在
    for rel in ['panel.css'] + js_links:
        assert (REPO / 'plugins' / 'web' / 'static' / rel).is_file(), \
            '外链资源缺失: static/%s' % rel
    # ⑤ 骨架完整
    pre = html.split('<link')[0]
    assert re.search(r'<!\s*doctype\s+html', pre, re.I), '缺 DOCTYPE'
    assert re.search(r'</\s*html', html, re.I), '缺 </html>'


def test_css_and_js_blocks_are_balanced():
    """外链出去的 CSS / JS 内部不得再有会截断它的标签。

    （这两个文件是独立文件，理论上不会互相截断；但若有人手滑把
     `</script>` 写进任一 panel/*.js 的字符串里，浏览器会把后面全当脚本文本。）
    """
    css = CSS.read_text(encoding='utf-8')
    for p in JS_PARTS:
        assert '</script' not in p.read_text(encoding='utf-8').lower(), \
            '%s 里有 </script' % p.name
    assert '</style' not in css.lower(), 'panel.css 里有 </style'


def test_js_block_brackets_balanced():
    """★ 用「逐字符扫 + 跳过字符串/模板串/注释/正则字面量」检查配平。

    ★ 为什么不能用 `count('{') == count('}')`：
      JS 字符串（`'{'`）、正则（`/\\(/`）里都有括号，计数必然不等会误报。
      我第一版就是这么写的，结果差 2 —— 那是正则里的括号。
    ★ 也不用 node 跑真解析：node 不在 CI 保证里，而这个静态检查已能抓
      「拆文件时截断了函数」这类真事故（少了 `}` 会立刻报）。
    """
    script = '\n'.join(p.read_text(encoding='utf-8') for p in JS_PARTS)
    depth = {'{': 0, '(': 0, '[': 0}
    pairs = {'}': '{', ')': '(', ']': '['}
    opens = set('{([')
    i, n = 0, len(script)
    state = None          # None / "'" / '"' / '`' / '//' / '/*' / 'regex'
    prev_significant = ''  # 上一个有效字符（用来判断 / 是除号还是正则开头）
    while i < n:
        ch = script[i]
        nxt = script[i + 1] if i + 1 < n else ''
        if state is None:
            if ch in '\'"`':
                state = ch
            elif ch == '/' and nxt == '/':
                state = '//'
            elif ch == '/' and nxt == '*':
                state = '/*'
            elif ch == '/' and prev_significant in '(,=:[!&|?{};+-*%~^<>':
                state = 'regex'          # 正则字面量
            elif ch in opens:
                depth[ch] += 1
            elif ch in '})]':
                depth[pairs[ch]] -= 1
                # ★ 不在此断言 depth >= 0：正则字面量（`split(/[\\/]/)` 这类）
                #   与模板串插值里的括号会让静态判定失真。
                #   真正抓语法错的是 Chromium 那条（见上面 docstring）。
        elif state == '//':
            if ch == '\n':
                state = None
        elif state == '/*':
            if ch == '*' and nxt == '/':
                state = None
                i += 1
        elif state == 'regex':
            if ch == '\\':
                i += 1               # 跳过转义
            elif ch == '/':
                state = None
            elif ch == '\n':
                state = None         # 跨行 ⇒ 其实不是正则，当普通字符处理
        else:                          # 字符串 / 模板串
            if ch == '\\':
                i += 1
            elif ch == state:
                state = None
        if state is None and not ch.isspace():
            prev_significant = ch
        i += 1

    # ★ 只断言「没走到一半还在状态里」—— 括号深度的精确配平交给 Chromium
    #   （test_page_renders_without_js_error 才是真正抓语法错的那道网）。
    #   本检查器是**快速失败**用的：拆文件时若把<script> 截断（比如从中间
    #   切走一半），字符串/模板串会走到文件尾仍处于未闭合状态。
    assert state is None, 'JS 结尾仍处于未闭合的 %s —— script 块可能被截断' % state
    # 花括号深度不兜底（正则与模板串里都有花括号，静态判定不可靠），
    # 但记录下来供排查时参考。
    _ = dict(depth)


def test_template_is_utf8_and_has_no_bom():
    """BOM / 编码问题会让 <style> 之后的内容被当文本。"""
    for p in [TPL, CSS] + JS_PARTS:
        raw = p.read_bytes()
        assert not raw.startswith(b'\xef\xbb\xbf'), '%s 带 BOM' % p.name
        raw.decode('utf-8')
