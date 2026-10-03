"""hotkeys.py — 热键位与档位映射（2026-10-02 web 换写法 S3）。

从 ``plugins/web/bin/ttbox-web.py`` 整块搬出。**行为逐字保留**。

Web 面用名字（'left'/'right'/...），core 面用位掩码（1/2/4/...）。本模块是两者之间
唯一的换算点。
"""
from __future__ import annotations


HOTKEY_BITS = {'left': 1, 'right': 2, 'middle': 4, 'back': 8, 'forward': 16}
BIT_HOTKEYS = {v: k for k, v in HOTKEY_BITS.items()}


def _hotkey_to_bits(v, default=0):
    """Web 热键字符串（'left'/'right'/''）→ 位掩码。"""
    if isinstance(v, str):
        return HOTKEY_BITS.get(v.strip().lower(), default)
    if isinstance(v, (int, float)):
        return int(v)
    return default


# 合法的热键位掩码集合（左1 右2 中4 侧8 侧16）。
# ★ 2026-09-25：只判「hk == 0」是不够的 —— -1 / 32 / 255 都会放行，落到 core 的
#   static_cast<uint8_t> 上会绕回 / 越界（如 -1 ⇒ 255），命中判据 `buttons & 255 != 0`
#   对**任意**物理键成立 ⇒ 按什么键都瞄准（热键闸门 fail-open）。
#   组合掩码（如 3 = 左|右）core 侧能用，但面板无法回填（_bits_to_hotkey(3) 返回 ''，
#   会被 `or 'right'` 静默显示成右键，下次保存真变成 2 ⇒ 静默漂移）⇒ 面板这一层拒掉。
AIM_PROFILE_VALID_BITS = frozenset(HOTKEY_BITS.values())


def _bits_to_hotkey(v):
    """位掩码 → Web 热键字符串（0 → ''）。"""
    try:
        return BIT_HOTKEYS.get(int(v), '')
    except (TypeError, ValueError):
        return ''


def _hotkey_guard_to_web(hg) -> dict:
    """Core 的 mouse.hotkey_guard → 前端控件值。

    缺字段时给的是**与 Core 结构体一致的默认值**（enabled=False / middle），
    不是另立一套。挂起状态本身不在这里——它是运行期状态，走 state.aim.hotkeys_suspended。
    """
    hg = hg if isinstance(hg, dict) else {}
    return {
        'enabled': bool(hg.get('enabled', False)),
        'toggle_hotkey': _bits_to_hotkey(hg.get('toggle_hotkey', 4)) or 'middle',
    }


# controller 内的数值/布尔直通字段（Web key → mouse key）
