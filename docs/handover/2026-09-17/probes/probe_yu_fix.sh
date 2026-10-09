#!/bin/bash
# 修正上一版探针的两处自身 bug（A4 留档解析 / A17 判据逻辑写反），并补查 stage 与 G4
cd /mnt/c/Users/Administrator/Desktop/TTBOX-Module-Edition-main || exit 1
SRC=$PWD
HOSTBIN=build-aarch64-t114/ttbox_core_main

echo "########## A4 修正：留档 md5 正确解析 ##########"
grep -nE '^\| `md5`|^\| `sha256`|^\| `commit`' build-aarch64-t114/RELEASE_BUILD.md
M=$(md5sum "$HOSTBIN" | awk '{print $1}')
R=$(sed -n 's/^| `md5` | `\([0-9a-f]\{32\}\)`.*/\1/p' build-aarch64-t114/RELEASE_BUILD.md)
echo "实算=$M 留档=$R"
[ -n "$R" ] && [ "$M" = "$R" ] && echo "A4 PASS（一致）" || echo "A4 FAIL"

echo
echo "########## A17 修正：unit 计数（判据 = 无计数>1 的行） ##########"
find "$SRC" -name '*.service' | sed 's#.*/##' | sort | uniq -c
DUP=$(find "$SRC" -name '*.service' | sed 's#.*/##' | sort | uniq -c | grep -v 'ttbox-ensure' | awk '$1>1')
if [ -n "$DUP" ]; then echo "A17 FAIL 重复: $DUP"; else echo "A17 PASS（各 1 份）"; fi

echo
echo "########## A7/A8：出货 stage 真实位置 ##########"
grep -n 'STAGE=' scripts/ttbox_build_release.sh | head -5
find build-aarch64-t114 -maxdepth 2 -type d -name '*stage*' 2>/dev/null
STAGE=$(grep -oE 'STAGE=[^ ]*' scripts/ttbox_build_release.sh | head -1 | cut -d= -f2)
echo "解析 STAGE 变量定义 = $STAGE"
ls -d build-aarch64-t114/*stage* 2>/dev/null

echo
echo "########## G4：cpu6 硬编码是否还在（全仓搜） ##########"
grep -rn "cpu6\|1ULL << 6\|1 << 6" core/src/ 2>/dev/null | head -5
echo "--- CpuAffinity.cpp 关键段 ---"
sed -n '30,66p' core/src/common/CpuAffinity.cpp

echo
echo "########## G8 / C2：host 构建目录是否存在（ctest 基线） ##########"
ls -d build-host* build-native* 2>/dev/null || echo "无 host 构建目录 ⇒ ctest 基线未在本次实测"
which cmake ninja 2>/dev/null
