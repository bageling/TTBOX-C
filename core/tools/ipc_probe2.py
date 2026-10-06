# ★ 分工（业主 2026-10-06 定案「代码全部用 C++，只有脚本用 py」）：
#   本脚本只做「调 C++ / 生成输入 / 判阈值 / 出报告」，不含任何控制逻辑。
#   控制器与物理仿真全在 C++（core/src/aim/*.hpp、core/tools/replay/replay_main.cpp）。
# 板端诊断：打印 GET_STATUS data 结构
import json, socket, os, sys
# IPC socket 默认单点真源（A-PATH-5）：复用同仓 plugins/web/lib/paths.py。
# 根锚发现（P6）：向上找同时含 plugins/framework/scripts/deploy 的目录，
# 不写死 "../../" 相对跳目录（换一次布局就静默指错根）。
_root = os.path.dirname(os.path.abspath(__file__))
while _root != os.path.dirname(_root) and not all(
        os.path.isdir(os.path.join(_root, _n))
        for _n in ("plugins", "framework", "scripts", "deploy")):
    _root = os.path.dirname(_root)
sys.path.append(_root)
from plugins.web.lib.paths import IPC_SOCKET_DEFAULT as _IPC_DEFAULT

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
d = json.loads(data.split(b'\n')[0].decode())
dd = d.get('data')
print('data type:', type(dd))
if isinstance(dd, dict):
    print('data keys:', sorted(dd.keys()))
    for k in dd:
        v = dd[k]
        if isinstance(v, dict):
            print('  %s: dict keys=%s' % (k, sorted(v.keys())[:50]))
        else:
            print('  %s: %r' % (k, str(v)[:100]))
    metrics = dd.get('metrics') if isinstance(dd.get('metrics'), dict) else {}
    print('preview:', {k: metrics.get(k) for k in ('preview_fps', 'preview_encode_ms', 'preview_width', 'preview_height', 'preview_dropped')})
    print('mouse:', {k: metrics.get(k) for k in ('mouse_control_connected', 'mouse_control_socket_write_ok', 'mouse_control_socket_write_fail', 'mouse_control_send_count', 'injection_allowed', 'gated_frames', 'mouse_control_send_count')})
elif isinstance(dd, list):
    print('data is list len', len(dd), 'first:', str(dd[0])[:300] if dd else '')


★ 分工（业主 2026-10-06 定案「代码全部用 C++，只有脚本用 py」）：
  本脚本是**板端运维/诊断**工具，只读不写、不含任何控制逻辑。
  控制器与仿真逻辑全在 C++（core/src/aim/*.hpp、core/tools/replay/replay_main.cpp）。
