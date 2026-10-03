# 板端临时诊断：高频采样 GET_STATUS(data.metrics)，分析目标框抖动模式
import json, socket, time, sys, statistics, os
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

SECONDS = float(sys.argv[1]) if len(sys.argv) > 1 else 6.0
SOCK = os.environ.get('TTBOX_IPC_SOCKET', _IPC_DEFAULT)

def get_status():
    try:
        s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        s.settimeout(1.0)
        s.connect(SOCK)
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
        return d.get('data', {})
    except Exception as exc:
        return {'error': str(exc)}

samples = []
t0 = time.time()
while time.time() - t0 < SECONDS:
    d = get_status()
    m = d.get('metrics', {}) if isinstance(d, dict) else {}
    samples.append({
        't': round(time.time() - t0, 3),
        'x1': m.get('aim_target_x1'), 'y1': m.get('aim_target_y1'),
        'x2': m.get('aim_target_x2'), 'y2': m.get('aim_target_y2'),
        'err_x': m.get('aim_error_x'), 'err_y': m.get('aim_error_y'),
        'active': m.get('aim_active'), 'has': m.get('aim_has_target'),
        'class': m.get('aim_target_class_id'), 'tid': m.get('aim_target_id'),
        'detect': m.get('detect_count'), 'fps': m.get('fps'),
        'iw': m.get('input_width'), 'ih': m.get('input_height'),
    })
    time.sleep(0.02)

print('samples:', len(samples))
boxed = [s for s in samples if s['x1'] is not None]
print('with box:', len(boxed), '| input:', boxed[0]['iw'], 'x', boxed[0]['ih'] if boxed else '?')
if boxed:
    cx = [(s['x1'] + s['x2']) / 2 for s in boxed]
    cy = [(s['y1'] + s['y2']) / 2 for s in boxed]
    w = [s['x2'] - s['x1'] for s in boxed]
    h = [s['y2'] - s['y1'] for s in boxed]
    print('box x1: %.1f..%.1f  y1: %.1f..%.1f' % (min(s['x1'] for s in boxed), max(s['x1'] for s in boxed), min(s['y1'] for s in boxed), max(s['y1'] for s in boxed)))
    print('box w: %.1f..%.1f (mean %.1f)  h: %.1f..%.1f (mean %.1f)' % (min(w), max(w), statistics.mean(w), min(h), max(h), statistics.mean(h)))
    print('center x: %.2f..%.2f  center y: %.2f..%.2f' % (min(cx), max(cx), min(cy), max(cy)))
    print('center jitter: %.2f px' % max(max(cx) - min(cx), max(cy) - min(cy)))
    dx = [abs(cx[i] - cx[i-1]) for i in range(1, len(cx))]
    dy = [abs(cy[i] - cy[i-1]) for i in range(1, len(cy))]
    print('per-frame center move: x max %.2f mean %.2f | y max %.2f mean %.2f' % (max(dx), statistics.mean(dx), max(dy), statistics.mean(dy)))
    print('class:', set(s['class'] for s in boxed), 'tids:', set(s['tid'] for s in boxed))
    errs = [s['err_x'] for s in samples if s['err_x'] is not None]
    if errs:
        print('err_x range: %.2f..%.2f' % (min(errs), max(errs)))
    print('fps:', set(s['fps'] for s in samples if s['fps']))
    for s in boxed[:40]:
        print('t=%.2f x1=%.1f y1=%.1f x2=%.1f y2=%.1f err=(%.1f,%.1f) has=%s act=%s c=%s id=%s' % (s['t'], s['x1'], s['y1'], s['x2'], s['y2'], s['err_x'] or 0, s['err_y'] or 0, s['has'], s['active'], s['class'], s['tid']))
else:
    acts = [s for s in samples if s['active']]
    print('aim_active frames:', len(acts), '/', len(samples))
    print('detect:', set(s['detect'] for s in samples), 'fps:', set(s['fps'] for s in samples), 'input:', set((s['iw'], s['ih']) for s in samples))
