"""_scan_smooth.py — SmoothAimController 在 TTBOX 环境（gain=0.65, 51ms 延迟）的稳定区间扫描。"""
import random


def clamp(v, lo, hi):
    return max(lo, min(hi, v))


class SmoothAim:
    def __init__(self, alpha, gain, max_move, deadzone_ratio):
        self.alpha = alpha
        self.gain = gain
        self.max_move = max_move
        self.deadzone_ratio = deadzone_ratio
        self.last_error = 0.0

    def update(self, error, box_h):
        if abs(error) < 0.5:                      # 绝对死区（防 EMA 惯性）
            self.last_error = 0.0
            return 0.0
        if box_h > 0.0 and abs(error) < box_h * self.deadzone_ratio:  # 尺寸自适应死区
            self.last_error = 0.0
            return 0.0
        smoothed = self.last_error * (1.0 - self.alpha) + error * self.alpha
        self.last_error = smoothed
        move = clamp(smoothed * self.gain, -self.max_move, self.max_move)
        return move


GAIN = 0.65


def sim(alpha, aim_gain, max_move, dz_ratio, delay_ms, dist, move_px_s, box_h=100.0,
        noise=2.0, dt=7.5, steps=900, seed=7):
    random.seed(seed)
    c = SmoothAim(alpha, aim_gain, max_move, dz_ratio)
    err = float(dist)
    df = max(0, int(round(delay_ms / dt)))
    q = [0.0] * df
    es, os = [], []
    for _ in range(steps):
        e = err + random.uniform(-noise, noise)
        u = c.update(e, box_h)
        out = u * GAIN
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
    return amp, fl


def main():
    print("SmoothAimController（EMA+比例+限幅+框高死区），环境 gain=0.65 delay=51ms box_h=100 噪声±2px")
    print("死区 = max(0.5px, 框高100×ratio)")
    print("\n== aim_gain × aim_alpha 扫描（移动 50px/s，max_move=30，dz_ratio=0.3）==")
    for alpha in (0.3, 0.5, 0.8, 1.0):
        row = []
        for g in (0.05, 0.1, 0.15, 0.2, 0.3):
            es, os = sim(alpha, g, 30.0, 0.3, 51.0, 40.0, 50.0)
            amp, fl = ana(es, os)
            v = '振荡' if (fl >= 4 or amp > 4.0) else ('慢' if abs(es[-1]) > 10.0 else 'OK')
            row.append(f'g{g:<4}幅{amp:5.2f}翻{fl:2d}末{es[-1]:5.1f}{v}')
        print(f'  alpha={alpha}: ' + ' | '.join(row))
    print("\n== 死区比例扫描（gain=0.1, alpha=0.5, max_move=30）==")
    for dz in (0.0, 0.2, 0.3, 0.5):
        es, os = sim(0.5, 0.1, 30.0, dz, 51.0, 40.0, 50.0)
        amp, fl = ana(es, os)
        v = '振荡' if (fl >= 4 or amp > 4.0) else ('慢' if abs(es[-1]) > 10.0 else 'OK')
        print(f'  dz_ratio={dz}: 幅={amp:5.2f} 翻转={fl:2d} 末误差={es[-1]:5.1f} {v}')


if __name__ == "__main__":
    main()
