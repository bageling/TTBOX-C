"""_scan_gain.py — 带 V1.0.39 gain 换算的 PID 参数扫描（临时诊断，不进产品）。

模拟板端真实行为：Pid1Controller.hpp 的 gain 换算（tv = ed + last_u*gain）。
对比：pid1 原文 / 标定写回 / 中间档，在静态目标 + 移动目标两个场景下的稳态表现。
"""
import random


class Pid1Gain:
    """Pid1Controller.hpp 的 Python 移植（含 V1.0.39 gain 换算）。"""
    def __init__(self, kp, kd, predict, rate, smooth, gain):
        self.kp = kp; self.kd = kd; self.predict = predict
        self.rate = rate; self.smooth = smooth; self.gain = gain
        self.kb = 10000.0
        self.kp_gain = 0.0; self.igain = 0.0; self.u = 0.0
        self.le = 0.0; self.lu = 0.0
        self.vfx = 0.0; self.vfp = 1.0; self.ifx = 0.0; self.ifp = 1.0

    def st(self, v, bw, os):
        r = v / bw; q = r * r
        return (r * (1.0 + (4.0/15.0)*q) / (1.0 + (3.0/5.0)*q)) * os

    def upd(self, e):
        if abs(e) < 0.3: e = 0.0
        if abs(e - self.le) > 30.0: self.reset()
        ae = abs(e)
        if ae < 50.0:
            r = 1.0 - ae/50.0; self.igain += (r - self.igain)*0.025
        else:
            r = 50.0/ae; self.igain += (r*self.igain - self.igain)*0.1
        self.igain = max(0.0, min(1.0, self.igain))
        if ae < 1920.0:
            r = 1.0 - ae/1920.0; self.kp_gain += (r - self.kp_gain)*self.rate
        else:
            r = 1920.0/ae; self.kp_gain += (r*self.kp_gain - self.kp_gain)*0.1
        self.kp_gain = max(0.0, min(1.0, self.kp_gain))
        ed = e - self.le
        tv = ed + self.lu * self.gain            # ★ V1.0.39 gain 换算
        px = self.vfx; pp = self.vfp + 0.01; k = pp/(pp+1.0)
        self.vfx = px + k*(tv - px); self.vfp = (1-k)*pp
        rvi = self.vfx
        if abs(e) < 1.0 and abs(ed) < 0.1:
            rvi = ed + self.lu * self.gain * 0.5  # ★ V1.0.39 近点分支
        kr = rvi
        if abs(kr) <= 0.5: kr = 0.0
        kr = (kr * self.predict) * self.igain
        px = self.ifx; pp = self.ifp + 0.5; k = pp/(pp+1.0)
        self.ifx = px + k*(kr - px); self.ifp = (1-k)*pp
        kr = self.ifx
        Kp = self.kp * e; Ki = kr; Kd = self.kd * ed
        if self.smooth:
            Kp = self.st(Kp, self.kb, self.kb - self.smooth)
            Ki = self.st(Ki, self.kb, self.kb - 1000.0)
            Kd = self.st(Kd, self.kb, self.kb - self.smooth)
        self.u = (Kp + Ki + Kd) * self.kp_gain
        self.lu = self.u; self.le = e
        return self.u

    def reset(self):
        self.kp_gain = 0.0; self.igain = 0.0; self.u = 0.0
        self.le = 0.0; self.lu = 0.0
        self.vfx = 0.0; self.vfp = 1.0; self.ifx = 0.0; self.ifp = 1.0


GAIN = 0.65
DZ = 1.0


def sim(kp, kd, predict, rate, smooth, delay_ms, dist, move_px_s, noise=2.0, dt=7.5, steps=900, seed=7):
    random.seed(seed)
    p = Pid1Gain(kp, kd, predict, rate, smooth, GAIN)
    err = float(dist)
    df = max(0, int(round(delay_ms/dt)))
    q = [0.0]*df
    es, os = [], []
    for i in range(steps):
        e = err + random.uniform(-noise, noise)
        u = p.upd(e)
        out = u * GAIN
        if abs(out) < DZ: out = 0.0
        q.append(out)
        applied = q.pop(0) if q else 0.0
        err -= applied * GAIN
        err -= move_px_s * (dt/1000.0)
        es.append(err); os.append(out)
    return es, os


def ana(es, os):
    te = es[-300:]; to = os[-300:]
    amp = max(abs(x) for x in te)
    fl = sum(1 for i in range(1, len(te)) if (te[i] > 0.3) != (te[i-1] > 0.3))
    nz = sum(1 for o in to if abs(o) >= 1)
    return amp, fl, nz


def main():
    delay = 51.0
    cfgs = [
        ("pid1原文 25/25/p3.0",    (25.0, 25.0, 3.0, 0.3)),
        ("标定写回 10.73/37.86/p0.14", (10.7296, 37.8584, 0.139, 0.3)),
        ("25/25/p1.0",             (25.0, 25.0, 1.0, 0.3)),
        ("25/25/p0.5",             (25.0, 25.0, 0.5, 0.3)),
        ("25/15/p1.0",             (25.0, 15.0, 1.0, 0.3)),
        ("标定kp/kd + p1.0",       (10.7296, 37.8584, 1.0, 0.3)),
        ("标定kp/kd + p0.5",       (10.7296, 37.8584, 0.5, 0.3)),
        ("10.73/15/p1.0",          (10.7296, 15.0, 1.0, 0.3)),
    ]
    for scene, dist, move in [("静态40px", 40.0, 0.0), ("移动50px/s", 40.0, 50.0)]:
        print(f"\n===== {scene}（delay={delay:.0f}ms gain={GAIN}）=====")
        for name, (kp, kd, pr, rt) in cfgs:
            es, os = sim(kp, kd, pr, rt, 9900.0, delay, dist, move)
            amp, fl, nz = ana(es, os)
            verdict = "!!振荡" if (fl >= 4 or amp > 3.0) else ("!!跟不上" if (move and abs(es[-1]) > 8.0) else "OK")
            print(f"  {name:<28} 幅={amp:6.2f}px 翻转={fl:3d} 非零={nz:3d}/300 末误差={es[-1]:6.2f}  {verdict}")


if __name__ == "__main__":
    main()
