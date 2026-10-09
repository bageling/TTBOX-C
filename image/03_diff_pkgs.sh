#!/usr/bin/env bash
# 对比：开发板已装包 vs 镜像已装包 → 精确得出"必须补装"的集合
set -u
IMG="$1"
BOARD_PKGS="$2"   # 板端 dpkg 快照文件

LOOP="$(losetup --find --show --partscan --read-only "$IMG")"
RO=/tmp/img_root3; mkdir -p "$RO"
mount -o ro "${LOOP}p2" "$RO"
cleanup(){ umount "$RO" 2>/dev/null||true; losetup -d "$LOOP" 2>/dev/null||true; }
trap cleanup EXIT

# 镜像内已装包（取包名，只取 installed 状态）
IMG_PKGS=/tmp/img_pkgs.txt
awk '
  /^Package: /{p=$2}
  /^Status: install ok installed/{print p}
' "$RO/var/lib/dpkg/status" | sort -u > "$IMG_PKGS"

echo "镜像已装包数: $(wc -l < "$IMG_PKGS")"
echo "板端已装包数: $(wc -l < "$BOARD_PKGS")"

echo
echo "===== 板有 / 镜像没有（候选补装清单，全部） ====="
comm -23 "$BOARD_PKGS" "$IMG_PKGS" > /tmp/only_board.txt
wc -l < /tmp/only_board.txt

echo
echo "===== 其中与 TTBOX 运行相关的（关键词过滤） ====="
grep -iE 'rknn|rga|opencv|flask|waitress|numpy|v4l|jpeg|gstreamer|ffmpeg|mpp|rockchip|python3-|usb|hid|i2c|gpio|edid' /tmp/only_board.txt || echo "(无)"

echo
echo "===== 镜像有 / 板没有（仅供参考） ====="
comm -13 "$BOARD_PKGS" "$IMG_PKGS" | wc -l

echo
echo "===== 镜像内 librga / opencv / rknnrt 实际文件 ====="
find "$RO/usr/lib" "$RO/usr/lib/aarch64-linux-gnu" "$RO/usr/local/lib" -maxdepth 2 \
     \( -name 'librga*' -o -name 'libopencv*' -o -name 'librknn*' \) 2>/dev/null | head -20 || true
echo "(以上为空则确实缺失)"

echo
echo "===== 镜像内 python3 版本与 flask/waitress/numpy ====="
ls "$RO/usr/bin/python3"* 2>/dev/null
find "$RO/usr/lib/python3" -maxdepth 3 -name 'flask' -o -maxdepth 3 -name 'waitress' -o -maxdepth 3 -name 'numpy' 2>/dev/null | head
