# 复现 C++ 完整逻辑：位置 OneEuro + vx 原始帧差 EMA(0.4) + predict=fx+vx*pred_s
import math

DT = 4e-3
V = 100.0
PRED_S = 0.05
PRED_FRAMES = 12
VEL_A = 0.4

def alpha(cutoff, dt):
    return 1.0 / (1.0 + 1.0 / (2 * math.pi * cutoff * dt))

def run(xs, dt, mc, beta, dc):
    fx = xs[0]; fdx = 0.0; vx = 0.0; prev_raw = xs[0]
    preds = []
    for x in xs[1:]:
        raw_dx = (x - prev_raw) / dt          # 原始帧差速度（C++ 语义）
        vx += VEL_A * (raw_dx - vx)           # EMA(0.4) + clamp 2500
        prev_raw = x
        rdx = (x - fx) / dt                    # 平滑器原始导数
        ad = alpha(dc, dt)
        fdx += ad * (rdx - fdx)
        cutoff = mc + beta * abs(fdx)
        ax = alpha(cutoff, dt)
        fx += ax * (x - fx)
        preds.append(fx + vx * PRED_S)
    return preds

xs = [400.0 + i * V * DT for i in range(200)]
print("dc beta | hit_err")
for dc in [0.5, 1.0, 1.5, 2.0]:
    for beta in [0.05, 0.1, 0.15, 0.2]:
        pr = run(xs, DT, 0.8, beta, dc)
        hit = sum(abs(pr[i] - xs[i + PRED_FRAMES]) for i in range(len(xs) - PRED_FRAMES)) / (len(xs) - PRED_FRAMES)
        flag = ' <<<' if hit < 2.0 else ''
        print(f"{dc:>3.1f} {beta:>5.2f} | {hit:6.2f}{flag}")
