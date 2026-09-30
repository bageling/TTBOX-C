"""sim_run.py — 目标锁定抖动仿真（延迟 + 噪声 + deadzone 步进）

场景：准星误差 40px 开始收敛，目标静止。检测框噪声 ±2px 抖动，
HID 输出延迟 delay_ms（输出要 delay 帧后才作用于误差）。
观察稳态：误差幅度、符号翻转（来回抖）、非零输出帧。
"""
import random
import sys
from pid1 import Pid1

GAIN = 0.65       # px per count
DZ = 1.0          # deadzone count


def sim(kp, kd, predict, rate, smooth, steps=900, dist=40.0,
        noise=2.0, delay_ms=30.0, dt_ms=7.5, seed=7, jump_px=0.0,
        jump_every=20, move_px_s=0.0):
    """jump_px: 每 jump_every 帧检测框跳变幅度（低置信度框抖动）
    move_px_s: 目标匀速移动速度（px/s）"""
    random.seed(seed)
    p = Pid1(kp, kd, predict, rate, smooth)
    err = float(dist)
    es = []
    os = []
    delay_frames = max(0, int(round(delay_ms / dt_ms)))
    out_q = [0.0] * delay_frames
    for i in range(steps):
        e = err + random.uniform(-noise, noise)
        if jump_px and i % jump_every == 0:
            e += random.uniform(-jump_px, jump_px)
        u = p.upd(e)
        out = u * GAIN
        if abs(out) < DZ:
            out = 0.0
        out_q.append(out)
        applied = out_q.pop(0) if out_q else 0.0
        err -= applied * GAIN
        if move_px_s:
            err -= move_px_s * (dt_ms / 1000.0)
        es.append(err)
        os.append(out)
    return es, os


def ana(es, os):
    t_e = es[-300:]
    t_o = os[-300:]
    amp = max(abs(x) for x in t_e)
    fl = sum(1 for i in range(1, len(t_e))
             if (t_e[i] > 0.3) != (t_e[i - 1] > 0.3))
    nz = sum(1 for o in t_o if abs(o) >= 1)
    mx = max(abs(o) for o in t_o)
    return amp, fl, nz, mx


def main():
    cfgs = [
        ("当前 kp25 kd0 p1 rate0.2", (25, 0, 1, 0.2, 9900)),
        ("参考 kp25 kd25 p3 rate0.3", (25, 25, 3, 0.3, 9900)),
        ("建议A kp25 kd25 p1", (25, 25, 1, 0.2, 9900)),
        ("建议B kp25 kd25 p0.5", (25, 25, 0.5, 0.2, 9900)),
        ("建议C kp25 kd15 p0.5", (25, 15, 0.5, 0.2, 9900)),
    ]
    delay = float(sys.argv[1]) if len(sys.argv) > 1 else 30.0
    jump = float(sys.argv[2]) if len(sys.argv) > 2 else 0.0
    move = float(sys.argv[3]) if len(sys.argv) > 3 else 0.0
    tag = f"延迟{delay:.0f}ms"
    if jump:
        tag += f" 框跳±{jump:.0f}px/20帧"
    if move:
        tag += f" 目标移动{move:.0f}px/s"
    print(f"目标锁定稳态（{tag} 噪声±2px deadzone=1count）")
    for name, cfg in cfgs:
        es, os = sim(*cfg, delay_ms=delay, jump_px=jump, move_px_s=move)
        amp, fl, nz, mx = ana(es, os)
        print(f"  {name}: 稳态误差幅={amp:5.2f}px 翻转={fl:3d} "
              f"非零输出帧={nz:3d}/300 max={mx:5.2f}count")


if __name__ == "__main__":
    main()
