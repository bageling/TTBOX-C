"""capture_geometry.py — 采集裁剪尺寸与 FOV 归一（2026-10-02 web 换写法 S3）。

从 ``plugins/web/bin/ttbox-web.py`` 整块搬出。**行为逐字保留**。
"""
from __future__ import annotations


CROP_SIZE_FULL_FRAME = 0
CROP_SIZE_MIN_VALID = 64
CROP_SIZE_MAX_VALID = 3840


def normalize_capture_crop_size(value) -> int:
    """把面板的 crop_size 归一化为 Core 合法值（0=全帧，或 64~3840）。

    非数字 / None ⇒ 0（全帧 = 不裁剪）。**绝不产生 1~63**：那是合法域之外的"夹出来"的假值，
    正是它把整份配置送进 Core 的校验拒绝分支。
    """
    try:
        v = int(round(float(value)))
    except (TypeError, ValueError):
        return CROP_SIZE_FULL_FRAME
    if v <= CROP_SIZE_FULL_FRAME:
        return CROP_SIZE_FULL_FRAME
    if v < CROP_SIZE_MIN_VALID:
        return CROP_SIZE_MIN_VALID
    if v > CROP_SIZE_MAX_VALID:
        return CROP_SIZE_MAX_VALID
    return v


def normalize_profile_capture_size(prof: dict) -> dict:
    """归一化 profile.capture.width/height（就地把非法值拉回 Core 合法域）。

    为什么在**合并之后**再归一一次：profile 可能来自三条路——面板全量提交、预设文件（可能是
    旧版 RuntimeProfile 结构）、Core 当前运行配置。任何一条路上残留 1~63 的值都会让整份
    SET_CONFIG 被 Core 拒收（连带其它本来合法的字段一起丢）。这里做最后一道闸。
    """
    if not isinstance(prof, dict):
        return prof
    cap = prof.get('capture')
    if isinstance(cap, dict):
        for key in ('width', 'height'):
            if key in cap or (key == 'width' and cap.get('width') is None):
                cap[key] = normalize_capture_crop_size(cap.get(key))
    return prof


# 瞄准半径倍率（总览「FOV 半径」/ 热键卡「热键 FOV 缩放」）的合法区间与夹取。
# 为什么必须有下限：倍率 0 会让 core 的 fov.radius 撞上「FOV 半径必须在 (0,1]」校验
# （core/src/model/RuntimeProfile.cpp:141）⇒ 整个保存失败；半径 0 本身也等于选靶全灭。
# 取 0.1 ⇒ 半径 = 0.05 × 内接圆（板端 640 截取 ⇒ 32px），仍可用且合法。
FOV_FACTOR_MIN = 0.1

# V1.0.12（2026-09-30）：ZOOM_SCALE_MIN/MAX 与 GAIN_PX_PER_COUNT_MAX 已删。
# 业主口径「不区分倍镜，靠压枪和自瞄把准星拉回目标身上」⇒ 本档不再有"倍镜真实倍率"，
# 也没有"本档 px/count"。旧配置里出现这两个键由 core 侧静默忽略。


def _fov_factor_clamp(v, default=1.0) -> float:
    """把倍率夹到 [FOV_FACTOR_MIN, 1.0]；非数值 / NaN 回退 default。"""
    try:
        f = float(v)
    except (TypeError, ValueError):
        return default
    if f != f:  # NaN
        return default
    return max(FOV_FACTOR_MIN, min(1.0, f))


def _fov_radius_to_factor(radius, enabled=True) -> float:
    """core 的 fov.radius → 面板倍率。

    core 侧 fov_range = fov.radius × 2（AimThread.cpp:117），所以倍率 = radius × 2；
    enabled=False 时 core 强制 fov_range=1.0 ⇒ 倍率就是 1.0（= 内接圆）。
    """
    if not enabled:
        return 1.0
    try:
        return _fov_factor_clamp(float(radius) * 2.0)
    except (TypeError, ValueError):
        return 1.0


# ---- 瞄准档位（多热键，2026-09-24）----
# 面板「热键与类别」页每张卡片 = 一个档位，提交体是 aim_profiles[] 数组。
# core 侧 MouseProfile.aim_profiles 是热键的**唯一真源**：老的平铺
# aim_hotkey / aim_hotkey2 / aim_hotkey_mode 已从结构体删除，只在 JSON 解析时
# 作为「数组缺失（老配置）」的合成源。所以这里必须整表遍历，不能再只取 [0]。
