"""scene3d.py — 3D 游戏场景生成（V1.0.46）

★ 为什么必须有这个（业主 2026-10-06 指出：三角洲是 3D 游戏）
  之前所有仿真都是「目标在固定深度的平面上左右移动、框大小写死 160px」。
  但 3D FPS 里目标带着**深度 Z**：
      屏幕投影   sx = f·X/Z,  sy = f·Y/Z      （透视）
      检测框高   h  = H_real / Z              （∝ 1/Z）
      落点       ty = y1 + off·h              （框内固定比例 ⇒ 跟着 h 缩放）
  于是「目标冲向玩家」（Z 从 50m 掉到 5m）时：
      框高放大 10 倍 ⇒ 落点外扩 10 倍距离 ⇒ 控制器几帧内必须跟上这个跳变。
  这是与「平面横移」**完全不同的动力学**，而它恰恰是实战里「追着怪/停不住」
  的一大来源。平面仿真结构上测不到它。

对照 BB927 实战：它把真实检测序列当输入（有真实的框暴涨暴跌），所以能扛。
我们的做法 = 用透视投影**生成**这种序列，让仿真至少在动力学上接近真实。

物理参数取值（按三角洲这类战术 FPS 的常见量级，取中间值）
--------------------------------------------------------------
  视场角垂直 60°  ⇒ 焦距 f = (capture_h/2) / tan(30°) ≈ 0.866 × capture_h
  人体身高     1.75 m（框高对应这个）
  交战距离     5 ~ 60 m
  横移速度     3 ~ 7 m/s（跑动）
  深度速度     ±4 m/s（前后跑动/进退）
  捕获分辨率   640×640 居中（与板端一致，capture 640×640 裁剪区）
"""
from __future__ import annotations

import math
import random
from dataclasses import dataclass, field

import trace_replay as T


@dataclass
class Proj:
    """透视投影器：世界坐标 → 屏幕像素 + 检测框。"""
    capture_h: float = 640.0
    fov_v_deg: float = 60.0
    body_h_m: float = 1.75

    @property
    def f(self) -> float:
        """焦距（px）：f = (h/2)/tan(fov/2)。"""
        return (self.capture_h * 0.5) / math.tan(math.radians(self.fov_v_deg) * 0.5)

    def project(self, wx: float, wy: float, wz: float) -> tuple[float, float, float, float]:
        """世界坐标 → (屏幕中心x, 屏幕中心y, 框高px, 深度m)。

        约定：wy 向上为正，屏幕 y 向下 ⇒ 屏幕 y = center - f·wy/wz。
        深度 wz 必须 > 0（相机在原点，目标在 +Z 前方）。
        """
        z = max(0.5, wz)
        sx = self.f * wx / z
        sy = -self.f * wy / z          # 右手系：wy 向上 ⇒ 屏幕 y 取负
        h_px = self.f * self.body_h_m / z
        return sx, sy, h_px, z

    def box_w_from_h(self, h_px: float, w_h_ratio: float = 0.32) -> float:
        """人体框宽高比（板端实测 0.32~0.52 漂，取 0.32 偏保守）。"""
        return h_px * w_h_ratio


def gen_scene3d(
    name: str,
    mode: str,                 # strafe | approach | retreat | zigzag | mixed
    seconds: float = 8.0,
    seed: int = 7,
    proj: Proj | None = None,
    noise_px: float = 2.0,
) -> T.Trace:
    """生成一段 3D 场景轨迹（输出仍是 T.Trace，target_x = 屏幕像素）。

    mode 决定深度与横向的运动形态 —— 每种对应实战里一类手感问题：
      strafe   横向平移        → 基础跟随（等效 2D 场景，但框大小随深度微变）
      approach 冲向玩家(Z↓)   → 框暴涨、落点外扩，最考验「停不住」
      retreat  远离玩家(Z↑)   → 框缩tiny、落点内收，考验「够不着」
      zigzag   斜向折返        → 方向突变，最考验「追着怪」（翻转次数）
      mixed    以上混合 + 深度抖动 → 接近实战
    """
    p = proj or Proj()
    rnd = random.Random(seed)
    frames_n = int(seconds * 1000.0 / T.FRAME_MS_144FPS)
    dt = T.FRAME_MS_144FPS / 1000.0

    # 初始世界位置：正前方 x 偏一点，深度 25m
    wx = rnd.uniform(-1.2, 1.2)
    wy = 0.0
    wz = 25.0
    vx = vy = vz = 0.0

    tr = T.Trace(name=name, source=f"scene3d:{mode}")
    for i in range(frames_n):
        # ── 目标运动（世界坐标，单位 m / m·s⁻¹）──
        if mode == "strafe":
            vx = 5.0 * math.sin(i * 0.012)
            vz = 0.0
        elif mode == "approach":
            vx = rnd.uniform(-0.8, 0.8)
            vz = -4.5                    # 冲向玩家
        elif mode == "retreat":
            vx = rnd.uniform(-0.8, 0.8)
            vz = +4.0                    # 远离玩家
        elif mode == "zigzag":
            vx = 6.0 * (1.0 if (i // 45) % 2 == 0 else -1.0)   # 每 45 帧折返
            vz = rnd.uniform(-1.0, 1.0)
        else:  # mixed
            vx = 4.5 * math.sin(i * 0.009) + rnd.uniform(-1.2, 1.2)
            vz = rnd.uniform(-3.0, 3.0)
        vy = rnd.uniform(-0.4, 0.4)

        wx += vx * dt
        wy += vy * dt
        wz = max(4.0, min(60.0, wz + vz * dt))   # 夹在 4~60m（太近/太远都不是常态）

        # ── 投影 → 屏幕 ──
        sx, sy, h_px, _ = p.project(wx, wy, wz)
        w_px = p.box_w_from_h(h_px)

        # 捕获区裁剪：板端 capture 是 640×640 居中，目标出框就不该被检测到
        in_view = (abs(sx) < p.capture_h * 0.5 - w_px * 0.5
                   and abs(sy) < p.capture_h * 0.5 - h_px * 0.5)
        # 落点：板端 ty = y1 + 0.15h（offset_y），y1 = 框顶 = 中心 - h/2
        aim_y = (sy - h_px * 0.5) + 0.15 * h_px

        tr.frames.append(T.Frame(
            t_ms=i * T.FRAME_MS_144FPS,
            box_w=w_px, box_h=h_px,
            # ★ 帧的 target_x 语义 = 目标**瞄准落点**在屏幕上的位置（准星在原点）。
            #   回放里准星从 0 出发 ⇒ 初始误差就是落点坐标。
            target_x=sx + rnd.uniform(-noise_px, noise_px),
            target_y=aim_y + rnd.uniform(-noise_px, noise_px),
            has_target=1 if in_view else 0,   # 出框 = 丢失（回放要能重现断跟）
        ))
    return tr


def scene3d_catalog() -> list[tuple[str, str]]:
    """场景目录（name, mode）—— 覆盖实战里各类手感问题。"""
    return [
        ("3D横移", "strafe"),
        ("3D冲向", "approach"),   # 框暴涨 ⇒ 考「停不住」
        ("3D远离", "retreat"),    # 框缩小 ⇒ 考「够不着」
        ("3D折返", "zigzag"),    # 方向突变 ⇒ 考「追着怪」
        ("3D混合", "mixed"),
    ]
