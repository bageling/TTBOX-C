# -*- coding: utf-8 -*-
"""api/preview.py —— MJPEG 预览域 Blueprint。

从 bin/ttbox-web.py 搬出（2026-10-02 web 换写法 S9-b）。
搬出 1 条路由，**URL 一字未改**。

MJPEG 预览流（multipart/x-mixed-replace）。

★ `socket()` 是补丁锚点（test_web_status_honesty.py:185 打了 1 处），
  必须经 hub.get 调用时取 —— 若lib 自带 import socket，替身打不进去。
段外依赖处理：
    · socket —— 经 hub.get 调用时取。
    · os / time —— 标准库，直接 import。
    · Response —— flask，直接 import。

★ **禁止在 except 子句里调 hub**（求值时机是函数定义时，此时 hub.bind 还没执行 ⇒ 永久绑成 None ⇒ 该 except 永不匹配）。
  需要按异常类型分流时，抽 `_is_xxx(exc)` 函数在调用时才取类。
"""

from __future__ import annotations

import os
import time

from flask import Blueprint, Response

from plugins.web.lib import hub
import base64

def ipc_request(*args, **kwargs):
    """入口的 ipc_request —— 调用时取（monkeypatch 锚点×11）。"""
    return hub.call('ipc_request', *args, **kwargs)


bp = Blueprint('preview', __name__)

def _PREVIEW_MONITOR():
    """入口的 _PREVIEW_MONITOR —— 调用时取（单例 / 共享状态，身份须唯一）。"""
    return hub.get('_PREVIEW_MONITOR')


def socket():
    """入口的 socket —— 调用时取（补丁锚点 / 单例 / 异常类，身份须唯一）。"""
    return hub.get('socket')

@bp.get('/api/preview.mjpg')
def preview_stream():
    preview_url = os.environ.get('TTBOX_PREVIEW_URL', '').rstrip('/')
    # Socket 级 streaming proxy：直接透传 8001 的 MJPEG 字节流，
    # 不经 urllib 缓冲（urllib Response 包装引入 26% 帧率损耗 + 300ms 卡顿）。
    if preview_url:
        from urllib.parse import urlparse
        parsed = urlparse(preview_url)
        upstream_host = parsed.hostname or '127.0.0.1'
        upstream_port = parsed.port or 8001

        def proxy_stream():
            _PREVIEW_MONITOR()["active_conns"] += 1
            try:
                while True:
                    try:
                        upstream = socket().socket(socket().AF_INET, socket().SOCK_STREAM)
                        upstream.settimeout(5)
                        upstream.connect((upstream_host, upstream_port))
                        req_line = 'GET /api/preview.mjpg HTTP/1.1\r\nHost: {}:{}\r\n\r\n'.format(upstream_host, upstream_port)
                        upstream.sendall(req_line.encode())
                        # 剥掉上游 HTTP 响应头（读到第一个 CRLFCRLF），只透传 multipart body，
                        # 否则浏览器会在 multipart 流里收到嵌套的 HTTP 头而无法解析。
                        buf = b''
                        while b'\r\n\r\n' not in buf:
                            chunk = upstream.recv(4096)
                            if not chunk:
                                break
                            buf += chunk
                        if b'\r\n\r\n' in buf:
                            body = buf.split(b'\r\n\r\n', 1)[1]
                            if body:
                                _PREVIEW_MONITOR()["last_frame_ts"] = time.time()
                                yield body
                        # 后续字节是纯 multipart 流，直接透传
                        while True:
                            chunk = upstream.recv(65536)
                            if not chunk:
                                break
                            _PREVIEW_MONITOR()["last_frame_ts"] = time.time()
                            yield chunk
                    except (OSError, ConnectionResetError):
                        pass
                    time.sleep(0.5)  # 断线重连间隔
            finally:
                _PREVIEW_MONITOR()["active_conns"] -= 1

        return Response(
            proxy_stream(),
            mimetype='multipart/x-mixed-replace; boundary=ttboxframe',
            headers={'Cache-Control': 'no-store, no-cache', 'X-Accel-Buffering': 'no'},
        )

    # Fallback：直接读 Core IPC（无 8001 时）
    def generate():
        last_seq = -1
        while True:
            r = ipc_request('GET_PREVIEW', timeout=2)
            if r.get('status') == 0:
                d = r.get('data', {})
                b64 = d.get('jpeg_base64')
                seq = d.get('seq', 0)
                if b64 and seq != last_seq:
                    px = base64.b64decode(b64)
                    if px:
                        last_seq = seq
                        yield b'--ttboxframe' + b'\r\n'
                        yield b'Content-Type: image/jpeg' + b'\r\n'
                        yield b'Content-Length: ' + str(len(px)).encode() + b'\r\n\r\n'
                        yield px
                        yield b'\r\n'
            time.sleep(0.01)  # 10ms 轮询（Core 端 ~15fps 决定实际帧率）
    return Response(generate(), mimetype='multipart/x-mixed-replace; boundary=ttboxframe')
