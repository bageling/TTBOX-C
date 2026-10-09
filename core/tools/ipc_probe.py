# ★ 分工（业主 2026-10-06 定案「代码全部用 C++，只有脚本用 py」）：
#   本脚本只做「调 C++ / 生成输入 / 判阈值 / 出报告」，不含任何控制逻辑。
#   控制器与物理仿真全在 C++（core/src/aim/*.hpp、core/tools/replay/replay_main.cpp）。
# 板端诊断：打印 GET_STATUS 原始结构
import json, socket, os, sys
# IPC socket 默认单点真源（A-PATH-5）：复用同仓 plugins/web/lib/paths.py。
# 根锚发现（P6）：向上找同时含 plugins/usbproxy/scripts/deploy 的目录，
# 不写死 "../../" 相对跳目录（换一次布局就静默指错根）。
_root = os.path.dirname(os.path.abspath(__file__))
while _root != os.path.dirname(_root) and not all(
        os.path.isdir(os.path.join(_root, _n))
        for _n in ("plugins", "usbproxy", "scripts", "deploy")):
    _root = os.path.dirname(_root)
sys.path.append(_root)
from _ttbox_paths import IPC_SOCKET_DEFAULT as _IPC_DEFAULT

s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
s.settimeout(2.0)
s.connect(os.environ.get('TTBOX_IPC_SOCKET', _IPC_DEFAULT))
s.sendall(json.dumps({'type': 'GET_STATUS'}).encode() + b'\n')
data = b''
while True:
    chunk = s.recv(65536)
    if not chunk:
        break
    data += chunk
    if b'\n' in data:
        break
s.close()
text = data.split(b'\n')[0].decode()
d = json.loads(text)
print('top keys:', sorted(d.keys()))
m = d.get('metrics')
if isinstance(m, dict):
    print('metrics keys (%d):' % len(m), sorted(m.keys()))
else:
    print('metrics type:', type(m), 'value:', str(m)[:300])


★ 分工（业主 2026-10-06 定案「代码全部用 C++，只有脚本用 py」）：
  本脚本是**板端运维/诊断**工具，只读不写、不含任何控制逻辑。
  控制器与仿真逻辑全在 C++（core/src/aim/*.hpp、core/tools/replay/replay_main.cpp）。
