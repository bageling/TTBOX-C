"""sample_box.py — 只读采样 TTBOX core 的当前目标框尺寸（板端 python3 运行）。

用途：V3 阶段 1「f 实测标定」的第一步 —— 在训练场站在已知距离的靶位，
     读出当前目标框的像素高度 box_h，配合距离 d 和靶子实际高度 h 算焦距：

         f = box_h × d / h

为什么不需要业主操作：这是**纯观测**（GET_STATUS 只读），不注入任何鼠标位移，
     也就不需要按热键Gate（热键只在输出生效那一步才需要）。

坐标口径（已核实）：AimThread.cpp:791 取 selected.box，:207 注释写明
     「框坐标与 capture 同一套 1:1 像素，中心裁剪不缩放」且 :839 用的是
     One-Euro 平滑后的框（特意压制 y1 抖动）⇒ 静止靶下读数稳定，正好可做静态标定。
"""
import json
import os
import socket
import statistics
import sys
import time


def _resolve_sock() -> str:
    """IPC socket 路径：**不写死**，一律从真源取。

    取数顺序：命令行参数 > 环境变量 TTBOX_IPC_SOCKET > 面板 paths.ipc_socket()。
    门禁①规定 socket 字面量只能出现在 Paths.hpp 与 plugins/web/lib/paths.py，
    工具脚本必须走真源（否则升级改路径后会静默连到旧 sock）。
    """
    if len(sys.argv) > 2 and sys.argv[2]:
        return sys.argv[2]
    env = os.environ.get("TTBOX_IPC_SOCKET", "")
    if env:
        return env
    for lib in ("/opt/ttbox/web/lib",):
        if os.path.isdir(lib) and lib not in sys.path:
            sys.path.insert(0, lib)
    try:
        from paths import ipc_socket
        return ipc_socket()
    except Exception:
        return ""


SOCK = _resolve_sock()
if not SOCK:
    print(json.dumps({"samples": 0, "error": "拿不到 IPC socket 路径（用参数或 TTBOX_IPC_SOCKET 指定）"},
                     ensure_ascii=False))
    sys.exit(2)
DURATION = float(sys.argv[1]) if len(sys.argv) > 1 else 6.0


def get_status() -> dict:
    """IPC GET_STATUS。

    ★ 协议（核实自 plugins/web/lib/ipc.py:80）：请求体必须是 JSON + 换行，
      不是裸字符串——早前发 'GET_STATUS\\n' 得到
      "invalid JSON request: 无法识别的字符 'G'"，就是踩的这个坑。
      响应同样是「一行 JSON」，读到含 \\n 为止；真实数据在 'data' 里。
    """
    s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    s.settimeout(2.0)
    try:
        s.connect(SOCK)
        s.sendall(json.dumps({'type': 'GET_STATUS'}).encode() + b'\n')
        buf = b''
        while b'\n' not in buf:
            chunk = s.recv(65536)
            if not chunk:
                break
            buf += chunk
        return json.loads(buf.decode('utf-8', 'replace'))
    finally:
        s.close()


hs, ws = [], []
cid = -1
running_seen = False
t0 = time.time()
while time.time() - t0 < DURATION:
    try:
        st = get_status()
        if int(st.get('status', -1)) != 0:
            time.sleep(0.05)
            continue
        data = st.get('data') or {}
        running_seen = bool(data.get('runtime_running'))
        m = data.get('metrics') or {}
        if m.get('aim_has_target'):
            h = float(m.get("aim_target_height", 0.0))
            w = float(m.get("aim_target_width", 0.0))
            if h > 0:
                hs.append(h)
                ws.append(w)
                cid = int(m.get("aim_target_class_id", -1))
    except Exception:
        pass
    time.sleep(0.05)

if not hs:
    print(json.dumps({"samples": 0, "core_running": running_seen,
                      "error": "画面里没有锁定目标"}, ensure_ascii=False))
    sys.exit(0)

print(json.dumps({
    "samples": len(hs),
    "core_running": running_seen,
    "class_id": cid,
    "box_h_px_median": round(statistics.median(hs), 2),
    "box_h_px_stdev": round(statistics.pstdev(hs), 2) if len(hs) > 1 else 0.0,
    "box_h_px_min": round(min(hs), 2),
    "box_h_px_max": round(max(hs), 2),
    "box_w_px_median": round(statistics.median(ws), 2),
}, ensure_ascii=False))
