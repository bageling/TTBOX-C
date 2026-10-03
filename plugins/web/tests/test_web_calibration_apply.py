# test_web_calibration_apply.py — ttbox-web.py 标定结果写回逻辑单测（本地，不启动 Web）
# 验证：标定 gain 写回 mouse.gain_x/y_px_per_count + 拟人化联动，
#      且不再用旧体系 K_LOOP 改写 kp（pid1 体系 kp 保持不动）。
# ★ 2026-10-02（web 换写法 S4）：实现已从 bin/ttbox-web.py 搬到 plugins/web/lib/calibration.py，
#   切片源跟着换文件；命名空间仍注入 _get_runtime_profile / ipc_request / _CFG_WRITE_LOCK
#   三个名字（lib 侧对这三个只用裸名，所以注入照样生效）。
import sys
import threading

src = open('plugins/web/lib/calibration.py', encoding='utf-8').read()

# 提取 _calib_apply_gain 函数体（到下一个顶层 def 为止）
start = src.index('def _calib_apply_gain(')
end = src.index('def _calib_worker(')
func_src = src[start:end]

failures = 0

def check(cond, msg):
    global failures
    if cond:
        print('  PASS:', msg)
    else:
        print('  FAIL:', msg)
        failures += 1

class _FakeIpc:
    def __init__(self):
        self.sent = []

def _make_ns(profile, ipc_status=0):
    ipc = _FakeIpc()
    ns = {
        '_get_runtime_profile': lambda: profile,
        'ipc_request': lambda req_type, params: (ipc.sent.append((req_type, params)),
                                                 {'status': ipc_status, 'error': ''})[1],
        # _calib_apply_gain 现在全程持写锁（读-改-写），测试命名空间得给一把真锁
        '_CFG_WRITE_LOCK': threading.RLock(),
    }
    return ns, ipc

# ---- Case 1: 正常写回，gain 落盘、kp 不动 ----
print('[写回] 正常标定结果')
profile = {
    'mouse': {
        'kp_x': 25.0, 'kp_y': 25.0,
        'rate_x': 0.3, 'rate_y': 0.3,
        'sensitivity': 1.0, 'output_scale': 1.0,
    },
    'personal_trajectory': {'enabled': True},
}
calib = {'mouse_gain_x_px_per_count': 0.62, 'mouse_gain_y_px_per_count': 0.48}
ns, ipc = _make_ns(profile)
exec(func_src, ns)
ok, detail = ns['_calib_apply_gain'](calib)
check(ok, '写回成功')
check(profile['mouse']['gain_x_px_per_count'] == 0.62, 'gain_x_px_per_count 落盘 0.62')
check(profile['mouse']['gain_y_px_per_count'] == 0.48, 'gain_y_px_per_count 落盘 0.48')
check(profile['mouse']['personal_trajectory']['response_px_per_count'] == 0.48, 'response_px_per_count 联动 gain_y（mouse 子对象）')
check(profile['mouse'].get('kp_x') == 25.0, 'kp_x 保持 25（不再被 K_LOOP 改写）')
check(profile['mouse'].get('kp_y') == 25.0, 'kp_y 保持 25（不再被 K_LOOP 改写）')
check(len(ipc.sent) == 1 and ipc.sent[0][0] == 'SET_CONFIG', '提交一次 SET_CONFIG')

# ---- Case 2: gain <= 0 拒绝，不提交 ----
print('[写回] 非法增益')
profile2 = {'mouse': {'kp_x': 25.0}}
ns2, ipc2 = _make_ns(profile2)
exec(func_src, ns2)
ok2, detail2 = ns2['_calib_apply_gain']({'mouse_gain_x_px_per_count': 0, 'mouse_gain_y_px_per_count': 0.5})
check(not ok2 and '增益必须 > 0' in detail2, 'gain=0 被拒绝')
check(len(ipc2.sent) == 0, '未提交 SET_CONFIG')

# ---- Case 3: Core 返回失败，如实上报 ----
print('[写回] Core 拒绝')
profile3 = {'mouse': {'kp_x': 25.0}}
ns3, ipc3 = _make_ns(profile3, ipc_status=4)
exec(func_src, ns3)
ok3, _ = ns3['_calib_apply_gain']({'mouse_gain_x_px_per_count': 0.62, 'mouse_gain_y_px_per_count': 0.48})
check(not ok3, 'Core 失败如实上报（不假成功）')

# ---- Case 4: profile 缺 mouse 子对象也能创建（有顶层键，非空失败） ----
print('[写回] 结构容错')
profile4 = {'model_id': 'x'}
ns4, ipc4 = _make_ns(profile4)
exec(func_src, ns4)
ok4, _ = ns4['_calib_apply_gain']({'mouse_gain_x_px_per_count': 0.55, 'mouse_gain_y_px_per_count': 0.55})
check(ok4 and profile4['mouse']['gain_x_px_per_count'] == 0.55, '缺 mouse 时自动创建并写回')

# ---- Case 4b: 空 profile（Core 读取失败）如实拒绝 ----
print('[写回] 空 profile 拒绝')
profile4b = {}
ns4b, ipc4b = _make_ns(profile4b)
exec(func_src, ns4b)
ok4b, _ = ns4b['_calib_apply_gain']({'mouse_gain_x_px_per_count': 0.55, 'mouse_gain_y_px_per_count': 0.55})
check(not ok4b and len(ipc4b.sent) == 0, '空 profile 视为读取失败，拒绝且不提交')

# ---- Case 5: 缺 gain 字段（只给一个）按 0 拒绝 ----
print('[写回] 缺字段')
profile5 = {'mouse': {}}
ns5, ipc5 = _make_ns(profile5)
exec(func_src, ns5)
ok5, _ = ns5['_calib_apply_gain']({'mouse_gain_x_px_per_count': 0.5})
check(not ok5, '只给一个增益被拒绝（不写半套配置）')

print()
print('结果: %d failures' % failures)
sys.exit(1 if failures else 0)
