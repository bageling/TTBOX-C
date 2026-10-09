#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""契约黑盒测试：把 C++ ``ttbox_web`` 起起来，对 86 条路由逐条打 HTTP，断言非 404。

URL 铁律安全网（web-to-cpp-migration-plan.md 附 D / M6 验收）：
    C++ 服务注册的每条路由，用正确方法打一个具体 URL，必须**不返回 404**。
    404 意味着该路由根本没注册 = URL 漂移。Core 离线时 IPC 类路由会返回 500
    或 200+ok:false，但绝不 404；页面路由（/ /desktop /mobile）未激活时 302
    → /activate，也非 404。

用法：
    python3 scripts/web_route_blackbox.py [--binary PATH] [--port N]
"""
import argparse
import http.client
import json
import os
import signal
import subprocess
import sys
import time
import urllib.parse

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SNAPSHOT = os.path.join(REPO, 'scripts', 'web_route_snapshot.txt')
DEFAULT_BINARY = os.path.join(REPO, 'core', 'build-web', 'ttbox_web')
DEFAULT_ROOT = os.path.join(REPO, 'plugins', 'web')

# 快照里的 3 个钩子不是 HTTP 路由（Flask 不放 url_map，httplib 也没有等价物）。
NON_ROUTE_METHODS = {'BEFORE_REQUEST', 'ERRORHANDLER', 'HOOK', 'ERROR'}

# Flask 路径参数 → 具体取值（对齐 C++ 侧 regex 的可匹配值）。
PATH_PARAMS = [
    ('<path:filename>', 'main.js'),
    ('<int:index>', '0'),
    ('<profile_id>', 'probe1'),
    ('<session_id>', 'sess1'),
    ('<theme_id>', 'theme1'),
    ('<version>', '1'),
    ('<name>', 'probe'),
]


def concretize(url: str) -> str:
    """把 Flask 风格的路径参数替换为具体值，得到可打的具体 URL。"""
    for token, value in PATH_PARAMS:
        url = url.replace(token, value)
    # 兜底：任何没认出来的 <...> 都给个安全值。
    while '<' in url and '>' in url:
        left = url.index('<')
        right = url.index('>', left)
        url = url[:left] + 'x' + url[right + 1:]
    return url


def load_snapshot():
    rows = []
    with open(SNAPSHOT, encoding='utf-8') as fh:
        for line in fh:
            line = line.strip()
            if not line or line.startswith('#'):
                continue
            parts = line.split(None, 2)
            if len(parts) == 3:
                rows.append(tuple(parts))
    routes = [(m, u, v) for m, u, v in rows if m not in NON_ROUTE_METHODS]
    return routes


def request_once(host, port, method, url, timeout=8.0):
    """发一次请求，返回 (status, reason)。异常时 status=None。"""
    conn = http.client.HTTPConnection(host, port, timeout=timeout)
    try:
        headers = {}
        body = None
        if method in ('POST', 'PUT', 'PATCH'):
            headers['Content-Type'] = 'application/json'
            body = '{}'
        conn.request(method, url, body=body, headers=headers)
        resp = conn.getresponse()
        status = resp.status
        # 流式端点只读到状态行即可（不消费 body，避免被 MJPEG 拖住）。
        resp.close()
        return status
    except Exception as exc:  # 连接失败 / 超时 / 流被掐
        return None
    finally:
        conn.close()


def wait_ready(host, port, timeout=15.0):
    deadline = time.time() + timeout
    while time.time() < deadline:
        status = request_once(host, port, 'GET', '/api/health', timeout=2.0)
        if status is not None:
            return True
        time.sleep(0.25)
    return False


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--binary', default=DEFAULT_BINARY, help='ttbox_web 可执行文件路径')
    ap.add_argument('--port', type=int, default=18080, help='测试监听端口（默认 18080）')
    ap.add_argument('--host', default='127.0.0.1')
    ap.add_argument('--root', default=DEFAULT_ROOT, help='TTBOX_ROOT（含 static/ templates/）')
    args = ap.parse_args()

    if not os.path.exists(args.binary):
        print('[route-blackbox] 找不到二进制：%s' % args.binary)
        return 2

    routes = load_snapshot()
    if len(routes) != 86:
        print('[route-blackbox] ★快照路由数异常：期望 86，实际 %d —— 解析器可能失效' % len(routes))
        return 2

    workdir = '/tmp/ttbox-webtest-%d' % args.port
    env = dict(os.environ)
    env.update({
        # 监听地址 = WebServer.hpp::kDefaultHost（0.0.0.0），V-20 已删除 TTBOX_WEB_HOST。
        # args.host 仅作**客户端**拨号目标（127.0.0.1 ⊂ 0.0.0.0），故仍需保留。
        'TTBOX_WEB_PORT': str(args.port),
        'TTBOX_ROOT': args.root,
        'TTBOX_PREFIX': workdir,
        'TTBOX_CONFIG_DIR': os.path.join(workdir, 'config'),
        'TTBOX_MOTION_PROFILES_DIR': os.path.join(workdir, 'config', 'motion-profiles'),
        'TTBOX_PRESETS_DIR': os.path.join(workdir, 'presets'),
        'TTBOX_MODELS_ROOT': os.path.join(workdir, 'models'),
        'TTBOX_STATE': os.path.join(workdir, 'state'),
        'TTBOX_IPC_SOCKET': os.path.join(workdir, 'core.sock'),
    })
    os.makedirs(workdir, exist_ok=True)

    proc = subprocess.Popen([args.binary], env=env, stdout=subprocess.DEVNULL,
                            stderr=subprocess.DEVNULL)
    try:
        if not wait_ready(args.host, args.port):
            print('[route-blackbox] ★服务 15s 内未就绪，进程是否存活：%s'
                  % (proc.poll() is None))
            return 2

        print('=== C++ ttbox_web 路由黑盒断言（86 条，非 404 = 已注册）===')
        print('  binary=%s  port=%d  root=%s' % (args.binary, args.port, args.root))

        fails = []
        for method, url, view in sorted(routes, key=lambda r: (r[0], r[1])):
            concrete = concretize(url)
            status = request_once(args.host, args.port, method, concrete)
            ok = status is not None and status != 404
            mark = 'OK ' if ok else 'FAIL'
            print('  [%s] %-6s %-45s -> %s' % (mark, method, concrete,
                                               ('HTTP %s' % status) if status else '无响应'))
            if not ok:
                fails.append((method, url, concrete, status))

        print('  合计：%d 通过 / %d 失败' % (len(routes) - len(fails), len(fails)))
        if fails:
            print('  --- 失败明细 ---')
            for method, url, concrete, status in fails:
                print('  FAIL %-6s %s (%s) -> %s' % (method, url, concrete, status))
            return 1
        print('  结论：PASS —— 86 条路由全部命中（无 404，URL 零漂移）')
        return 0
    finally:
        proc.send_signal(signal.SIGTERM)
        try:
            proc.wait(timeout=5)
        except subprocess.TimeoutExpired:
            proc.kill()


if __name__ == '__main__':
    sys.exit(main())
