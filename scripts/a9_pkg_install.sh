#!/bin/bash
# a9_pkg_install.sh — 板端 HID Package v0.0.1 安装/激活（演示完整生命周期）
set -e
HID=/home/ubuntu/ttbox2/hid
PKG=/home/ubuntu/ttbox2/ttbox/core/build/ttbox-hid-pkg
SRC=/home/ubuntu/ttbox2/hid_pkg_src_001

echo "== 0. 清理残留状态 =="
rm -rf $HID/registry $HID/packages $HID/staging $HID/quarantine $SRC
echo "  done"

echo "== 1. 建立干净包源（0.0.1）=="
mkdir -p $SRC/config $SRC/descriptors $SRC/bin
cp $HID/manifest.json $HID/VERSION $SRC/
cp $HID/config/hid_config.json $SRC/config/
cp $HID/descriptors/*.desc $SRC/descriptors/
cp /home/ubuntu/ttbox2/scripts/a9_setup_hid_gadget.sh $SRC/bin/ 2>/dev/null || true
echo "  done"

echo "== 2. init registry =="
$PKG --root $HID init

echo "== 3. import → staging =="
$PKG --root $HID import $SRC 0.0.1

echo "== 4. validate =="
$PKG --root $HID validate 0.0.1

echo "== 5. install =="
$PKG --root $HID install 0.0.1

echo "== 6. activate =="
$PKG --root $HID activate 0.0.1

echo "== 7. list / get-active =="
$PKG --root $HID list
echo "active=$($PKG --root $HID get-active)"

echo "== 8. 回滚演示（先 deactivate 验证）=="
$PKG --root $HID rollback 2>&1 || echo "  （无 previous，回滚跳过——首次激活场景）"

echo "== 9. health check =="
sudo /home/ubuntu/ttbox2/ttbox/core/build/ttbox-hid-health --root $HID 2>&1 | grep -E 'PASS|FAIL|health'
