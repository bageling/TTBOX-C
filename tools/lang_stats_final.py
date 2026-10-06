#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""TTBOX 语言占比 · 最终口径（v2）

修正 v1 的两个问题：
1) v1 只看了顶层 tests/（541 行），漏掉了 core/tests（20332 行 C++ 测试）、
   plugins/web/tests、framework/tests 等所有测试代码。测试代码必须单独成类。
2) 区分「产品代码」与「测试代码」，测试规模是质量指标，不能混在产品代码里。

分类桶：
  product  — 产品实现代码
  test     — 测试代码（路径含 tests/ 或 test_ 前缀或 *_test.*）
  tool     — 开发/构建工具（tools/、core/tools/、image/、scripts/ 归为脚本层）
  doc      — 文档
  thirdparty — 第三方
"""
import json
import os
import subprocess
import sys
from collections import defaultdict

# 仓库根 = 本脚本所在目录的父目录（从脚本自身位置派生，禁硬编码开发机路径）
REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

EXT = {
    ".cpp": "C++", ".hpp": "C++", ".c": "C", ".py": "Python", ".sh": "Shell",
    ".lua": "Lua", ".js": "JavaScript", ".html": "HTML", ".css": "CSS",
    ".json": "JSON", ".md": "Markdown", ".txt": "Text", ".yml": "YAML",
    ".yaml": "YAML", ".cmake": "CMake", ".service": "Systemd", ".prod": "Nginx配置",
    ".ps1": "PowerShell", ".toml": "TOML", ".mermaid": "Mermaid", ".timer": "Systemd",
    ".h": "C/C++头",
}
LINE_COMMENT = {
    "Python": ["#"], "Shell": ["#"], "Lua": ["--"], "JavaScript": ["//"], "Perl": ["#"],
    "C++": ["//"], "C": ["//"], "HTML": ["<!--"], "CMake": ["#"], "YAML": ["#"],
    "Nginx配置": ["#"], "Systemd": ["#"], "PowerShell": ["#"], "TOML": ["#"],
}
BLOCK = {"C++": ("/*", "*/"), "C": ("/*", "*/")}
SHEBANG = [("sh", "Shell"), ("bash", "Shell"), ("python", "Python"), ("lua", "Lua"), ("node", "JavaScript")]


def detect_by_shebang(path):
    try:
        with open(path, "rb") as f:
            first = f.readline(200).decode("utf-8", errors="replace")
    except OSError:
        return None
    if not first.startswith("#!"):
        return None
    for k, v in SHEBANG:
        if k in first:
            return v
    return None


def classify(rel):
    full = os.path.join(REPO, rel)
    base = os.path.basename(rel)
    ext = os.path.splitext(base)[1].lower()
    if ext == "":
        if base in ("Makefile", "makefile", "GNUmakefile"):
            return "Makefile"
        if "." not in base:
            lang = detect_by_shebang(full)
            if lang:
                return lang
    if base.lower() == "cmakelists.txt":
        return "CMake"
    return EXT.get(ext), ext


def count_lines(text, lang):
    lc = LINE_COMMENT.get(lang, [])
    pair = BLOCK.get(lang)
    in_block = False
    code = comment = blank = 0
    for line in text.splitlines():
        s = line.strip()
        if not s:
            blank += 1
            continue
        if in_block:
            comment += 1
            j = s.find(pair[1])
            if j != -1:
                in_block = False
                rest = s[j + 2:].strip()
                if rest and not any(rest.startswith(p) for p in lc):
                    code += 1
            continue
        if any(s.startswith(p) for p in lc):
            comment += 1
            continue
        if pair:
            i = s.find(pair[0])
            if i != -1:
                j = s.find(pair[1], i + 2)
                if j == -1:
                    in_block = True
                    comment += 1
                    continue
                before, after = s[:i].strip(), s[j + 2:].strip()
                if before:
                    code += 1
                elif after and not any(after.startswith(p) for p in lc):
                    code += 1
                else:
                    comment += 1
                continue
        code += 1
    return code, comment, blank


def bucket_of(rel, lang):
    """决定归入哪个桶"""
    parts = rel.split("/")
    base = parts[-1]
    if rel.startswith("core/third_party/"):
        return "thirdparty"
    if lang in ("Markdown", "Mermaid"):
        return "doc"
    # 测试判定
    if "tests" in parts or base.startswith("test_") or base.endswith("_test.cpp") \
       or base.endswith("_test.py") or base.endswith("_test.js") or "/test_" in rel:
        return "test"
    return "product"


def ns():
    return {"files": 0, "code": 0, "comment": 0, "blank": 0}


def main():
    files = subprocess.run(["git", "ls-files"], cwd=REPO, capture_output=True,
                           text=True, encoding="utf-8").stdout.splitlines()
    buckets = {"product": defaultdict(ns), "test": defaultdict(ns),
               "thirdparty": defaultdict(ns), "doc": defaultdict(ns)}
    skip = defaultdict(int)
    prod_dir = defaultdict(lambda: defaultdict(int))
    test_dir = defaultdict(lambda: defaultdict(int))

    for rel in files:
        full = os.path.join(REPO, rel)
        if not os.path.isfile(full):
            continue
        res = classify(rel)
        if isinstance(res, tuple):
            lang, ext = res
        else:
            lang, ext = res, os.path.splitext(rel)[1].lower()
        if lang is None:
            skip[ext or "(无扩展名)"] += 1
            continue
        try:
            with open(full, "rb") as f:
                raw = f.read()
            if b"\x00" in raw[:8192]:
                skip[(ext or "?") + "(二进制)"] += 1
                continue
            text = raw.decode("utf-8", errors="replace")
        except OSError:
            skip[(ext or "?") + "(读取失败)"] += 1
            continue
        c, cm, bl = count_lines(text, lang)
        b = bucket_of(rel, lang)
        s = buckets[b][lang]
        s["files"] += 1
        s["code"] += c
        s["comment"] += cm
        s["blank"] += bl
        top = rel.split("/")[0]
        if b == "test":
            test_dir[top][lang] += c
        elif b == "product":
            prod_dir[top][lang] += c

    prod = sum(v["code"] for v in buckets["product"].values())
    test = sum(v["code"] for v in buckets["test"].values())
    tp = sum(v["code"] for v in buckets["thirdparty"].values())
    doc = sum(v["code"] for v in buckets["doc"].values())

    print("=" * 88)
    print("TTBOX 语言占比 · 最终口径（产品代码 / 测试代码 分开）")
    print("=" * 88)
    print(f"产品代码 {prod:>8} 行   测试代码 {test:>7} 行   第三方 {tp:>7} 行   文档 {doc:>6} 行")
    print(f"测试代码占产品代码比例：{test / prod * 100:.1f}%   （行业经验参考：10~30% 为健康）")
    print()
    print("【产品代码语言占比】分母 = %d 行" % prod)
    print("-" * 88)
    print(f"{'语言':<12}{'文件数':>7}{'代码行':>9}{'注释行':>9}{'空行':>8}{'占产品':>9}")
    print("-" * 88)
    po = sorted(buckets["product"].items(), key=lambda kv: -kv[1]["code"])
    for lang, s in po:
        print(f"{lang:<12}{s['files']:>7}{s['code']:>9}{s['comment']:>9}{s['blank']:>8}{s['code'] / prod * 100:>8.1f}%")
    print(f"{'产品合计':<12}{sum(v['files'] for v in buckets['product'].values()):>7}{prod:>9}"
          f"{sum(v['comment'] for v in buckets['product'].values()):>9}"
          f"{sum(v['blank'] for v in buckets['product'].values()):>8}{'100.0%':>9}")

    print()
    print("【测试代码语言占比】分母 = %d 行" % test)
    print("-" * 88)
    to = sorted(buckets["test"].items(), key=lambda kv: -kv[1]["code"])
    for lang, s in to:
        print(f"{lang:<12}{s['files']:>7}{s['code']:>9}{s['comment']:>9}{s['blank']:>8}{s['code'] / test * 100:>8.1f}%")

    print()
    print("【产品代码 · 顶层目录分布】")
    print("-" * 88)
    for d in sorted(prod_dir, key=lambda x: -sum(prod_dir[x].values())):
        langs = sorted(prod_dir[d].items(), key=lambda kv: -kv[1])
        print(f"{d:<16}{sum(prod_dir[d].values()):>7} 行   " + ", ".join(f"{k}:{v}" for k, v in langs[:4]))

    print()
    print("【测试代码 · 分布】")
    print("-" * 88)
    for d in sorted(test_dir, key=lambda x: -sum(test_dir[x].values())):
        langs = sorted(test_dir[d].items(), key=lambda kv: -kv[1])
        print(f"{d:<20}{sum(test_dir[d].values()):>6} 行   " + ", ".join(f"{k}:{v}" for k, v in langs[:4]))

    out = {
        "product_code_lines": prod, "test_code_lines": test,
        "thirdparty_code_lines": tp, "doc_lines": doc,
        "test_to_product_ratio": round(test / prod, 4),
        "product_by_language": {k: dict(v) for k, v in po},
        "test_by_language": {k: dict(v) for k, v in to},
        "product_by_dir": {d: dict(v) for d, v in sorted(prod_dir.items(), key=lambda kv: -sum(kv[1].values()))},
        "test_by_dir": {d: dict(v) for d, v in sorted(test_dir.items(), key=lambda kv: -sum(kv[1].values()))},
    }
    p = os.path.join(REPO, "docs", "audit", "lang_stats_final.json")
    os.makedirs(os.path.dirname(p), exist_ok=True)
    with open(p, "w", encoding="utf-8") as f:
        json.dump(out, f, ensure_ascii=False, indent=2)
    print(f"\n[JSON] {p}")


if __name__ == "__main__":
    main()