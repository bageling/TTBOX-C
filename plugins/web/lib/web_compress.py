# -*- coding: utf-8 -*-
"""静态资源 gzip 压缩 —— 2026-10-03 性能优化第1 项。

★ 为什么不用 werkzeug.middleware.gzip：
  本项目锁定的 Werkzeug 3.1.8 **已经没有这个模块**（实测 import 失败）。
  ⇒ 用标准库 gzip 自己实现，零新增依赖、行为完全可控。

★ 为什么只压静态资源、不碰 API：
  - 静态资源（JS/CSS）体积大、可重复压缩、压缩率最高（实测 ~70%）
  - API 是 JSON，体积小且多为一次性数据，压了收益低、还可能干扰调试
  - `.mjpg`（预览视频流）**绝对不能压**：multipart 流不是静态文本，
    压了会破坏流式边界 ⇒ 显式排除（按实测这是最危险的一条）

★ 为什么不改 Cache-Control: no-cache：
  那条是**故意的**（防止浏览器缓存旧 JS 导致页面异常，见入口 add_no_cache_headers）。
  保留它，只加压缩。压缩与缓存是两件事。
"""
from __future__ import annotations

import gzip
from io import BytesIO

from flask import request

# 只压这些前缀（文本类静态资源）
COMPRESSIBLE_PREFIXES = ('/static/',)
# 明确排除：视频流（multipart），压了会破坏流边界
EXCLUDE_SUFFIXES = ('.mjpg', '.mjpeg', '.mp4', '.png', '.jpg', '.jpeg',
                    '.gif', '.webp', '.ico', '.zip', '.gz', '.br', '.woff',
                    '.woff2', '.ttf', '.eot', '.mp3', '.wav')
# 压缩阈值：小于这个字节数压了反而变大（gzip 头尾有开销）
MIN_BYTES = 1024
# 缓冲上限：超过这个体积的静态资源不读进内存（静态资源本来就该流式传）
MAX_BUFFER_BYTES = 8 * 1024 * 1024
# 压缩级别：6 = zlib 默认，速度与体积平衡（板端是 RK3588，别用 9）
LEVEL = 6


def wants_gzip() -> bool:
    """客户端是否接受 gzip（注意 q=0 表示明确拒绝）。"""
    accept = request.headers.get('Accept-Encoding', '')
    if not accept:
        return False
    for part in accept.split(','):
        bits = part.strip().split(';')
        coding = bits[0].strip().lower()
        if coding != 'gzip':
            continue
        # 处理 gzip;q=0 / gzip;q=0.0（明确不接受）
        for p in bits[1:]:
            p = p.strip()
            if p.startswith('q='):
                try:
                    if float(p[2:]) == 0:
                        return False
                except ValueError:
                    pass
        return True
    return False


def is_compressible(path: str) -> bool:
    if not path.startswith(COMPRESSIBLE_PREFIXES):
        return False
    if path.endswith(EXCLUDE_SUFFIXES):
        return False
    # 只压明确的文本类型
    return path.endswith(('.js', '.css', '.html', '.json', '.svg', '.map', '.txt'))


# ---- 中间件用的无上下文版本（WSGI 层拿不到 flask.request）----

def _is_compressible_path(path: str) -> bool:
    return is_compressible(path or '')


def _wants_gzip_str(accept: str) -> bool:
    """与 wants_gzip() 同口径，但直接吃 WSGI 的原始头字符串。"""
    if not accept:
        return False
    for part in accept.split(','):
        bits = part.strip().split(';')
        if bits[0].strip().lower() != 'gzip':
            continue
        for p in bits[1:]:
            p = p.strip()
            if p.startswith('q='):
                try:
                    if float(p[2:]) == 0:
                        return False
                except ValueError:
                    pass
        return True
    return False


def compress_response(resp):
    """对可压缩的静态响应加 Content-Encoding: gzip（幂等：已压过就跳过）。"""
    try:
        if resp.status_code != 200:
            return resp
        if not is_compressible(request.path):
            return resp
        if resp.headers.get('Content-Encoding'):
            return resp                    # 已被别处压过
        if resp.direct_passthrough:
            return resp
        # ★ Flask 的 send_file 静态响应 is_streamed=True（边读边传）。
        #   但那**只对排除清单里的类型成立**——本函数已经先is_compressible()
        #   过滤掉了 .mjpg / 图片 / 字体等流式媒体，剩下的都是纯文本文件，
        #   get_data() 会把它们完整缓冲到内存（实测 217914B 正常取出）。
        #   ⇒ 这里不再因 is_streamed 而跳过，否则压缩对所有静态文件都不生效。
        #   上限保护：超过 MAX_BUFFER_BYTES 就不缓冲（防止超大文件吃内存）。
        if resp.is_streamed:
            clen = resp.headers.get('Content-Length')
            if clen is None or int(clen) > MAX_BUFFER_BYTES:
                return resp
        data = resp.get_data()
        if len(data) < MIN_BYTES:
            return resp
        buf = BytesIO()
        with gzip.GzipFile(fileobj=buf, mode='wb', compresslevel=LEVEL,
                           mtime=0) as gz:   # mtime=0 ⇒ 输出确定，便于测试
            gz.write(data)
        compressed = buf.getvalue()
        if len(compressed) >= len(data):
            return resp                    # 压不小就别压
        resp.set_data(compressed)
        resp.headers['Content-Encoding'] = 'gzip'
        resp.headers['Content-Length'] = str(len(compressed))
        resp.headers['Vary'] = 'Accept-Encoding'
        return resp
    except Exception:
        # ★ 压缩失败绝不能影响正常响应
        return resp


def install(app):
    """装上压缩钩子（幂等：已装则跳过）。

    ★ 为什么用 WSGI 中间件而不是 @app.after_request：
      Flask 对 `is_streamed` 的响应（send_file 静态文件就是）**会跳过
      after_request 钩子** —— 实测手动调钩子返回 gzip，走框架却没生效。
      ⇒ 改用 `@app.wsgi_app` 包一层，在响应 iterable 落地后再压。
      顺序：先 gzip，再进Flask 的 after_request（no-cache 头）——两件事互不干扰。
    """
    if getattr(app, '_ttbox_gzip_installed', False):
        return app
    inner = app.wsgi_app

    def gzip_middleware(environ, start_response):
        # 先问一次：这次请求要不要压。放外层可以避免无谓的响应缓冲。
        path = environ.get('PATH_INFO', '')
        accept = environ.get('HTTP_ACCEPT_ENCODING', '')
        should = _wants_gzip_str(accept) and _is_compressible_path(path)
        if not should:
            return inner(environ, start_response)

        body = {'chunks': []}

        def _start(status, headers, exc_info=None):
            body['status'] = status
            body['headers'] = headers
            return _start

        # 跑内层应用，收集响应体（静态文本资源体积有限，可安全缓冲）
        result = inner(environ, _start)
        try:
            data = b''.join(result)
        except Exception:
            # 收集失败（真流式资源）⇒ 原样放行
            if result is not None and hasattr(result, 'close'):
                result.close()
            return _passthrough(environ, start_response, body, result)
        finally:
            if result is not None and hasattr(result, 'close'):
                try:
                    result.close()
                except Exception:
                    pass

        status = body.get('status', '200 OK')
        headers = list(body.get('headers', []))
        # 已压过 / 体积不划算⇒ 原样
        if any(k.lower() == 'content-encoding' for k, _ in headers) \
                or len(data) < MIN_BYTES:
            return _emit(start_response, status, headers, data)
        buf = BytesIO()
        try:
            with gzip.GzipFile(fileobj=buf, mode='wb', compresslevel=LEVEL,
                               mtime=0) as gz:
                gz.write(data)
            compressed = buf.getvalue()
        except Exception:
            return _emit(start_response, status, headers, data)
        if len(compressed) >= len(data):
            return _emit(start_response, status, headers, data)
        headers = [(k, v) for k, v in headers if k.lower() != 'content-length']
        headers.append(('Content-Encoding', 'gzip'))
        headers.append(('Content-Length', str(len(compressed))))
        headers.append(('Vary', 'Accept-Encoding'))
        return _emit(start_response, status, headers, compressed)

    def _passthrough(environ, start_response, body, result):
        # 收集失败时的兜底：直接把内层结果回放
        def _sr(status, headers, exc_info=None):
            body['status'] = status
            body['headers'] = headers
            return _sr
        return result

    def _emit(start_response, status, headers, data):
        start_response(status, headers)
        return [data]

    app.wsgi_app = gzip_middleware
    app._ttbox_gzip_installed = True
    return app
