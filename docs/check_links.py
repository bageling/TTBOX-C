#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""递归 Markdown 相对链接检查器（覆盖 docs/** 全层，含 archive）。

输出：扫描文件数 / 检查链接数 / 断链数 / 断链清单。
仅检查仓库内相对路径；跳过 http(s)/mailto/纯锚点。
"""
import io
import os
import re
import sys

ROOT = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.abspath(os.path.join(ROOT, ".."))

# 覆盖面：docs/** 全层（含 archive）+ 根 README.md + modules/**
targets = []
for base in ("docs", "modules"):
    for dirpath, dirnames, filenames in os.walk(os.path.join(REPO, base)):
        for fn in filenames:
            if fn.lower().endswith(".md"):
                targets.append(os.path.join(dirpath, fn))
root_readme = os.path.join(REPO, "README.md")
if os.path.isfile(root_readme):
    targets.append(root_readme)

LINK_RE = re.compile(r"\]\(([^)\s]+)\)")
SKIP = ("http://", "https://", "mailto:")
# file:// = 绝对 URI（指向本机/旧工作区路径），非仓库相对链接：单独统计，不计入断链
ABS_FILE = "file://"

scanned = 0
checked = 0
absolute_file_uris = []
broken = []

for path in sorted(targets):
    scanned += 1
    try:
        with io.open(path, encoding="utf-8") as f:
            text = f.read()
    except (IOError, OSError) as e:
        broken.append((path, "<unreadable: %s>" % e))
        continue
    base_dir = os.path.dirname(path)
    for m in LINK_RE.finditer(text):
        link = m.group(1).strip()
        if not link or link.startswith("#") or link.startswith(SKIP):
            continue
        if link.startswith(ABS_FILE):
            absolute_file_uris.append((os.path.relpath(path, REPO), link))
            continue
        target = link.split("#", 1)[0]
        if not target:
            continue
        checked += 1
        full = os.path.normpath(os.path.join(base_dir, target))
        if not os.path.exists(full):
            broken.append((os.path.relpath(path, REPO), link))

print("scanned_files=%d" % scanned)
print("links_checked=%d (repo-relative only)" % checked)
print("absolute_file_uris=%d (out of scope, see list)" % len(absolute_file_uris))
for p, l in absolute_file_uris:
    print("ABS-URI %s -> %s" % (p, l))
print("broken_count=%d" % len(broken))
for p, l in broken:
    print("BROKEN %s -> %s" % (p, l))
print("RESULT=%s" % ("PASS" if not broken else "FAIL"))
