#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""edid_golden_gen.py — 生成 EDID 黄金样本（C++ 移植的**逐字节对拍基准**）。

为什么要它
    V1.0.53 把 EDID 从 Python 移植到 C++（业主令「去掉所有 Python 代码」）。
    「移植是否等价」不能靠眼看 —— 必须以**现役 Python 实现**的输出当唯一标准答案。
    本脚本必须在删 Python **之前**运行，把结果冻结成 JSON，供 C++ 单测逐字节对拍。

覆盖六类（全部为纯函数 / 纯逻辑，不碰文件系统与硬件）：
    A. mode_info(token)                    内置名 / 动态 WxH@Hz / 各类非法输入
    B. reduced_blanking_pixel_clock_khz()  CVT-RB 公式（含退化输入）
    C. lookup_timing(token)                内置表 + "WxH@R" 反查
    D. build_from_config(cfg) -> 256 字节   ★核心，逐字节 + sha256 + verify 结论
    E. edid_apply.sh::PYEOF 的 native_mode 保护逻辑（空/auto/非法 ⇒ profile 首选）
    F. PnP 编解码 + 配置字段归一（大小写/截断/非法兜底）

用法
    <python> tools/edid_golden_gen.py [out.json]
    缺省输出 core/tests/fixtures/edid_golden.json
"""
from __future__ import annotations

import hashlib
import itertools
import json
import os
import sys

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(REPO, "scripts"))

from edid.builder import EdidBuilder, build_from_config, _pnp_encode, _pnp_decode  # noqa: E402
from edid.timing_db import (  # noqa: E402
    TIMING_MAP, SAFE_MODES, mode_info, lookup_timing,
    reduced_blanking_pixel_clock_khz, pixel_clock_fits_hdmi_rx, is_dynamic_mode_token,
)
from edid.validator import validate_edid, verify_edid  # noqa: E402
from edid.mode_builder import (  # noqa: E402
    PROFILES_SET, NATIVE_MODES_SET, MAX_ADVERTISED_MODES,
    _safe_ascii, _hex_text, _bool_value,
)

DOC = {
    "generated_from": "scripts/edid/*.py（现役 Python 实现）",
    "purpose": "C++ 移植的逐字节对拍基准；不得在删 Python 前丢失本文件",
    "note": "hex 为 256 字节连续小写十六进制（512 字符）；sha256 对**字节**取",
}


def _hexof(b: bytes) -> str:
    return b.hex()


def gen_mode_info():
    """A. mode_info(token) 的输入输出。"""
    tokens = [
        # 内置（TIMING_MAP）
        "1080p60", "1080p60compat", "1080p90", "1080p120", "1080p120compat",
        "1080p144", "1080p240", "1080p240compat",
        "1440p60", "1440p120", "1440p144", "1440p165", "2160p60",
        # 动态
        "2560x1440@165", "1920x1080@200", "1280x720@60", "3840x2160@60",
        " 2560x1440@144 ",   # 带空格（mode_info 不 strip，按 lambda 原样比对）
        # 非法 / 边界
        "", "auto", "abc", "1920x1080", "1920x1080@", "@60",
        "100x100@60",        # 宽高过小
        "5000x2000@60",      # 宽高超上限
        "1920x1080@20",      # 刷新率过低
        "1920x1080@400",     # 刷新率过高
        "3840x2160@240",     # 像素时钟超 600MHz
        "1080P60",           # 大小写敏感 ⇒ 非法
        None,
    ]
    out = []
    for t in tokens:
        out.append({"token": t, "result": mode_info(t)})
    return out


def gen_pixel_clock():
    """B. CVT-RB 像素时钟公式。"""
    pairs = [
        [1920, 1080, 60], [1920, 1080, 120], [1920, 1080, 144], [1920, 1080, 240],
        [2560, 1440, 60], [2560, 1440, 120], [2560, 1440, 144], [2560, 1440, 165],
        [3840, 2160, 60], [1280, 720, 60], [640, 400, 24], [4095, 4095, 360],
        [0, 0, 0], [1920, 1080, 0], [0, 1080, 60],   # 退化
    ]
    return [{"w": w, "h": h, "r": r,
             "khz": reduced_blanking_pixel_clock_khz(w, h, r),
             "fits": pixel_clock_fits_hdmi_rx(reduced_blanking_pixel_clock_khz(w, h, r))}
            for w, h, r in pairs]


def gen_lookup():
    """C. lookup_timing(token)。"""
    ok_tokens = list(TIMING_MAP.keys()) + ["2560x1440@144", "1920x1080@240", "3840x2160@60"]
    bad_tokens = ["", "nope", "1920x1080@61", "2560x1440@164"]
    out = []
    for t in ok_tokens:
        try:
            tm = lookup_timing(t)
            out.append({"token": t, "ok": True, "timing": {
                "w": tm.width, "h": tm.height, "refresh": tm.refresh,
                "pc": tm.pixel_clock, "hfp": tm.h_front_porch, "hs": tm.h_sync,
                "hbp": tm.h_back_porch, "vfp": tm.v_front_porch, "vs": tm.v_sync,
                "vbp": tm.v_back_porch, "interlaced": tm.interlaced,
                "h_pol": tm.h_pol, "v_pol": tm.v_pol,
                "pc_10khz": tm.pixel_clock_10khz, "h_blank": tm.h_blank,
                "v_blank": tm.v_blank, "h_total": tm.h_total, "v_total": tm.v_total,
                "verify_error": tm.verify(),
            }})
        except KeyError:
            out.append({"token": t, "ok": False, "timing": None})
    for t in bad_tokens:
        try:
            lookup_timing(t)
            out.append({"token": t, "ok": True, "timing": None})
        except KeyError:
            out.append({"token": t, "ok": False, "timing": None})
    return out


def _configs():
    """D. 待构建的配置组合（含出厂配置与随机身份样本）。"""
    base = {
        "device": "auto", "name": "TTBox-COMPAT", "vendor": "AIB",
        "product_id": "0x2400", "serial": "0xA1B00001",
        "native_mode": "1080p240", "native_only": True, "profile": "boot-safe-full",
    }
    cfgs = [dict(base)]
    # 各内置模式 + native_only 两种取值
    for mode in list(TIMING_MAP.keys()) + ["2560x1440@165", "1920x1080@200"]:
        for only in (True, False):
            c = dict(base)
            c["native_mode"] = mode
            c["native_only"] = only
            cfgs.append(c)
    # 随机身份样本（前端 randomizeDisplayHardware 造出来的形态）
    for vendor, name, pid, ser in [
        ("KQP", "KQP-3A7F21", "0x1B2C", "0x7F3A91C4"),
        ("ZZT", "ZZT-1F0D45", "0xFFFE", "0x00000001"),
        ("AAA", "AAA-000001", "0x0001", "0xFFFFFFFF"),
    ]:
        c = dict(base)
        c.update({"vendor": vendor, "name": name, "product_id": pid, "serial": ser,
                  "native_mode": "1440p144", "native_only": False})
        cfgs.append(c)
    # added_modes
    c = dict(base); c["native_mode"] = "1080p60"; c["native_only"] = False
    c["added_modes"] = ["1440p144", "2160p60"]; cfgs.append(c)
    c = dict(base); c["native_mode"] = "1440p60"; c["native_only"] = False
    c["added_modes"] = ["1080p240", "1080p60", "1080p240"]; cfgs.append(c)  # 含重复
    # 字段边界：小写 vendor / 超长 name / 非法 hex
    c = dict(base); c.update({"vendor": "opi", "name": "X" * 20,
                              "product_id": "0xFFFF", "serial": "0xFFFFFFFF"}); cfgs.append(c)
    c = dict(base); c.update({"vendor": "ttbox", "name": "", "product_id": "zzz", "serial": ""}); cfgs.append(c)
    # 出厂兼容身份（deploy/config/hardware_display.json 原样）
    cfgs.append({"device": "auto", "name": "TTBox-COMPAT", "vendor": "AIB",
                 "product_id": "0x2400", "serial": "0xA1B00001",
                 "native_mode": "1080p240", "native_only": True, "profile": "boot-safe-full"})
    return cfgs


def gen_build():
    """D. build_from_config → 256 字节（核心对拍项）。"""
    out = []
    for cfg in _configs():
        try:
            edid = build_from_config(cfg)
        except Exception as exc:  # noqa: BLE001
            out.append({"config": cfg, "ok": False, "error": str(exc)})
            continue
        ok, errs = verify_edid(edid)
        out.append({
            "config": cfg,
            "ok": True,
            "len": len(edid),
            "hex": _hexof(edid),
            "sha256": hashlib.sha256(edid).hexdigest(),
            "verify_ok": ok,
            "verify_errors": errs,
            "validate_errors": validate_edid(edid),
        })
    return out


def gen_native_mode_guard():
    """E. edid_apply.sh::PYEOF 里的 native_mode 保护（PROFILE_FIRST 映射 + 兜底）。"""
    profile_first = {
        "boot-safe-1080p240": "1080p240compat",
        "boot-safe-full": "1080p60compat",
        "standard-dual": "1080p120",
        "single-1440p60": "1440p60",
        "single-1080p120": "1080p120",
        "single-1080p144": "1080p144",
        "single-1080p240": "1080p240",
        "single-1440p144": "1440p144",
        "single-2160p60": "2160p60",
    }
    out = []
    for profile in sorted(profile_first) + ["unknown-profile", ""]:
        for nm in ["", "auto", "bogus", "1080p240", "2560x1440@165"]:
            # 复刻脚本逻辑：nm 为空/auto/非法 ⇒ 用 profile 首选，否则原样
            if nm in ("", "auto") or mode_info(nm) is None:
                resolved = profile_first.get(profile) or "1080p60compat"
            else:
                resolved = nm
            out.append({"profile": profile, "native_mode_in": nm, "resolved": resolved})
    return out


def gen_helpers():
    """F. PnP 编解码 + 字段归一工具。"""
    vendors = ["AIB", "OPI", "AAA", "ZZZ", "KQP", "ABC", "XYZ"]
    pnp = []
    for v in vendors:
        enc = _pnp_encode(v)
        pnp.append({"vendor": v, "bytes": _hexof(enc), "roundtrip": _pnp_decode(enc)})
    helpers = []
    for raw, limit, fallback in [("ABC", 3, "OPI"), ("abcd", 3, "OPI"), ("", 3, "OPI"),
                                 ("A" * 20, 13, "TTBOX"), ("TTBox-COMPAT", 13, "TTBOX"),
                                 ("中文", 3, "OPI"), ("A\tB", 3, "OPI")]:
        helpers.append({"fn": "_safe_ascii", "in": raw, "limit": limit,
                        "fallback": fallback, "out": _safe_ascii(raw, limit, fallback)})
    for raw, width, fallback in [("0x2400", 4, "0x3588"), ("2400", 4, "0x3588"),
                                 ("zzz", 4, "0x3588"), ("", 8, "0x20260414"),
                                 ("0x0", 4, "0x3588"), ("0xFFFF", 4, "0x3588"),
                                 ("0x10000", 4, "0x3588")]:
        helpers.append({"fn": "_hex_text", "in": raw, "width": width,
                        "fallback": fallback, "out": _hex_text(raw, width, fallback)})
    for raw in [True, False, 1, 0, "true", "on", "yes", "1", "false", "off", "no", "0", "", "x"]:
        helpers.append({"fn": "_bool_value", "in": raw, "fallback": False,
                        "out": _bool_value(raw, False)})
    return {"pnp": pnp, "helpers": helpers}


def gen_meta():
    """常量表（供 C++ 侧核对表内容一致）。"""
    timings = {}
    for k, tm in TIMING_MAP.items():
        timings[k] = {"w": tm.width, "h": tm.height, "refresh": tm.refresh,
                      "pc": tm.pixel_clock, "hfp": tm.h_front_porch, "hs": tm.h_sync,
                      "hbp": tm.h_back_porch, "vfp": tm.v_front_porch, "vs": tm.v_sync,
                      "vbp": tm.v_back_porch, "h_pol": tm.h_pol, "v_pol": tm.v_pol,
                      "interlaced": tm.interlaced}
    return {
        "timing_map": timings,
        "safe_modes": [list(x) for x in SAFE_MODES],
        "profiles_set": sorted(PROFILES_SET),
        "native_modes_set": sorted(NATIVE_MODES_SET),
        "max_advertised_modes": MAX_ADVERTISED_MODES,
        "is_dynamic_1080p240": is_dynamic_mode_token("1080p240"),
        "is_dynamic_2560x1440@144": is_dynamic_mode_token("2560x1440@144"),
    }


def main():
    out_path = sys.argv[1] if len(sys.argv) > 1 else os.path.join(
        REPO, "core", "tests", "fixtures", "edid_golden.json")
    data = {
        "_doc": DOC,
        "meta": gen_meta(),
        "A_mode_info": gen_mode_info(),
        "B_pixel_clock": gen_pixel_clock(),
        "C_lookup_timing": gen_lookup(),
        "D_build": gen_build(),
        "E_native_mode_guard": gen_native_mode_guard(),
        "F_helpers": gen_helpers(),
    }
    os.makedirs(os.path.dirname(out_path), exist_ok=True)
    with open(out_path, "w", encoding="utf-8") as fh:
        json.dump(data, fh, ensure_ascii=False, indent=1)
    n_build = len(data["D_build"])
    n_ok = sum(1 for x in data["D_build"] if x.get("ok"))
    print("黄金样本已写出: %s" % out_path)
    print("  A mode_info      : %d 例" % len(data["A_mode_info"]))
    print("  B pixel_clock    : %d 例" % len(data["B_pixel_clock"]))
    print("  C lookup_timing  : %d 例" % len(data["C_lookup_timing"]))
    print("  D build          : %d 例（成功 %d）" % (n_build, n_ok))
    print("  E native_mode 保护: %d 例" % len(data["E_native_mode_guard"]))
    print("  F helpers        : pnp %d + util %d" % (
        len(data["F_helpers"]["pnp"]), len(data["F_helpers"]["helpers"])))
    print("  meta: timing_map %d / safe_modes %d" % (
        len(data["meta"]["timing_map"]), len(data["meta"]["safe_modes"])))


if __name__ == "__main__":
    main()
