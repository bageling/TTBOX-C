#!/bin/sh
# run_selftest.sh — 自瞄控制器自测一键入口（V1.0.46）
# ★ 分工：控制逻辑与物理仿真全在 C++（core/tools/replay/replay_main.cpp，链接产品控制器）；
#   本脚本（Python）只做「调 C++ 回放器 + 判阈值 + 出报告 + 对比回归基线」。
# 退出码：0=无 FAIL（可能 WARN）  1=有 FAIL  2=找不到 C++ 回放器
set -e
DIR=$(cd "$(dirname "$0")" && pwd)
REPO=$(cd "$DIR/../../.." && pwd)
PY=${PY:-python}
# 先确保 C++ 回放器已编译（它是唯一的控制逻辑实现）
if [ ! -f "$REPO/core/build-ascii/ttbox_replay.exe" ] && [ ! -f "$REPO/core/build-ascii/ttbox_replay" ]; then
  echo "C++ 回放器未编译：cd core/build-ascii && cmake --build . --target ttbox_replay"
  exit 2
fi
cd "$DIR"
exec "$PY" selftest.py "$@"
