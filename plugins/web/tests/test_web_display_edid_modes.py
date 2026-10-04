# -*- coding: utf-8 -*-
"""显示器 EDID 模式解析的行为锁定测试（2026-10-02 S10-c 随拆分补写）。

★ 为什么必须有这个测试：
  `get_display_hardware` 拆成 probe 之前，`advertised` 与 `available_modes`
  是**两份几乎逐行相同的代码**，唯一差别是空行处一个用 `break`、
  一个用 `in_modes = False` —— 看起来像行为差异，其实**两者等价**
  （都是「遇空行结束模式区」）。
  拆分时把它们合并成一个 `_probe_edid_modes()`。这个判断必须被测试钉住，
  否则以后有人「统一」成其中一种，会静默改变另一个的产出。

★ 为什么不能用真硬件测：v4l2-ctl / hdmirx_edid 都要板子上才有，
  所以这里只锁**纯解析逻辑** —— 把 subprocess 换成注入的假输出。

★ 本测试不依赖入口模块（直接 import lib 的 api.hardware），
  `_display_mode_entry` 走 hub 取，测试里自己补一个等价替身。
"""
import importlib.util
import re
import sys
from pathlib import Path

import pytest

# ★ 根锚用 paths.discover_root（门禁⑩ 禁parents[N]）：从本文件逐级向上找
#   同时含全部 _ROOT_ANCHORS 的目录，找不到就炸—— 不静默指错根。
from plugins.web.lib.paths import discover_root

REPO = Path(discover_root(__file__))
if str(REPO) not in sys.path:
    sys.path.insert(0, str(REPO))


@pytest.fixture(scope='module')
def hw():
    """载入 api.hardware，并给它一个不依赖入口的 _display_mode_entry。"""
    import plugins.web.api.hardware as H

    # ★ 替身形状必须与真实实现一致（2026-10-02 实测）：
    #   {'token','label','width','height','refresh','pixel_clock_khz',
    #    'source','hdmi_raw_gbps'}
    # 少字段会让「拆后 vs 原码」的比较失真（原码返回的是 tuple，
    # _norm 只取前6 个，所以替身多两个字段也无妨 —— 但少字段会 KeyError）。
    def entry(token, label, w, h, refresh, pc):
        gbps = round(pc / 1_000_000.0 * 30 / 1000.0, 3) if pc else 0
        return {'token': token, 'label': label, 'width': w, 'height': h,
                'refresh': refresh, 'pixel_clock_khz': pc,
                'source': 'safe', 'hdmi_raw_gbps': gbps}
    H._display_mode_entry = entry
    return H


def _inject(hw, edid_text):
    """把 subprocess 换成返回固定 EDID 文本，跑 _probe_edid_modes。

    ★ 注意：Fake.check_output 的形参也叫 `text`（subprocess 的真实签名），
      若直接引用闭包里的 edid_text 会取到 None —— 所以先改名再传。
    """
    real = hw.subprocess

    class Fake:
        @staticmethod
        def check_output(argv, text=None, timeout=None):
            return edid_text
    hw.subprocess = Fake
    try:
        return hw._probe_edid_modes()
    finally:
        hw.subprocess = real


# ★ 基准：拆分前那两份原始代码（逐行照抄，不是重写）
def _orig_advertised(text):
    out = []
    in_modes = False
    for lm in text.splitlines():
        lm = lm.strip()
        if lm.startswith('Modes:'):
            in_modes = True
            continue
        if in_modes:
            if not lm:
                break
            parts = lm.split()
            if not parts:
                continue
            token = parts[0]
            dims = re.search(r'(\d+)x(\d+)@(\d+)', lm)
            pc = re.search(r'pixel_clock=(\d+)', lm)
            out.append((token,
                        '%sx%s@%s' % (dims.group(1), dims.group(2),
                                      dims.group(3)) if dims else token,
                        int(dims.group(1)) if dims else 0,
                        int(dims.group(2)) if dims else 0,
                        int(dims.group(3)) if dims else 0,
                        int(pc.group(1)) if pc else 0))
    return out


def _orig_available(text):
    out = []
    in_modes = False
    for lm in text.splitlines():
        lm = lm.strip()
        if lm.startswith('Modes:'):
            in_modes = True
            continue
        if in_modes:
            if not lm:
                in_modes = False
                continue
            parts = lm.split()
            if not parts:
                continue
            token = parts[0]
            dims = re.search(r'(\d+)x(\d+)@(\d+)', lm)
            pc = re.search(r'pixel_clock=(\d+)', lm)
            out.append((token,
                        '%sx%s@%s' % (dims.group(1), dims.group(2),
                                      dims.group(3)) if dims else token,
                        int(dims.group(1)) if dims else 0,
                        int(dims.group(2)) if dims else 0,
                        int(dims.group(3)) if dims else 0,
                        int(pc.group(1)) if pc else 0))
    return out


def _norm(rows):
    return [(x.get('token'), x.get('label'), x.get('width'), x.get('height'),
             x.get('refresh'), x.get('pixel_clock_khz'))
            if isinstance(x, dict) else tuple(x) for x in rows]


CASES = {
    'normal_tail_after_blank':
        'Monitor Model:\n  Vendor: ACME\nModes:\n'
        '  1920x1080@60\n  1280x720@60\n\n  2560x1440@60\nPreferred: x\n',
    'modes_end_with_blank': 'Modes:\n  1920x1080@60\n  1280x720@60\n\n',
    'no_blank_at_all': 'Modes:\n  1920x1080@60\n  1280x720@60\n  2560x1440@60\n',
    'no_modes_section': 'Monitor Model:\n  Vendor: ACME\n',
    'all_blank_after_modes': 'Modes:\n\n\n  1920x1080@60\n',
    'pixel_clock_parsed':
        'Modes:\n  1920x1080@60 pixel_clock=148500\n',
}


@pytest.mark.parametrize('name', sorted(CASES))
def test_edid_modes_matches_original(hw, name):
    """拆后的统一函数与拆分前两份原代码输出一致。"""
    edid = CASES[name]
    assert _norm(_orig_advertised(edid)) == _norm(_orig_available(edid)), \
        '★ 前提不成立：原始两份代码不等价，不能合并'
    assert _norm(_inject(hw, edid)) == _norm(_orig_advertised(edid))


def test_pixel_clock_becomes_khz(hw):
    """pixel_clock=148500 ⇒ pixel_clock_khz=148500（不是除以 1000）。"""
    got = _inject(hw, CASES['pixel_clock_parsed'])
    assert got[0]['pixel_clock_khz'] == 148500
    assert got[0]['width'] == 1920
    assert got[0]['refresh'] == 60


def test_blank_line_ends_modes_section(hw):
    """空行之后即使还有像模式的东西也不收 —— 这是两份原代码共同的行为。"""
    edid = 'Modes:\n  1920x1080@60\n\n  2560x1440@60\n  3840x2160@30\n'
    assert [m['token'] for m in _inject(hw, edid)] == ['1920x1080@60']
