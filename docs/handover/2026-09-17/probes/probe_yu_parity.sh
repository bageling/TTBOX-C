#!/bin/bash
# TTBOX × yu 对标：host 侧可验项的**实测**（不照抄文档结论）
cd /mnt/c/Users/Administrator/Desktop/TTBOX-Module-Edition-main || exit 1
SRC=$PWD
HOSTBIN=build-aarch64-t114/ttbox_core_main
STAGE=build-aarch64-t114/stage

echo "########## 0) 构建/测试环境 ##########"
ls -d build-host* build-aarch64* 2>/dev/null
echo "--- host build 是否有 ctest ---"
for d in build-host build-host-native build-native; do
  [ -d "$d" ] && echo "$d: $(ctest --test-dir "$d" -N 2>/dev/null | tail -1)"
done

echo
echo "########## 1) A1/A2 产物字符串三档门禁 ##########"
if [ -f "$HOSTBIN" ]; then
  echo "A1 第三方域名(antszy|blpro|blpt) = $(strings -a "$HOSTBIN" | grep -Ec 'antszy|blpro|blpt')  (期望0)"
  echo "A2 自有端点(38.127.133.6)        = $(strings -a "$HOSTBIN" | grep -c '38\.127\.133\.6')  (M1期望0/登记)"
else
  echo "产物缺失: $HOSTBIN"
fi

echo
echo "########## 2) A3 凭据默认值源码门禁 ##########"
grep -nE 'client_secret_[[:space:]]*=[[:space:]]*"[^"]+"' core/src/auth/TtboxLicenseClient.hpp && echo "命中>0 FAIL" || echo "A3 命中 0 PASS"

echo
echo "########## 3) A4 产物 md5 == 留档 ##########"
if [ -f "$HOSTBIN" ]; then
  M=$(md5sum "$HOSTBIN" | awk '{print $1}')
  R=$(grep -oE 'md5=`[0-9a-f]+`' build-aarch64-t114/RELEASE_BUILD.md | head -1 | grep -oE '[0-9a-f]{32}')
  echo "实算 md5 = $M"
  echo "留档 md5 = $R"
  [ "$M" = "$R" ] && echo "A4 一致 PASS" || echo "A4 不一致 FAIL"
fi

echo
echo "########## 4) A5 RUNPATH ##########"
[ -f "$HOSTBIN" ] && readelf -d "$HOSTBIN" | grep -E 'RUNPATH|RPATH'

echo
echo "########## 5) A7/A8 payload bin 闭集 ##########"
if [ -d "$STAGE/bin" ]; then
  echo "--- stage/bin 文件 ---"; find "$STAGE/bin" -maxdepth 1 -type f -printf '%f\n' | sort
  echo "--- .bak-*/_backup* ---"; find "$STAGE/bin" -maxdepth 1 \( -name '*.bak-*' -o -name '*_backup*' \) | sort
else
  echo "stage 不存在（构建产物已被清理或目录不同）: $STAGE"
  ls build-aarch64-t114 2>/dev/null | head
fi

echo
echo "########## 6) A17 unit 单一权威（同名 .service 计数） ##########"
find "$SRC" -name '*.service' | sed 's#.*/##' | sort | uniq -c | grep -v ' ttbox-ensure' | awk '$1!=1' && echo "有重复 FAIL" || echo "A17 无重复 PASS"
echo "--- 全部 unit ---"
find "$SRC" -name '*.service' | sed 's#.*/##' | sort

echo
echo "########## 7) A30 usbproxy 诚实性 ##########"
if [ -d usbproxy ]; then
  (cd usbproxy && ls -l usb-proxy* 2>/dev/null | head; sha256sum -c usb-proxy.sha256 2>&1 | head -2)
  echo "旧路径字面量 = $(strings -a usbproxy/usb-proxy 2>/dev/null | grep -Ec '/opt/ttbox/(src/)?usbproxy')  (期望0)"
fi

echo
echo "########## 8) G4 CPU 亲和（是否仍硬编码 cpu6） ##########"
grep -rn "cpu6\|cpu_set\|CPU_SET\|affinity" core/src/*/CpuAffinity.* 2>/dev/null | head -10

echo
echo "########## 9) G6/G7 字段：features / ui_brand 是否在契约里 ##########"
grep -n "'features'\|'ui_brand'\|'is_pro'\|'plan'" plugins/web/bin/ttbox-web.py | head -10

echo
echo "########## 10) X1/T1.11 OTA 验签是否已存在 ##########"
ls -d tools/ota 2>/dev/null || echo "tools/ota 不存在"
ls scripts/ | grep -i ota || echo "scripts/ 下无 ota 脚本"
grep -rln "Ed25519\|ed25519" --include=*.py --include=*.cpp --include=*.hpp . 2>/dev/null | grep -v build | head

echo
echo "########## 11) G1 自愈脚本是否已有（T1.03） ##########"
ls scripts/ | grep -i ensure || echo "无 ensure 脚本"
ls deploy/systemd/ 2>/dev/null

echo
echo "########## 12) git 状态与 HEAD ##########"
git log --oneline -1
git status --porcelain | head
