#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""ota_golden_gen.py — 冻结 OTA 更新器的黄金样本（C++ 移植的逐字节对拍基准）。

为什么
    V1.0.56 把 OTA 从 Python 移植到 C++（业主令「去掉所有 Python 代码」）。
    OTA 是**唯一升级通道**，移植错了会导致板子再也升不动（V1.0.52 那次事故的教训）。
    「是否等价」必须以**现役 Python 实现**的输出为准 ⇒ 本脚本必须在删 Python **之前**跑，
    把结果冻结成 JSON 供 C++ 单测对拍。

覆盖（全部为纯函数 / 纯逻辑，不碰网络与硬件）
    A. canonical(rec)       —— 验签用的**紧凑 JSON 序列化**（sort_keys + 无空格 + ensure_ascii=False）
                               ★ 一个字节不同 ⇒ 验签必失败 ⇒ 最高优先级对拍项
    B. _version_key(v)      —— 版本排序键（数字段按数值、其余按字典序）
    C. is_downgrade(n, c)   —— 降级判定（相等也算拒）
    D. _check_safe_id(v)    —— 标识符消毒（正则 + 拒 '..'）
    E. 常量表               —— SIGNED_FIELDS / HEALTH_UNITS / DEFAULT_KEY_ID / _SAFE_ID_RE

用法： <python> tools/ota_golden_gen.py [out.json]
      缺省输出 core/tests/fixtures/ota_golden.json
"""
from __future__ import annotations

import json
import os
import sys

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(REPO, "scripts"))

import ttbox_ota_updater as u  # noqa: E402

DOC = {
    "generated_from": "scripts/ttbox_ota_updater.py（现役 Python 实现）",
    "purpose": "OTA C++ 移植的逐字节对拍基准；不得在删 Python 前丢失本文件",
    "note": "canonical 的输出是**字符串**（已 decode utf-8）；version_key 是嵌套数组",
}


def gen_canonical():
    """A. canonical(rec) —— 验签序列化，必须逐字节一致。"""
    inputs = [
        {"sha256": "abc", "version": "V1.0.1", "built_at": 123, "key_id": "k"},
        # 键顺序不同 ⇒ 输出必须相同（sort_keys=True）
        {"key_id": "k", "version": "V1.0.1", "sha256": "abc", "built_at": 123},
        {"built_at": 123, "sha256": "abc", "key_id": "k", "version": "V1.0.1"},
        {"version": "V1.0.1"},
        {},
        # ensure_ascii=False ⇒ 中文原样输出（不是 \uXXXX）
        {"sha256": "中文字符", "version": "V1.0.1", "built_at": 0, "key_id": "k"},
        # 类型混合与转义
        {"n": 1.5, "b": True, "z": None, "s": 'quote"and\\backslash'},
        {"n": 0, "b": False},
        {"big": 1791286707},
        {"nested": {"x": 1}, "arr": [1, 2, 3]},
        # 真实旁车签名的形状（SIGNED_FIELDS 子集）
        {"sha256": "3b528ca2d2f6da3d5dccef7cb98feaff94347f7d27371b962a601649a69f17da",
         "version": "V1.0.53", "built_at": 1791286707, "key_id": "ttbox-ota-2026b"},
    ]
    out = []
    for c in inputs:
        out.append({"in": c, "out": u.canonical(c).decode("utf-8")})
    return out


def gen_version_key():
    """B. _version_key(v)。"""
    vers = [
        "V1.0.1", "V1.0.10", "V1.0.55", "V1.0.9", "1.5.70", "1.5.7",
        "", "abc", "V1.0", "V1", "V1.0.1-beta", "1.0.1_2", "1-0+1",
        "V1.0.1.1", "V2.0.0", "v1.0.1", "1.0.1-delta-from-1.5.5",
    ]
    return [{"in": v, "out": [list(seg) for seg in u._version_key(v)]} for v in vers]


def gen_is_downgrade():
    """C. is_downgrade(new, cur) —— 相等也算降级（无意义重装）。

    ★ 必须含 V1.0.9 / V1.0.10 这一组：它验证的是「数字段按**数值**比较」
      （9 < 10），而不是字典序（"9" > "10"）。漏了这组就等于没测版本比较。
    """
    vers = ["V1.0.1", "V1.0.9", "V1.0.10", "V1.0.55", "1.5.70", "", "abc", "V2.0.0"]
    out = []
    for a in vers:
        for b in vers:
            out.append({"new": a, "cur": b, "out": u.is_downgrade(a, b)})
    return out


def gen_check_safe_id():
    """D. _check_safe_id(value, what)。"""
    ids = [
        "ttbox-ota-2026b", "abc", "a", "ABC-123_x.y",
        "a" * 64,
        "", "   ",
        "../../etc/passwd", "..", "a..b", "../x",
        "a/b", "a b", "a\tb", "-lead", ".dot", "_under",
        "中文", "a\nb", "a$b", "a;b", "a|b",
    ]
    out = []
    for v in ids:
        try:
            r = u.OtaUpdater._check_safe_id(v, "test")
            out.append({"in": v, "ok": True, "out": r})
        except Exception as e:  # noqa: BLE001
            out.append({"in": v, "ok": False, "state": getattr(e, "state", ""), "err": str(e)})
    return out


def gen_meta():
    """E. 常量表。"""
    return {
        "SIGNED_FIELDS": list(u.SIGNED_FIELDS),
        "HEALTH_UNITS": list(u.HEALTH_UNITS),
        "HEALTH_TIMEOUT_S": u.HEALTH_TIMEOUT_S,
        "DEFAULT_KEY_ID": u.DEFAULT_KEY_ID,
        "DEFAULT_JOBS_DIR": u.DEFAULT_JOBS_DIR,
        "SAFE_ID_RE": u.OtaUpdater._SAFE_ID_RE.pattern,
        # 版本比较的自证样例（供 C++ 侧 sanity）
        "order_samples": [["V1.0.9", "V1.0.10"], ["V1.0.55", "V1.0.9"], ["abc", "1.0.0"]],
    }


def gen_safe_members():
    """F. _safe_members 的成员名判定 —— 解包环节的安全命门。

    ★ 为什么这条最该单测：更新器以 **root** 身份解**不可信**的 tar，
      一个 `payload/../../etc/x` 成员就是 RCE 面。Python 侧用 realpath 判定越界。

    本段**不复制判定逻辑**，而是现场造一个真 tar（含指定成员名）交给现役
    `_safe_members` —— 这样基准反映的是真实现，而不是我对它的理解。
    """
    import io
    import tarfile

    def probe(name):
        buf = io.BytesIO()
        with tarfile.open(fileobj=buf, mode="w") as tf:
            ti = tarfile.TarInfo(name)
            ti.size = 0
            tf.addfile(ti)
        buf.seek(0)
        with tarfile.open(fileobj=buf, mode="r") as tf:
            try:
                u.OtaUpdater._safe_members(tf, "/tmp/ota_dest_probe")
                return {"ok": True}
            except Exception as e:  # noqa: BLE001
                return {"ok": False, "state": getattr(e, "state", ""), "err": str(e)}

    names = [
        "RELEASE_MANIFEST.json",
        "payload",
        "payload/",
        "payload/bin/ttbox_core_main",
        "payload/plugins/web/static/panel/10-flow.js",
        "payload/a/../b",          # realpath 归一后仍在 dest 内 ⇒ 合法
        "payload/./x",             # 归一后合法
        "./payload/y",
        "",                        # 空名 → 拒
        ".",                       # 指向 dest 本身 → 合法
        "..",                      # 逃出 → 拒
        "/abs/path",               # 绝对路径 → 拒
        "/",                       # 根 → 拒
        "../escape",               # 逃出 → 拒
        "payload/../../etc/passwd",  # 逃出（两级）→ 拒
        "payload/../..",           # 逃出 → 拒
        "a/../../b",
        "./../b",                  # 归一后逃出 → 拒
        "payload//double",         # 双斜杠归一后合法
        "payload/子目录/文件.txt",   # 非 ASCII 合法
    ]
    out = []
    for n in names:
        r = {"in": n}
        r.update(probe(n))
        out.append(r)
    return out


def main():
    out_path = sys.argv[1] if len(sys.argv) > 1 else os.path.join(
        REPO, "core", "tests", "fixtures", "ota_golden.json")
    data = {
        "_doc": DOC,
        "E_meta": gen_meta(),
        "A_canonical": gen_canonical(),
        "B_version_key": gen_version_key(),
        "C_is_downgrade": gen_is_downgrade(),
        "D_check_safe_id": gen_check_safe_id(),
        "F_safe_members": gen_safe_members(),
    }
    os.makedirs(os.path.dirname(out_path), exist_ok=True)
    with open(out_path, "w", encoding="utf-8") as fh:
        json.dump(data, fh, ensure_ascii=False, indent=1)
    print("OTA 黄金样本已写出: %s" % out_path)
    for k in ("A_canonical", "B_version_key", "C_is_downgrade", "D_check_safe_id", "F_safe_members"):
        print("  %-16s: %d 例" % (k, len(data[k])))
    print("  E_meta           : SIGNED_FIELDS=%s" % data["E_meta"]["SIGNED_FIELDS"])


if __name__ == "__main__":
    main()
