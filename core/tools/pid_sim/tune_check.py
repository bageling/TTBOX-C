"""tune_check.py — derive_pid_params 全场景稳定性验证（紧凑版）"""
import sys
import random
from pathlib import Path
# 根锚发现（A-PATH-3 / P6）：向上找同时含 plugins/framework/scripts/deploy 的目录，
# 不写死层级（原 `_HERE.parents[2]`：换一次布局就静默指错根）。
_HERE = Path(__file__).resolve().parent          # core/tools/pid_sim
_ROOT = _HERE
while _ROOT != _ROOT.parent and not all(
        (_ROOT / _n).is_dir() for _n in ("plugins", "framework", "scripts", "deploy")):
    _ROOT = _ROOT.parent
if str(_HERE) not in sys.path:
    sys.path.append(str(_HERE))                    # pid1.py 与本文件同目录（append 防遮蔽 stdlib）
if str(_ROOT) not in sys.path:
    sys.path.append(str(_ROOT))                    # ttbox_motion 领域包
from pid1 import Pid1
from ttbox_motion.calibration import derive_pid_params

DZ = 1.0
STEPS = 900


def sim(kp, kd, predict, delay, gain, dist=40.0, noise=2.0, seed=11):
    random.seed(seed)
    # ★ V1.0.13：kp/kd 已是生效值 ⇒ 仿真里 smooth 必须传 0（传 9900 等于再削 99%）
    p = Pid1(kp, kd, predict, 0.2, 0)
    err = float(dist)
    es = []
    os = []
    df = max(0, int(round(delay / 7.5)))
    q = [0.0] * df
    for _ in range(STEPS):
        e = err + random.uniform(-noise, noise)
        u = p.upd(e)
        out = u * gain
        if abs(out) < DZ:
            out = 0.0
        q.append(out)
        applied = q.pop(0)
        err -= applied * gain
        es.append(err)
        os.append(out)
    return es, os


def ana(es, os):
    te = es[-250:]
    to = os[-250:]
    amp = max(abs(x) for x in te)
    fl = sum(1 for i in range(1, len(te)) if (te[i] > 0.3) != (te[i - 1] > 0.3))
    nz = sum(1 for o in to if abs(o) >= 1)
    conv = next((i for i, x in enumerate(es) if abs(x) < 5.0), STEPS)
    return amp, fl, nz, conv


def run_row(label, kp, kd, predict, delay, gain):
    es, os = sim(kp, kd, predict, delay, gain)
    amp, fl, nz, conv = ana(es, os)
    v = "OK"
    if fl >= 4 or amp > 3.0:
        v = "!!振荡"
    elif conv > 300:
        v = "!!慢"
    print(f"{label:<38} kp={kp:6.3f} kd={kd:6.3f} p={predict:4.2f} "
          f"d={delay:3d} g={gain:4.2f} | 幅={amp:6.2f} 翻转={fl:3d} "
          f"非零={nz:3d} 收敛={conv:3d}  {v}")


def main():
    import sys as _s
    mode = _s.argv[1] if len(_s.argv) > 1 else "base"
    print(f"== 因子实验: {mode} ==")
    for gain in (0.65, 1.0, 1.5):
        for delay in (30, 60):
            d = derive_pid_params(gain, gain, delay)
            kp, kd, predict = d["kp"], d["kd"], d["predict"]
            if mode == "kd_boost":
                kd = max(0.08, min(0.6, kp * (1.2 + delay / 80.0)))
            elif mode == "predict_low":
                predict = max(0.1, min(0.4, 0.4 - delay / 300.0))
            elif mode == "kd_boost_predict_low":
                kd = max(0.08, min(0.6, kp * (1.2 + delay / 80.0)))
                predict = max(0.1, min(0.4, 0.4 - delay / 300.0))
            run_row(mode, kp, kd, predict, delay, gain)


if __name__ == "__main__":
    main()
