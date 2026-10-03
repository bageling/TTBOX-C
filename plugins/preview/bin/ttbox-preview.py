"""Preview process plugin：latest-frame 快照、JPEG/MJPEG HTTP 输出。"""
from __future__ import annotations
import base64
import json
import os
import select
import socket
import sys
import threading
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path

def _ttbox_tree_root():
    """定位 TTBOX 树根（开发机 = 仓库根；板端 = release 树根，即 ``current/``）。

    判据与 ``plugins/web/lib/paths.py::_ROOT_ANCHORS`` 同源：向上找**最近一层**
    同时含 plugins / framework / scripts / deploy 的目录。**不假设目录深度** ——
    原先这里写死 ``parents[1]`` / ``parents[2]``，改一次目录布局就静默指错根。
    """
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
# 两处都 append 到 sys.path 末尾（不用 insert(0)：防遮蔽 stdlib，见 ttbox-web.py 头部说明）
#   · <树根>/plugins/preview —— 本插件目录（preview_contract）
#   · <树根>/plugins/web     —— IPC socket 单点真源 lib/paths.py（A-PATH-5）
sys.path.append(str(_TREE_ROOT / "plugins" / "preview"))
sys.path.append(str(_TREE_ROOT / "plugins" / "web"))
from preview_contract import PreviewConfig, PreviewFrame, PreviewStatus, PixelFormat, now_us
from lib.paths import IPC_SOCKET_DEFAULT as _IPC_SOCKET_DEFAULT

class MemoryFrameSource:
    """测试和进程边界使用的 latest-frame 源；覆盖旧帧，不形成队列。"""
    def __init__(self): self._lock=threading.Lock(); self._frame=None
    def publish(self, data, frame_number, width, height, pixel_format="jpeg", timestamp_us=None):
        frame=PreviewFrame(bytes(data), frame_number, timestamp_us or now_us(), width, height, pixel_format)
        with self._lock: self._frame=frame
    def latest(self):
        with self._lock: return self._frame

class CoreIpcFrameSource:
    """通过既有 GET_PREVIEW IPC 获取 JPEG；不暴露 DMA-BUF 或 Core buffer。"""
    def __init__(self, socket_path=None): self.socket_path=socket_path or os.environ.get("TTBOX_IPC_SOCKET", _IPC_SOCKET_DEFAULT)
    def latest(self):
        if self.socket_path.startswith("tcp:"): return None
        try:
            sock=socket.socket(socket.AF_UNIX, socket.SOCK_STREAM); sock.settimeout(2); sock.connect(self.socket_path)
            sock.sendall(b'{"type":"GET_PREVIEW"}\n'); chunks=[]
            while True:
                chunk=sock.recv(65536)
                if not chunk: break
                chunks.append(chunk)
                if b"\n" in chunk: break
            sock.close(); response=json.loads(b"".join(chunks).split(b"\n",1)[0]); data=response.get("data", {})
            raw=base64.b64decode(data["jpeg_base64"]) if data.get("jpeg_base64") else b""
            if not raw: return None
            return PreviewFrame(raw, int(data.get("seq",0)), now_us(), 0, 0, PixelFormat.JPEG.value)
        except Exception: return None

class PreviewService:
    def __init__(self, source, config=None):
        self.source=source; self.config=config or PreviewConfig(); self.config.validate(); self.enabled=True; self.running=False; self._last=None; self._lock=threading.Lock(); self._dropped=0; self._no_frame_count=0; self._last_pub_seq=-1
        self._fps_samples=[]; self._measured_fps=0.0
    def _record_frame(self, frame):
        """记录真实出帧速率；status.fps 不再回显配置值。"""
        now=time.monotonic()
        if self._fps_samples and self._fps_samples[-1][1] == frame.frame_number:
            return
        self._fps_samples.append((now, frame.frame_number))
        cutoff=now-2.0
        while len(self._fps_samples) > 2 and self._fps_samples[0][0] < cutoff:
            self._fps_samples.pop(0)
        if len(self._fps_samples) >= 2:
            dt=self._fps_samples[-1][0]-self._fps_samples[0][0]
            ds=self._fps_samples[-1][1]-self._fps_samples[0][1]
            if dt > 0.0 and ds >= 0:
                self._measured_fps=float(ds)/dt
    def start(self): self.enabled=True; self.running=True
    def stop(self): self.running=False
    def snapshot(self):
        frame=self.source.latest()
        if frame is None:
            # core 停止/无帧：立即清空缓存断流，防浏览器显示陈旧画框
            self._no_frame_count += 1
            with self._lock:
                self._last=None
                self._last_pub_seq=-1
                self._fps_samples=[]; self._measured_fps=0.0
            return None
        self._no_frame_count = 0
        with self._lock:
            # core 重启 seq 归零：重置已推送序号，强制重新推帧
            if frame.frame_number < self._last_pub_seq:
                self._last_pub_seq=-1
            self._last=frame
            self._last_pub_seq=frame.frame_number
            self._record_frame(frame)
        return frame
    def status(self):
        frame=self.snapshot()
        with self._lock:
            measured=self._measured_fps
        fps=measured if measured > 0.0 else float(self.config.fps)
        return {"enabled":self.enabled,"running":self.running,"health":"healthy" if frame else "unknown","frame_number":frame.frame_number if frame else 0,"timestamp_us":frame.timestamp_us if frame else 0,"width":frame.width if frame else 0,"height":frame.height if frame else 0,"pixel_format":frame.pixel_format if frame else "jpeg","source":frame.source if frame else "core_ipc","fps":round(fps,2),"target_fps":self.config.fps,"measured":measured > 0.0,"dropped":self._dropped}

class PreviewHandler(BaseHTTPRequestHandler):
    service=None
    def setup(self):
        super().setup()
        self.request.settimeout(10)  # 写卡死10秒必退出，防线程/句柄泄漏

    def log_message(self, *_): pass
    def do_GET(self):
        if self.path == "/api/preview.jpg":
            frame=self.service.snapshot()
            if not frame: self.send_error(404,"暂无预览帧"); return
            self.send_response(200); self.send_header("Content-Type","image/jpeg"); self.send_header("Content-Length",str(len(frame.data))); self.end_headers(); self.wfile.write(frame.data); return
        if self.path == "/api/preview/status":
            body=json.dumps(self.service.status(),ensure_ascii=False).encode(); self.send_response(200); self.send_header("Content-Type","application/json"); self.send_header("Content-Length",str(len(body))); self.end_headers(); self.wfile.write(body); return
        if self.path == "/api/preview.mjpg":
            # MJPEG 流：持续推送新帧（不设 Content-Length，保持连接不断开）。
            self.send_response(200)
            self.send_header("Content-Type", "multipart/x-mixed-replace; boundary=ttboxframe")
            self.send_header("Cache-Control", "no-store")
            self.send_header("Connection", "keep-alive")
            self.end_headers()
            last_seq = -1
            try:
                while True:
                    frame = self.service.snapshot()
                    if frame and frame.frame_number != last_seq:
                        last_seq = frame.frame_number
                        self.wfile.write(b"--ttboxframe\r\n")
                        self.wfile.write(b"Content-Type: image/jpeg\r\n")
                        self.wfile.write(b"Content-Length: " + str(len(frame.data)).encode() + b"\r\n\r\n")
                        self.wfile.write(frame.data)
                        self.wfile.write(b"\r\n")
                        self.wfile.flush()
                    else:
                        # 无新帧时小步等待：20ms 步进轮询新帧 + 检测客户端断开(EOF)。
                        # 旧实现 select 固定睡 0.5s → 每帧周期被拉到 ~503ms（≈2fps 卡顿）：
                        # Core 预览 15~30fps（新帧周期 33~66ms），推完一帧后下一圈 snapshot
                        # 大概率还是同一 seq，走 else 分支 select 睡满 0.5s，错过中间所有新帧。
                        # 20ms 步进让新帧一到就返回主循环立即推送，同时保留断开检测。
                        deadline = time.time() + 0.5
                        disconnected = False
                        while time.time() < deadline:
                            r, _, _ = select.select([self.request], [], [], 0.02)
                            if r and not self.request.recv(1, socket.MSG_PEEK):
                                disconnected = True
                                break
                            nxt = self.service.snapshot()
                            if nxt and nxt.frame_number != last_seq:
                                break  # 新帧已就绪，回到主循环立即推送
                        if disconnected:
                            break
            except (BrokenPipeError, ConnectionResetError, TimeoutError, OSError):
                pass
            finally:
                self.close_connection = True  # 确保连接与句柄释放
            return
        self.send_error(404)

def create_server(host="127.0.0.1", port=8001, service=None):
    service=service or PreviewService(CoreIpcFrameSource()); PreviewHandler.service=service
    return ThreadingHTTPServer((host, port), PreviewHandler)

def main():
    service=PreviewService(CoreIpcFrameSource()); service.start(); server=create_server(os.environ.get("TTBOX_PREVIEW_HOST","0.0.0.0"),int(os.environ.get("TTBOX_PREVIEW_PORT","8001")),service); server.serve_forever()
if __name__ == "__main__": main()
