#!/bin/bash
# a9_pkg_guard_verify.sh — 板端验证：禁删 active / deactivate / remove inactive / 恢复 active=0.0.1
set -e
HID=/home/ubuntu/ttbox2/hid
PKG=/home/ubuntu/ttbox2/ttbox/core/build/ttbox-hid-pkg

echo "== 1. remove active 0.0.1 应拒绝 =="
$PKG --root $HID remove 0.0.1 || echo "  （已拒绝，符合规则）"

echo "== 2. deactivate =="
$PKG --root $HID deactivate
echo "  active=$($PKG --root $HID get-active)"

echo "== 3. 重新激活 0.0.1 =="
$PKG --root $HID activate 0.0.1
echo "  active=$($PKG --root $HID get-active)"

echo "== 4. remove inactive 0.0.0 应成功 =="
$PKG --root $HID remove 0.0.0
$PKG --root $HID list
