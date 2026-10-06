#!/bin/sh
# run_selftest.sh — 自瞄控制器自测一键入口（V1.0.46）
# 退出码：0=无 FAIL（可能 WARN）  1=有 FAIL  2=脚本/环境错
set -e
DIR=$(cd "$(dirname "$0")" && pwd)
PY=${PY:-python}
cd "$DIR"
"$PY" fitts.py                     # ① 控制器移植自检（fitts.py ↔ core 单测对齐）
"$PY" selftest.py "$@"             # ② 阈值判定 + 回归 + 鲁棒性
