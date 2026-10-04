#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""split_application.py — 把 Application.cpp 按职责拆成 4 个编译单元（一次性工具）。

★ 为什么用脚本切而不用手改：36 个成员、2341 行，手切必然漏函数或切坏函数体。
  本脚本按**花括号深度**算每个成员定义的起止行（不用"到下一个函数的距离"——
  那种算法会把匿名命名空间里的辅助函数当成成员，边界算错）。

★ 为什么必须一次性跑完：拆完后各 TU 的成员定义位置就变了，脚本不可重复执行。
  故带 --verify 守卫：已拆过就拒绝运行。

用法：
    python scripts/split_application.py            # 执行拆分
    python scripts/split_application.py --check    # 只报告不写（预演）
"""
from __future__ import annotations

import io
import os
import re
import sys
from pathlib import Path

# ★ 根锚不写死层级（门禁⑩禁 parents[N]）：从本文件位置逐级向上找同时含
#   core/src/app/Application.cpp 与 scripts/ 的目录。找不到直接炸——
#   静默回退到某一上层目录会让脚本改错文件，那比失败严重得多。
def _discover_repo(start: Path) -> Path:
    cur = start.resolve()
    while True:
        if (cur / "core" / "src" / "app" / "Application.cpp").is_file() \
                and (cur / "scripts").is_dir():
            return cur
        parent = cur.parent
        if parent == cur:
            raise SystemExit(
                f"找不到 TTBOX 主根：从 {start} 向上未发现同时含 "
                "core/src/app/Application.cpp 与 scripts/ 的目录"
            )
        cur = parent


REPO = _discover_repo(Path(__file__).resolve().parent)
SRC = REPO / "core" / "src" / "app" / "Application.cpp"

MEMBER_RE = re.compile(
    r'^(?:[A-Za-z_][\w:<>,\s\*&]*?)\bApplication::([a-z_][\w]*)\s*\(')

# ---- 四类归属（成员清单必须与文件里实际存在的定义一一对应，缺一个就报错）----
IPC = [
    "handle_config_update",
    "handle_license_activate",
    "handle_license_activate_cloud",
    "handle_model_activate",
    "handle_model_import",
    "handle_model_install",
    "handle_model_list",
    "handle_model_remove",
    "handle_model_set_concurrency",
    "handle_model_validate",
    "handle_runtime_control",
]
LICENSE = [
    "license_allow_run",
    "license_is_pro",
    "license_status_snapshot",
    "resolve_license_card",
]
# 生命周期 + 对外只读快照（status/status_provider/config_provider/request_shutdown
#   留在主 TU：它们不是"命令处理"，而是主循环与 IPC provider 的实现）
MAIN = [
    "initialize",
    "run",
    "shutdown",
    "request_shutdown",
    "status",
    "status_provider",
    "config_provider",
]
RUNTIME = [
    "apply_preview_degrade",
    "apply_startup_runtime_intent",
    "brand_upper",
    "build_runtime_params",
    "current_feature_gates",
    "gate_missing_summary",
    "has_capture_signal",
    "load_runtime_intent",
    "migrate_output_enabled",
    "persist_runtime_intent",
    "persist_runtime_profile",
    "switch_active_model_runtime",
    "sync_model_id_to_profile",
    "try_resume_from_degraded",
]

# 各新 TU 的文件头注释
HEADER_TMPL = """// {fname} — {title}
//
// ★ 由 Application.cpp 按职责拆分而来（2026-10-04 core 结构治理 S1）。
//   拆分方式 = **只搬定义，不改任何逻辑**：成员函数仍是 Application 的同一个类的成员，
//   只是定义落在别的编译单元（类外定义可跨 TU 分散，这是 C++ 标准允许的）。
//   ⇒ 外部行为、符号名、ABI 全部不变；唯一变化是 .o 的组织方式。
//
// 本文件负责：{responsibility}
{extra}
#include "app/Application.hpp"
"""


def member_bounds(lines: list[str]) -> dict[str, tuple[int, int]]:
    """按花括号深度算出每个 Application 成员定义的 [起, 止] 行号（含收尾行）。

    ★ 不用"到下一个函数定义的距离"：那样会把成员之间的注释/空行算进上一个函数，
      也会在成员之间夹着非成员定义时算错边界。
    """
    out: dict[str, tuple[int, int]] = {}
    for i, ln in enumerate(lines):
        m = MEMBER_RE.match(ln)
        if not m:
            continue
        name = m.group(1)
        depth = 0
        started = False
        j = i
        while j < len(lines):
            for ch in lines[j]:
                if ch == "{":
                    depth += 1
                    started = True
                elif ch == "}":
                    depth -= 1
            if started and depth <= 0:
                break
            j += 1
        if name in out:
            raise SystemExit(f"成员 {name} 有两处定义 —— 枚举正则有误，需修正")
        out[name] = (i, j)
    return out


def slice_func(lines: list[str], span: tuple[int, int],
               above: list[str], below: list[str]) -> list[str]:
    """把成员定义整段取出，并把它上方紧贴的注释块一并带走（不丢注释）。"""
    a, b = span
    start = a
    # 往上吞紧贴的注释行（空行即停）
    k = a - 1
    while k >= 0 and (lines[k].lstrip().startswith("//") or lines[k].strip() == ""):
        if lines[k].strip() == "":
            break
        k -= 1
    start = k + 1
    body = lines[start:b + 1]
    if body and body[-1].strip():
        body.append("")
    return above + body + below


def main() -> int:
    check_only = "--check" in sys.argv
    text = SRC.read_text(encoding="utf-8")
    lines = text.splitlines(keepends=True)
    bounds = member_bounds(lines)

    groups = {"main": MAIN, "ipc": IPC, "license": LICENSE, "runtime": RUNTIME}
    flat = [n for g in groups.values() for n in g]
    if len(flat) != len(set(flat)):
        dup = sorted({n for n in flat if flat.count(n) > 1})
        raise SystemExit(f"归属清单里有名重复：{dup}")
    missing = [n for n in flat if n not in bounds]
    extra = sorted(set(bounds) - set(flat))
    if missing:
        raise SystemExit(f"归属清单里的成员在文件里找不到定义：{missing}")
    if extra:
        raise SystemExit(
            f"★ 文件里有 {len(extra)} 个成员没归类：{extra}\n"
            "请先决定它们归哪一类，再跑本脚本（不允许'先拆了再说'）。"
        )
    print(f"核对通过：{len(flat)} 个成员，四类无重无漏")

    chunks = {k: [] for k in groups}
    # 把每个成员整段（含其上方注释）搬到目标文件
    taken = [False] * len(lines)
    for key, names in groups.items():
        for n in names:
            a, b = bounds[n]
            k = a - 1
            while k >= 0 and lines[k].lstrip().startswith("//"):
                k -= 1
            s = k + 1
            if any(taken[s:b + 1]):
                raise SystemExit(f"成员 {n} 的区间与已搬区间重叠")
            for t in range(s, b + 1):
                taken[t] = True
            seg = lines[s:b + 1]
            if seg and seg[-1].strip():
                seg = seg + [""]
            if chunks[key]:
                chunks[key].append("")
            chunks[key].extend(seg)

    remaining = [ln for i, ln in enumerate(lines) if not taken[i]]

    if check_only:
        print(f"[预演] 主 TU 余 {len(remaining)} 行；" +
              "；".join(f"{k}={len(v)} 行" for k, v in chunks.items()))
        return 0

    out_dir = SRC.parent
    specs = [
        ("ApplicationIpc.cpp", "ipc", "应用 IPC 命令处理（11 个 handle_*）",
         "// ★ 这 11 个函数是 IPC 命令表的实现：改一条命令只需要动这一个文件。\n"
         "//   它们只**向下**依赖 runtime 组（persist_runtime_profile /\n"
         "//   switch_active_model_runtime / persist_runtime_intent /\n"
         "//   try_resume_from_degraded），不反向依赖 —— 依赖是单向的，无环。"),
        ("ApplicationLicense.cpp", "license", "授权状态查询（4 个）",
         "// ★ 这一组与其它三组**零调用耦合**（机械枚举验证：4 个函数只被外部/IPC 调用，\n"
         "//   组内 4 个互不调用，也不调任何 Application 工具）⇒ 独立成 TU 最干净。"),
        ("ApplicationRuntime.cpp", "runtime", "运行时参数/模型热切换/启停意愿（14 个）",
         "// ★ 这一组是「装配零件」：功能门读取、预览降级、RuntimeProfile 持久化、\n"
         "//   模型热切换回滚、启停意愿落盘。调用方是主 TU 的生命周期与 IPC 组。"),
    ]

    # ★ main 组的成员（initialize/run/shutdown 等）不写新文件 —— 它们要**留回主 TU**。    #   把它们按原顺序插回 remaining 里"匿名命名空间之后、~Application 之后"的位置，
    #   而不是丢掉（第一版就是这里出的 bug：main 组只参与 chunks 收集，从未回写，
    #   ⇒ initialize/run/shutdown 三个最关键的成员凭空消失，还编译不出来报错）。
    #   做法：在 remaining 里找 `Application::~Application()` 之前的空行区，
    #   把 main 组整段插在它后面（析构在前、成员定义在后，与原文顺序一致）。
    main_seg = chunks["main"]
    anchor = next((i for i, ln in enumerate(remaining)
                   if ln.startswith("Application::~Application()")), None)
    if anchor is None:
        raise SystemExit("找不到 `Application::~Application()` 作为回插锚点")
    remaining = remaining[:anchor] + ["\n"] + main_seg + remaining[anchor:]
    SRC.write_text("".join(remaining), encoding="utf-8", newline="")

    for fname, key, title, extra in specs:
        head = HEADER_TMPL.format(
            fname=fname, title=title, responsibility=title, extra=extra)
        body = "".join(chunks[key]).strip("\n")
        (out_dir / fname).write_text(head + "\n" + body + "\n", encoding="utf-8", newline="")
        print(f"已写出 {fname}")

    print(f"主 TU {SRC.name}: 2341 -> {len(remaining)} 行")
    for fname, key, _, _ in specs:
        n = len((out_dir / fname).read_text(encoding="utf-8").splitlines())
        print(f"  {fname}: {n} 行")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
