"""_scan_cascade.py — yey 开源 cascade_pid 在 TTBOX 环境（gain=0.65, 51ms 延迟）的仿真验证。"""
import random


def clamp(v, lo, hi):
    return max(lo, min(hi, v))


class PID:
    def __init__(self, kp, ki, kd, maxI, maxOut):
        self.kp, self.ki, self.kd = kp, ki, kd
        self.maxI, self.maxOut = maxI, maxOut
        self.error, self.lastError, self.integral, self.output = 0.0, 0.0, 0.0, 0.0

    def calc(self, reference, feedback):
        self.lastError = self.error
        self.error = reference - feedback
        dout = (self.error - self.lastError) * self.kd
        pout = self.error * self.kp
        self.integral = clamp(self.integral + self.error * self.ki, -self.maxI, self.maxI)
        self.output = clamp(pout + self.integral + dout, -self.maxOut, self.maxOut)
        return self.output


class CascadeAxis:
    def __init__(self, outer, inner, aim_speed):
        self.outer = PID(*outer)
        self.inner = PID(*inner)
        self.aim_speed = aim_speed
        self.last_inner = 0.0

    def step(self, error):
        self.outer.calc(error, 0.0)
        self.inner.calc(self.outer.output, self.last_inner)
        self.last_inner = self.inner.output
        return self.inner.output * self.aim_speed

    def reset(self):
        self.outer = PID(self.outer.kp, self.outer.ki, self.outer.kd, self.outer.maxI, self.outer.maxOut)
        self.inner = PID(self.inner.kp, self.inner.ki, self.inner.kd, self.inner.maxI, self.inner.maxOut)
        self.last_inner = 0.0


GAIN = 0.65
DZ = 1.0


def sim(delay_ms, dist, move_px_s, noise=2.0, dt=7.5, steps=900, seed=7):
    random.seed(seed)
    cx = CascadeAxis((0.9, 0.05, 0.025, 50, 100), (0.7, 0.01, 0.025, 30, 50), 0.3)
    cy = CascadeAxis((0.9, 0.05, 0.025, 50, 100), (0.7, 0.01, 0.025, 30, 50), 0.1)
    err = float(dist)
    df = max(0, int(round(delay_ms / dt)))
    q = [0.0] * df
    es, os = [], []
    for _ in range(steps):
        e = err + random.uniform(-noise, noise)
        u = cx.step(e)
        out = u * GAIN
        if abs(out) < DZ:
            out = 0.0
        q.append(out)
        applied = q.pop(0) if q else 0.0
        err -= applied * GAIN
        err -= move_px_s * (dt / 1000.0)
        es.append(err)
        os.append(out)
    return es, os


def ana(es, os):
    te = es[-300:]
    amp = max(abs(x) for x in te)
    fl = sum(1 for i in range(1, len(te)) if (te[i] > 0.3) != (te[i - 1] > 0.3))
    nz = sum(1 for o in os[-300:] if abs(o) >= 1)
    return amp, fl, nz


def main():
    print("yey cascade_pid（outer 0.9/0.05/0.025, inner 0.7/0.01/0.025, aim_speed 0.3/0.1）")
    print(f"环境：gain={GAIN}, deadzone={DZ}count, 噪声±2px\n")
    for scene, dist, move in [("静态40px", 40.0, 0.0), ("移动50px/s", 40.0, 50.0)]:
        for delay in (30, 51):
            es, os = sim(delay, dist, move)
            amp, fl, nz = ana(es, os)
            v = "!!振荡" if (fl >= 4 or amp > 3.0) else ("!!跟不上" if (move and abs(es[-1]) > 8.0) else "OK")
            print(f"  {scene} delay={delay}ms  幅={amp:6.2f}px 翻转={fl:3d} 非零={nz:3d}/300 末误差={es[-1]:6.2f}  {v}")


if __name__ == "__main__":
    main()
