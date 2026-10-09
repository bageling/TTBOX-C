#!/usr/bin/env bash
# 深挖镜像：内核能力 / 引导 / cloud-init / 已装包 / 用户
set -u
IMG="$1"
LOOP="$(losetup --find --show --partscan --read-only "$IMG")"
RO=/tmp/img_root
mkdir -p "$RO"
mount -o ro "${LOOP}p2" "$RO"
cleanup() { umount "$RO" 2>/dev/null || true; losetup -d "$LOOP" 2>/dev/null || true; }
trap cleanup EXIT

echo "===== 1. 引导配置 /boot/extlinux/* ====="
ls -la "$RO/boot/extlinux/"
for f in "$RO"/boot/extlinux/*; do
  [ -f "$f" ] && { echo "--- $(basename "$f") ---"; cat "$f"; }
done

echo
echo "===== 2. 内核 HDMI RX / RGA / ROCKCHIP 能力 ====="
CFG="$RO/boot/config-5.10.0-1012-rockchip"
if [ -f "$CFG" ]; then
  grep -E 'HDMIRX|VIDEO_ROCKCHIP|ROCKCHIP_RGA|CONFIG_DRM_ROCKCHIP=|CONFIG_VIDEO_V4L2=|CONFIG_MEDIA_SUPPORT=' "$CFG" | sort
  echo "-- USB gadget/HID --"
  grep -E 'CONFIG_USB_CONFIGFS|CONFIG_USB_GADGET=|CONFIG_USB_RAW_GADGET|CONFIG_USB_LIBCOMPOSITE|CONFIG_USB_CONFIGFS_F_HID' "$CFG" | sort
else
  echo "(无 $CFG)"
fi

echo
echo "===== 3. dtb / dtbo 可用性 ====="
find "$RO/boot" "$RO/usr/lib" -name '*.dtb' -o -name '*.dtbo' 2>/dev/null | head -40
echo "-- 查 hdmirx 相关 dtbo --"
find "$RO" -name '*hdmirx*' 2>/dev/null | head -20

echo
echo "===== 4. cloud-init ====="
ls -la "$RO/etc/cloud" 2>/dev/null | head
for d in "$RO/etc/cloud/cloud.cfg.d" "$RO/var/lib/cloud"; do
  echo "--- $d"; ls -la "$d" 2>/dev/null | head -15
done
echo "-- cloud-init 状态文件 --"
ls -la "$RO/var/lib/cloud/instance" 2>/dev/null | head

echo
echo "===== 5. 用户 / 组 ====="
grep -vE '^(#|$)' "$RO/etc/passwd" | awk -F: '{print $1":"$3":"$4":"$6":"$7}'
echo "-- groups --"
grep -vE '^(#|$)' "$RO/etc/group" | awk -F: '{print $1":"$3":"$4}'
echo "-- shadow 里设了密码的账号 --"
awk -F: '$2 ~ /^\$/ {print $1" (有密码哈希)"}' "$RO/etc/shadow" 2>/dev/null

echo
echo "===== 6. apt 源 ====="
cat "$RO/etc/apt/sources.list" 2>/dev/null | grep -vE '^\s*#|^\s*$'
ls "$RO/etc/apt/sources.list.d/" 2>/dev/null

echo
echo "===== 7. 关键依赖包是否已在镜像内 ====="
for p in librknnrt librga libjpeg libopencv python3-flask python3-waitress python3-numpy v4l-utils; do
  printf '%-18s ' "$p"
  find "$RO/usr/lib" "$RO/usr/share" -maxdepth 3 -name "*${p}*" 2>/dev/null | head -2 | paste -sd' ' || true
  echo
done
echo "-- dpkg status 摘要 --"
awk '/^Package: (libopencv|python3-flask|python3-waitress|python3-numpy|v4l-utils|librga|libjpeg)/ {p=$2; getline; if ($1=="Status:") print p" "$0}' "$RO/var/lib/dpkg/status" 2>/dev/null | head -30

echo
echo "===== 8. systemd 现状 ====="
ls "$RO/etc/systemd/system/" 2>/dev/null | head -30
echo "-- 已 enable 的 unit --"
ls "$RO/etc/systemd/system/multi-user.target.wants/" 2>/dev/null

echo
echo "===== 9. 磁盘占用 ====="
df -h "$RO"
