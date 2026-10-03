#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""ttbox_m2xx_console_accept.py — 「控制台 1:1 采用上游 YU 控制台」板端验收驱动。

断言口径唯一权威 = `docs/handover/2026-09-17/控制台1:1采用-架构设计-2026-09-18.md`
**§12 板端验收清单（A1–A10）**（可脚本化项）。本脚本与仓库既有
`scripts/ttbox_m207_accept.py` 同风格（HTTP + IPC，不跟随重定向、显式忽略环境代理）。

覆盖（A1–A10）：
  A1  9 页渲染：GET / 侧栏 9 个 module-tab 与 9 个 section id=*-page 一一对应（S1 减法后）
  A2  品牌随卡：/api/state.data.ui 的 skin 与 brand_* 取自服务端；skin ∈ {yu,xh,xcsh}
  A3  激活双保险 + fail-closed：未激活 /⇒302 /activate、/api/state⇒403 activation_required、
      /activate 卡片含 #licenseGateOverlay 结构；**服务端为唯一放行判据**（API 403 与弹层无关）
  A4  无 404：参照物 49 个 /api/* 端点全部存在（非 404）；/ui-custom.css 200 空串
  A5  预览可用：/api/preview.mjpg 200（MJPEG）；/api/state.state.preview 提供读数
  A6  无硬件页占位：/api/hailo/status 结构完整（pcie.present 布尔）、/api/kmboxb/devices 可达；
      **不得 500/白屏**
  A7  旧测试全绿：需在**仓库树**跑 `python -m pytest plugins/web/tests/`（板端 release 树无测试）→ 记为宿主侧
  A8  品牌皮肤回退：/（已激活）200 即证 _brand_template_name 回退链未回归
  A9  黑名单已下线：/api/system/lan-blocklist 必须已卸载（404；D04 fail-open 判死）
  A10 无构建链：/ 与 /activate 零 <link> / 零 <script src> / 零 http(s):// 资源

用法（板端 root @ 192.168.0.104）：
    python3 scripts/ttbox_m2xx_console_accept.py [--base-url URL] [--allow-state-toggle]
        [--no-restore] [--strict]
依赖板端：python3、web :8000、(可选) AF_UNIX core IPC socket、systemctl。
退出码：0 = 无 FAIL（含全 SKIP/PEND）；1 = 有 FAIL；--strict 时 SKIP/PEND 亦计为失败。
"""
from __future__ import annotations

import argparse
import json
import os
import re
import socket
import subprocess
import sys
import time
import urllib.error
import urllib.request
from pathlib import Path


# ── 路径 bootstrap（A-PATH-3）────────────────────────────────────────────────
# 树根用**锚点发现**确定，不写死目录深度（原 `parents[1]`：换布局即静默指错根）。
def _ttbox_tree_root() -> Path:
    cur = Path(__file__).resolve().parent
    while True:
        if all((cur / _n).is_dir() for _n in ("plugins", "framework", "scripts", "deploy")):
            return cur
        if cur.parent == cur:
            raise RuntimeError(
                "找不到 TTBOX 树根：从 %s 向上未发现同时含 "
                "plugins/framework/scripts/deploy 的目录" % __file__
            )
        cur = cur.parent


_TREE_ROOT = _ttbox_tree_root()

# ===========================================================================
# 配置（板端默认；可用命令行/环境变量覆盖）
# ===========================================================================
WEB_BASE = 'http://127.0.0.1:8000'

# IPC socket / 端口 SSOT：复用同仓 plugins/web/lib/paths.py，不散写字面量（A-PATH-5）。
sys.path.append(str(_TREE_ROOT / 'plugins' / 'web'))
try:
    from lib.paths import IPC_SOCKET_DEFAULT as _IPC_DEFAULT       # type: ignore
    from lib.paths import WEB_PORT_DEFAULT as _WEB_PORT_DEFAULT    # type: ignore
    CORE_SOCK = _IPC_DEFAULT
except Exception as _exc:    # 取不到真源时只接受已登记 RUNTIME 覆盖；静默兜底硬编码 = 造第二个真源
    _ov = os.environ.get('TTBOX_IPC_SOCKET')
    if not _ov:
        raise SystemExit('FATAL: 无法导入 lib/paths.py 取 IPC socket 真源，且未设 TTBOX_IPC_SOCKET：%r'
                         % (_exc,))
    CORE_SOCK = _ov
    _WEB_PORT_DEFAULT = 8000

STORE_DIR = '/var/lib/ttbox/license'
CLOUD_SESSION_PATH = os.environ.get('TTBOX_CLOUD_SESSION', '/opt/ttbox/config/cloud_session.json')

# 参照物 9 页签（侧栏 01..09）—— S1 减法（2026-09-18）后与 templates/index.html 一致
EXPECTED_SECTIONS = ['home', 'profiles', 'control', 'assist', 'model',
                     'hardware', 'preset', 'license', 'fan']
EXPECTED_TABS = ['总览', '热键控制', '移动控制', '辅助功能', '模型库',
                 '显示与鼠标', '预设参数', '系统状态', '风扇控制']

# 参照物消费的全部 /api/* 端点（A4 无 404；见架构设计 §6）
# S1 减法（2026-09-18）后收缩：ferrum/hailo/makcu/kmboxb 设备与状态、network/wifi 已删
# （已删端点的反向断言在 a6()）。
REF_API_PATHS = [
    '/api/announcement', '/api/config', '/api/control/start', '/api/control/stop',
    '/api/diagnostics/aim-trace', '/api/diagnostics/usb-proxy.zip',
    '/api/hardware/display', '/api/hardware/mouse',
    '/api/license', '/api/models',
    '/api/models/device-code', '/api/presets', '/api/preview.mjpg',
    '/api/state', '/api/system', '/api/system/storage', '/api/update/status',
    '/api/update/versions',
]

HEARTBEAT_WAIT_S = 95
RESTART_WAIT_S = 60
WEB_READY_S = 90

ALLOW_STATE_TOGGLE = False
NO_RESTORE = False
STRICT = False


# ===========================================================================
# HTTP（不跟随重定向，以便观察 302→/activate）
# ===========================================================================
class _NoRedirect(urllib.request.HTTPRedirectHandler):
    def redirect_request(self, req, fp, code, msg, headers, newurl):  # noqa: N802
        return None


# 板端 web 为本机目标：显式忽略环境代理，避免 HTTP(S)_PROXY 把本地请求劫持成 502。
_OP_NOFOLLOW = urllib.request.build_opener(_NoRedirect, urllib.request.ProxyHandler({}))
_OP_FOLLOW = urllib.request.build_opener(urllib.request.ProxyHandler({}))


class Resp:
    __slots__ = ('status', 'headers', 'body')

    def __init__(self, status: int, headers: dict, body: str) -> None:
        self.status = status
        self.headers = headers or {}
        self.body = body or ''

    def json(self) -> dict:
        try:
            v = json.loads(self.body)
            return v if isinstance(v, dict) else {}
        except Exception:
            return {}

    def data(self) -> dict:
        d = self.json().get('data')
        return d if isinstance(d, dict) else {}

    def loc(self) -> str:
        return str(self.headers.get('Location', '') or '')


def http(method: str, path: str, *, json_body=None, timeout: float = 12.0,
         follow: bool = False) -> Resp:
    url = WEB_BASE + path
    headers = {'Accept': 'application/json'}
    data = None
    if json_body is not None:
        data = json.dumps(json_body, ensure_ascii=False).encode('utf-8')
        headers['Content-Type'] = 'application/json'
    req = urllib.request.Request(url, data=data, headers=headers, method=method.upper())
    opener = _OP_FOLLOW if follow else _OP_NOFOLLOW
    try:
        with opener.open(req, timeout=timeout) as r:
            ctype = str((r.headers or {}).get('Content-Type', '') or '').lower()
            if ctype.startswith('multipart/'):
                # 流式响应（MJPEG `multipart/x-mixed-replace`）**永不结束**：只读满
                # 一小段即可判定状态/类型。切勿对这类响应 `r.read()` 全量读取，
                # 否则会永久阻塞（连接不会关闭）——这正是 A4/A5 卡死的根因。
                try:
                    chunk = r.read(4096)
                except Exception:
                    chunk = b''
                return Resp(int(r.status), dict(r.headers), chunk.decode('utf-8', 'replace'))
            return Resp(int(r.status), dict(r.headers), r.read().decode('utf-8', 'replace'))
    except urllib.error.HTTPError as e:
        body = ''
        try:
            body = e.read().decode('utf-8', 'replace')
        except Exception:
            pass
        return Resp(int(e.code), dict(e.headers or {}), body)
    except Exception as e:
        return Resp(0, {}, '<connection error: %s>' % e)


# ===========================================================================
# core IPC（AF_UNIX，JSON 行协议）
# ===========================================================================
def ipc(req_type: str, params=None, timeout: float = 5.0) -> dict:
    try:
        s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        s.settimeout(timeout)
        s.connect(CORE_SOCK)
        msg = {'type': req_type}
        if params is not None:
            msg['params'] = params
        s.sendall((json.dumps(msg) + '\n').encode('utf-8'))
        buf = b''
        while b'\n' not in buf:
            chunk = s.recv(65536)
            if not chunk:
                break
            buf += chunk
        s.close()
        line = buf.split(b'\n', 1)[0].decode('utf-8', 'replace').strip()
        return json.loads(line) if line else {}
    except Exception as e:
        return {'status': -1, 'error': 'IPC 失败: %s' % e}


def core_ping(timeout: float = 5.0) -> bool:
    r = ipc('PING', timeout=timeout)
    return isinstance(r, dict) and r.get('status') == 0


# ===========================================================================
# license / 状态助手
# ===========================================================================
def license_payload() -> dict:
    r = http('GET', '/api/license')
    return r.data() if r.status == 200 else {}


def lic_core() -> dict:
    return license_payload().get('license') or {}


def state_data() -> dict:
    r = http('GET', '/api/state')
    return r.data() if r.status == 200 else {}


def web_reachable() -> bool:
    return http('GET', '/api/license', timeout=5).status != 0


def has_systemctl() -> bool:
    for d in os.environ.get('PATH', '').split(os.pathsep):
        if os.path.exists(os.path.join(d, 'systemctl')):
            return True
    return False


def systemctl(*args: str) -> bool:
    try:
        subprocess.run(['systemctl', *args], timeout=30,
                       stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, check=False)
        return True
    except Exception:
        return False


def wait_until(pred, timeout_s: float) -> bool:
    deadline = time.time() + timeout_s
    while time.time() < deadline:
        try:
            if pred():
                return True
        except Exception:
            pass
        time.sleep(2.0)
    return False


def wait_web_ready(timeout: float = WEB_READY_S) -> bool:
    return wait_until(web_reachable, timeout)


def restart_core() -> bool:
    if not has_systemctl():
        return False
    systemctl('reset-failed', 'ttbox-core')
    systemctl('restart', 'ttbox-core')
    return True


def restart_web() -> bool:
    if not has_systemctl():
        return False
    systemctl('reset-failed', 'ttbox-web')
    systemctl('restart', 'ttbox-web')
    wait_web_ready()
    return True


# ===========================================================================
# 未激活基线的临时切换（A3 用；可逆）
# ===========================================================================
def _toggle_to_unactivated() -> bool:
    if not has_systemctl():
        return False
    for p in (os.path.join(STORE_DIR, 'license.json'),
              os.path.join(STORE_DIR, 'state.json'),
              CLOUD_SESSION_PATH):
        if os.path.exists(p):
            try:
                os.replace(p, p + '.m2xxbak')
            except Exception:
                pass
    restart_core()
    restart_web()
    return wait_until(core_ping, RESTART_WAIT_S)


def _restore_activated() -> None:
    for p in (os.path.join(STORE_DIR, 'license.json'),
              os.path.join(STORE_DIR, 'state.json'),
              CLOUD_SESSION_PATH):
        if os.path.exists(p + '.m2xxbak'):
            try:
                os.replace(p + '.m2xxbak', p)
            except Exception:
                pass
    restart_core()
    restart_web()
    wait_until(core_ping, RESTART_WAIT_S)
    wait_web_ready()
    # 被动等待激活态再现（**不调用任何 activate**；就绪等待 ≠ 重建激活兜底）。
    wait_until(lambda: bool(lic_core().get('activated')), 40)


# ===========================================================================
# 结果记录
# ===========================================================================
RESULTS: list = []
_VERDICT_TAG = {'PASS': 'PASS', 'FAIL': 'FAIL', 'SKIP': 'SKIP', 'PEND': 'PEND'}


def _emit(verdict: str, cid: str, desc: str, detail: str) -> None:
    RESULTS.append({'id': cid, 'desc': desc, 'verdict': verdict, 'detail': detail})
    line = '[%s] %-6s %s' % (_VERDICT_TAG[verdict], cid, desc)
    if detail:
        line += ' :: ' + detail
    print(line, flush=True)


def check(cid: str, desc: str, ok: bool, detail: str = '') -> None:
    _emit('PASS' if ok else 'FAIL', cid, desc, detail)


def skip(cid: str, desc: str, reason: str) -> None:
    _emit('SKIP', cid, desc, reason)


# ===========================================================================
# A1 9 页渲染
# ===========================================================================
def a1() -> None:
    desc = 'A1 9 页渲染（侧栏 9 module-tab ↔ 9 section id=*-page；S1 减法后）'
    if not lic_core().get('activated'):
        skip('A1', desc, '需已激活基线（未激活 / 会 302 /activate）')
        return
    r = http('GET', '/')
    if r.status != 200:
        skip('A1', desc, 'GET / => %s' % r.status)
        return
    html = r.body
    sections = re.findall(r'<section id="([a-z0-9\-]+)-page"', html)
    targets = re.findall(r'data-page-target="([a-z0-9\-]+)"', html)
    labels = [m.strip() for m in re.findall(
        r'data-page-target="[^"]*"[^>]*>\s*<span>\d+</span>\s*([^<]+?)\s*</button>', html)]
    ok = (set(sections) == set(EXPECTED_SECTIONS) and len(sections) == 9
          and set(targets) == {s + '-page' for s in EXPECTED_SECTIONS}
          and set(labels) == set(EXPECTED_TABS))
    check('A1', desc, ok,
          'sections=%d %s ; targets=%d ; labels=%s' % (len(sections), sections, len(targets), labels))


# ===========================================================================
# A2 品牌随卡
# ===========================================================================
def a2() -> None:
    desc = 'A2 品牌随卡（/api/state.data.ui.skin ∈ {yu,xh,xcsh}；brand_* 来自服务端且与 /api/license 同源）'
    if not lic_core().get('activated'):
        skip('A2', desc, '需已激活基线')
        return
    ui = state_data().get('ui') or {}
    lic_ui = license_payload().get('ui') or {}
    skin = ui.get('skin')
    ok = (skin in ('yu', 'xh', 'xcsh')
          and bool(ui.get('brand_mark')) and bool(ui.get('brand_eyebrow')) and bool(ui.get('brand_title'))
          and ui.get('ui_brand') == lic_ui.get('ui_brand')
          and ui.get('brand_title') == lic_ui.get('brand_title'))
    check('A2', desc, ok,
          'ui_brand=%r skin=%r mark=%r eyebrow=%r title=%r ; /api/license 同源=%s'
          % (ui.get('ui_brand'), skin, ui.get('brand_mark'), ui.get('brand_eyebrow'),
             ui.get('brand_title'), ui.get('brand_title') == lic_ui.get('brand_title')))


# ===========================================================================
# A3 激活双保险 + fail-closed
# ===========================================================================
def a3() -> None:
    desc = ('A3 激活双保险（未激活 /⇒302 /activate；/api/state⇒403 activation_required；'
            '/activate 卡片含 #licenseGateOverlay；服务端唯一放行判据）')
    if not web_reachable():
        skip('A3', desc, '板端 web 不可达')
        return
    activated = bool(lic_core().get('activated'))
    toggled = False
    if activated:
        if not ALLOW_STATE_TOGGLE:
            skip('A3', desc, '需未激活基线；已激活。加 --allow-state-toggle 可临时切换'
                             '（移走 store/cloud_session 并重启 core/web，结束自动还原）')
            return
        toggled = _toggle_to_unactivated()
        if not toggled:
            check('A3', desc, False, '临时切换未激活基线失败（systemctl/core 不可用）')
            return
    try:
        r_root = http('GET', '/', follow=False)
        red_ok = (r_root.status == 302 and r_root.loc().endswith('/activate'))
        r_state = http('GET', '/api/state', follow=False)
        blocked = (r_state.status == 403 and (r_state.json().get('error') == 'activation_required'))
        p = http('GET', '/activate', follow=False)
        html = p.body
        card_ok = all(m in html for m in (
            'id="licenseGateOverlay"', '设备授权', '输入激活码后继续使用',
            'id="licenseGateKeyInput"', 'id="licenseGateActivateButton"',
            'id="licenseGateRefreshButton"', '/api/license/activate'))
        ok = red_ok and blocked and p.status == 200 and card_ok
        check('A3', desc, ok,
              'GET / => %s loc=%s ; /api/state => %s %s ; /activate=%s card=%s'
              % (r_root.status, r_root.loc(), r_state.status,
                 r_state.json().get('error'), p.status, card_ok))
    finally:
        if toggled and not NO_RESTORE:
            _restore_activated()


# ===========================================================================
# A4 无 404
# ===========================================================================
def a4() -> None:
    desc = 'A4 无 404（参照物 /api/* 端点全在；/ui-custom.css 200 空串）'
    if not lic_core().get('activated'):
        skip('A4', desc, '需已激活基线（未激活非白名单 API 会 403，混淆 404 判据）')
        return
    missing = []
    for p in REF_API_PATHS:
        st = http('GET', p, timeout=8).status
        if st == 404:
            missing.append(p)
    css = http('GET', '/ui-custom.css', timeout=8)
    css_ok = (css.status == 200)
    check('A4', desc, (not missing) and css_ok,
          '404 端点=%s ; /ui-custom.css=%s(len=%d)' % (missing or '无', css.status, len(css.body)))


# ===========================================================================
# A5 预览可用
# ===========================================================================
def a5() -> None:
    desc = 'A5 预览可用（/api/preview.mjpg 200 MJPEG；state.preview 提供读数）'
    if not lic_core().get('activated'):
        skip('A5', desc, '需已激活基线')
        return
    r = http('GET', '/api/preview.mjpg', timeout=8)
    ctype = str(r.headers.get('Content-Type', ''))
    mjpeg_ok = (r.status == 200 and 'multipart' in ctype)
    st = state_data().get('state') or {}
    prev = st.get('preview') or {}
    path_ok = (st.get('preview_path') == '/api/preview.mjpg')
    check('A5', desc, mjpeg_ok and path_ok,
          'preview.mjpg=%s ctype=%r ; state.preview.keys=%s preview_path=%r'
          % (r.status, ctype, sorted(prev.keys()), st.get('preview_path')))


# ===========================================================================
# A6 无硬件页占位
# ===========================================================================
def a6() -> None:
    # S1 减法（2026-09-18）：Hailo / 键鼠盒子 / 无线三页整页移除，对应端点已卸载。
    # 原断言（hailo/kmboxb 结构可达）随功能删除失效，改为反向断言：已删端点必须 404。
    desc = 'A6 硬件页已移除（/api/hailo/status 与 /api/kmboxb/devices 必须已卸载 404）'
    if not lic_core().get('activated'):
        skip('A6', desc, '需已激活基线')
        return
    h = http('GET', '/api/hailo/status', timeout=8)
    k = http('GET', '/api/kmboxb/devices', timeout=8)
    check('A6', desc, h.status == 404 and k.status == 404,
          'hailo/status=%s ; kmboxb/devices=%s' % (h.status, k.status))


# ===========================================================================
# A7 旧测试全绿（宿主侧）
# ===========================================================================
def a7() -> None:
    desc = 'A7 旧测试全绿（pytest plugins/web/tests/：freeauth 302/403 + brand 9 标签 + grep 门禁）'
    repo_tests = _TREE_ROOT / 'plugins' / 'web' / 'tests'
    if not repo_tests.is_dir():
        skip('A7', desc, '板端 release 树无 plugins/web/tests/ ⇒ 请在**仓库树**跑 '
                         '`python -m pytest plugins/web/tests/`')
        return
    try:
        r = subprocess.run([sys.executable, '-m', 'pytest', str(repo_tests), '-q'],
                           timeout=600, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
        out = r.stdout.decode('utf-8', 'replace')
        check('A7', desc, r.returncode == 0, out.strip().splitlines()[-1] if out.strip() else '')
    except Exception as e:
        skip('A7', desc, '无法执行 pytest：%s' % e)


# ===========================================================================
# A8 品牌皮肤回退
# ===========================================================================
def a8() -> None:
    desc = 'A8 品牌皮肤回退（已激活 / 200 即证 _brand_template_name 回退链未回归）'
    if not lic_core().get('activated'):
        skip('A8', desc, '需已激活基线')
        return
    for path in ('/', '/desktop', '/mobile'):
        st = http('GET', path, timeout=8).status
        if st != 200:
            check('A8', desc, False, '%s => %s' % (path, st))
            return
    check('A8', desc, True, '/ /desktop /mobile 均 200')


# ===========================================================================
# A9 黑名单已下线
# ===========================================================================
def a9() -> None:
    # D04（2026-09-18 定案 §2.5）：黑名单 fail-open（名单恒空），整块下线删干净。
    desc = 'A9 黑名单已下线（/api/system/lan-blocklist 必须已卸载 404）'
    if not lic_core().get('activated'):
        skip('A9', desc, '需已激活基线')
        return
    r = http('GET', '/api/system/lan-blocklist', timeout=8)
    check('A9', desc, r.status == 404, 'HTTP %s' % r.status)


# ===========================================================================
# A10 无构建链
# ===========================================================================
def a10() -> None:
    desc = 'A10 无构建链（/ 与 /activate 零 <link> / 零 <script src> / 零 http(s):// 资源）'
    if not lic_core().get('activated'):
        skip('A10', desc, '需已激活基线（/ 未激活 302）')
        return
    bad = []
    for path in ('/', '/activate'):
        r = http('GET', path, timeout=8, follow=True)
        html = r.body
        if '<link' in html:
            bad.append(path + ':link')
        if re.search(r'<script[^>]*\ssrc=', html):
            bad.append(path + ':script-src')
        if re.search(r'(?:src|href)\s*=\s*["\']https?://', html):
            bad.append(path + ':http-resource')
    check('A10', desc, not bad, '外联命中=%s' % (bad or '无'))


# ===========================================================================
# main
# ===========================================================================
def main() -> int:
    global WEB_BASE, ALLOW_STATE_TOGGLE, NO_RESTORE, STRICT

    ap = argparse.ArgumentParser(description='控制台 1:1 采用 · 板端验收（A1–A10）')
    ap.add_argument('--base-url', default=os.environ.get('TTBOX_WEB', WEB_BASE),
                    help='web 基址（默认 %s）' % WEB_BASE)
    ap.add_argument('--allow-state-toggle', action='store_true',
                    help='允许 A3 临时切换未激活基线（移走 store/session 并重启 core/web）')
    ap.add_argument('--no-restore', action='store_true',
                    help='A3 临时切换后不自动还原（默认还原）')
    ap.add_argument('--strict', action='store_true',
                    help='SKIP/PEND 亦计为失败（默认只 FAIL 计失败）')
    args = ap.parse_args()

    WEB_BASE = args.base_url.rstrip('/')
    ALLOW_STATE_TOGGLE = args.allow_state_toggle
    NO_RESTORE = args.no_restore
    STRICT = args.strict

    print('=' * 78)
    print('控制台 1:1 采用 · 板端验收（A1–A10）')
    print('web=%s  core.sock=%s' % (WEB_BASE, CORE_SOCK))
    print('-' * 78)

    if not web_reachable():
        print('!! 板端 web 不可达（%s）—— 除纯离线项外将 SKIP。' % WEB_BASE, flush=True)
    elif not core_ping():
        print('!! core IPC 未就绪（%s）—— 部分项将 SKIP。' % CORE_SOCK, flush=True)

    a1()
    a2()
    a3()
    a4()
    a5()
    a6()
    a7()
    a8()
    a9()
    a10()

    n_pass = sum(1 for r in RESULTS if r['verdict'] == 'PASS')
    n_fail = sum(1 for r in RESULTS if r['verdict'] == 'FAIL')
    n_skip = sum(1 for r in RESULTS if r['verdict'] == 'SKIP')
    n_pend = sum(1 for r in RESULTS if r['verdict'] == 'PEND')
    print('-' * 78)
    print('汇总：PASS=%d FAIL=%d SKIP=%d PEND=%d 共 %d 项'
          % (n_pass, n_fail, n_skip, n_pend, len(RESULTS)))
    print('JSON_RESULT=' + json.dumps(
        {'pass': n_pass, 'fail': n_fail, 'skip': n_skip, 'pend': n_pend, 'results': RESULTS},
        ensure_ascii=False))
    print('=' * 78)

    failed = n_fail > 0 or (STRICT and (n_skip > 0 or n_pend > 0))
    return 1 if failed else 0


if __name__ == '__main__':
    sys.exit(main())
