#!/bin/bash
# a9_pkg_rollback_demo.sh — 板端 HID Package 双版本回滚演示（0.0.0 ←→ 0.0.1）
# 演示：install/activate/升级/回滚，结束状态恢复 active=0.0.1
set -e
HID=/home/ubuntu/ttbox2/hid
PKG=/home/ubuntu/ttbox2/ttbox/core/build/ttbox-hid-pkg
SRC=/tmp/hid_src_000

echo "== 0. 清理残留状态 + 构造 0.0.0 包源 =="
rm -rf $HID/registry $HID/packages $HID/staging $HID/quarantine
rm -rf $SRC
mkdir -p $SRC/config $SRC/descriptors
cp $HID/manifest.json $HID/VERSION $SRC/
cp $HID/config/hid_config.json $SRC/config/
cp $HID/descriptors/*.desc $SRC/descriptors/
python3 - <<'EOF'
import json
p = "/tmp/hid_src_000/manifest.json"
m = json.load(open(p))
m["version"] = "0.0.0"
json.dump(m, open(p, "w"), indent=2, ensure_ascii=False)
open("/tmp/hid_src_000/VERSION", "w").write("0.0.0")
EOF
echo "  done"

echo "== 1. import → validate → install → activate 0.0.0（模拟旧版本，active=0.0.0）=="
$PKG --root $HID import $SRC 0.0.0
$PKG --root $HID validate 0.0.0
$PKG --root $HID install 0.0.0
$PKG --root $HID activate 0.0.0
echo "  active=$($PKG --root $HID get-active)"

echo "== 2. 安装并激活 0.0.1（升级，active=0.0.1，previous=0.0.0）=="
SRC1=/tmp/hid_src_001
rm -rf $SRC1
mkdir -p $SRC1/config $SRC1/descriptors
cp $HID/manifest.json $HID/VERSION $SRC1/
cp $HID/config/hid_config.json $SRC1/config/
cp $HID/descriptors/*.desc $SRC1/descriptors/
$PKG --root $HID import $SRC1 0.0.1
$PKG --root $HID validate 0.0.1
$PKG --root $HID install 0.0.1
$PKG --root $HID activate 0.0.1
echo "  active=$($PKG --root $HID get-active)"

echo "== 3. rollback → 0.0.0（验证回滚）=="
$PKG --root $HID rollback
echo "  active=$($PKG --root $HID get-active)"

echo "== 4. rollback → 恢复 0.0.1（演示后可再次回滚，禁止无可用包）=="
$PKG --root $HID rollback
echo "  active=$($PKG --root $HID get-active)"

echo "== 5. list =="
$PKG --root $HID list
