# 参数扫描：d_cutoff × beta，双目标（匀速命中 <2px + 噪声 step 衰减 <0.3）
import math

DT = 4e-3          # test_tracker kDtUs=4000us
V = 100.0          # 100px/s 匀速
PRED_S = 0.05
PRED_FRAMES = 12

def alpha(cutoff, dt):
    return 1.0 / (1.0 + 1.0 / (2 * math.pi * cutoff * dt))

def one_euro_run(xs, dt, mc, beta, dc):
    fx = xs[0]; fdx = 0.0; out = [fx]
    for x in xs[1:]:
        raw_dx = (x - fx) / dt
        ad = alpha(dc, dt)
        fdx += ad * (raw_dx - fdx)
        cutoff = mc + beta * abs(fdx)
        ax = alpha(cutoff, dt)
        fx += ax * (x - fx)
        out.append(fx)
    return out

# 场景1：匀速 100px/s，200 帧（起始于 ref）
xs = [400.0 + i * V * DT for i in range(200)]
# 场景2：真实 y1 噪声序列（dt=7.246ms 但此处用 4ms 近似不影响相对结论）
y1 = [723.7, 715.4, 712.1, 716.0, 714.6, 719.3, 717.9, 716.8, 729.1, 718.8,
      716.0, 730.3, 719.0, 718.7, 727.3, 712.7, 718.3, 714.0, 723.6, 719.4,
      717.1, 715.9, 713.4, 719.4, 715.5, 725.7, 713.4, 717.0, 716.2, 716.9]

print(f"{'dc':>4} {'beta':>5} | {'hit_err':>8} {'max_step':>8} {'ratio':>6}")
for dc in [0.5, 1.0, 1.5, 2.0, 2.5, 3.0]:
    for beta in [0.05, 0.1, 0.15, 0.2, 0.3]:
        sm = one_euro_run(xs, DT, 0.8, beta, dc)
        hit = 0.0; n = 0
        for i in range(len(xs) - PRED_FRAMES):
            hit += abs(sm[i] - xs[i + PRED_FRAMES]); n += 1
        hit /= n
        sm2 = one_euro_run(y1, 1/138, 0.8, beta, dc)
        mx = max(abs(sm2[i]-sm2[i-1]) for i in range(1, len(sm2)))
        raw_mx = max(abs(y1[i]-y1[i-1]) for i in range(1, len(y1)))
        flag = ' <<<' if (hit < 2.0 and mx/raw_mx < 0.30) else ''
        print(f"{dc:>4.1f} {beta:>5.2f} | {hit:8.2f} {mx:8.2f} {mx/raw_mx:6.2f}{flag}")
