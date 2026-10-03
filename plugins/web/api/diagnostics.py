# -*- coding: utf-8 -*-
"""api/diagnostics.py —— USB 代理诊断域 Blueprint。

从 bin/ttbox-web.py 搬出（2026-10-02 web 换写法 S9-b）。
搬出 1 条路由，**URL 一字未改**。

把 usb-proxy 的日志 + 状态打成 zip 供下载。纯只读，不改设备状态。
段外依赖处理：
    · glob / subprocess —— 标准库，直接 import。
    · Response —— flask，直接 import。

★ **禁止在 except 子句里调 hub**（求值时机是函数定义时，此时 hub.bind 还没执行 ⇒ 永久绑成 None ⇒ 该 except 永不匹配）。
  需要按异常类型分流时，抽 `_is_xxx(exc)` 函数在调用时才取类。
"""

from __future__ import annotations

import glob
import subprocess

from flask import Blueprint, Response

bp = Blueprint('diagnostics', __name__)

@bp.get('/api/diagnostics/usb-proxy.zip')
def download_usb_proxy_diagnostics():
    # 真实打包 usbproxy 诊断信息
    import io
    import zipfile
    buf = io.BytesIO()
    with zipfile.ZipFile(buf, 'w', zipfile.ZIP_DEFLATED) as zf:
        try:
            zf.writestr('usbproxy_status.txt', 'TTBOX usb-proxy diagnostic\n')
            try:
                out = subprocess.check_output(['systemctl', 'status', 'ttbox-usbproxy', '--no-pager'], text=True, timeout=5)
                zf.writestr('usbproxy_service.txt', out)
            except Exception:
                zf.writestr('usbproxy_service.txt', 'ttbox-usbproxy service not active\n')
            try:
                devs = sorted(glob.glob('/dev/hidg*'))
                zf.writestr('hidg_devices.txt', '\n'.join(devs) + '\n')
            except Exception:
                pass
        except Exception:
            pass
    buf.seek(0)
    return Response(buf.getvalue(), mimetype='application/zip',
                    headers={'Content-Disposition': 'attachment; filename=usb-proxy.zip'})
