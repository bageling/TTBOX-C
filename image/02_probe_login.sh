#!/usr/bin/env bash
# 看 cloud-init 种子 + fake-cloud + ssh 配置
set -u
IMG="$1"
LOOP="$(losetup --find --show --partscan --read-only "$IMG")"
RO=/tmp/img_root2; R1=/tmp/seed_p1
mkdir -p "$RO" "$R1"
mount -o ro "${LOOP}p1" "$R1"
mount -o ro "${LOOP}p2" "$RO"
cleanup() { umount "$R1" 2>/dev/null||true; umount "$RO" 2>/dev/null||true; losetup -d "$LOOP" 2>/dev/null||true; }
trap cleanup EXIT

echo "########## CIDATA 种子分区 (p1) ##########"
for f in meta-data user-data network-config; do
  echo "===== $f ====="; cat "$R1/$f" 2>/dev/null; echo
done

echo "########## /etc/cloud/cloud.cfg.d/99-fake-cloud.cfg ##########"
cat "$RO/etc/cloud/cloud.cfg.d/99-fake-cloud.cfg" 2>/dev/null

echo
echo "########## /etc/cloud/cloud.cfg 关键段 ##########"
sed -n '1,60p' "$RO/etc/cloud/cloud.cfg" 2>/dev/null

echo
echo "########## sshd 配置 ##########"
grep -vE '^\s*#|^\s*$' "$RO/etc/ssh/sshd_config" 2>/dev/null
echo "-- sshd_config.d --"; ls -la "$RO/etc/ssh/sshd_config.d/" 2>/dev/null
cat "$RO/etc/ssh/sshd_config.d/"*.conf 2>/dev/null

echo
echo "########## 根文件系统关键项 ##########"
echo "-- /etc/fstab --"; grep -vE '^\s*#|^\s*$' "$RO/etc/fstab" 2>/dev/null
echo "-- /etc/hostname --"; cat "$RO/etc/hostname" 2>/dev/null
echo "-- /etc/machine-id --"; cat "$RO/etc/machine-id" 2>/dev/null; echo
echo "-- /etc/ttbox 是否存在 --"; ls -la "$RO/etc/ttbox" 2>/dev/null || echo "(无)"
echo "-- /var/lib/ttbox 是否存在 --"; ls -la "$RO/var/lib/ttbox" 2>/dev/null || echo "(无)"
echo "-- /opt 内容 --"; ls -la "$RO/opt" 2>/dev/null
echo "-- journald 持久化? --"; grep -E '^\s*Storage' "$RO/etc/systemd/journald.conf" 2>/dev/null || echo "(默认)"
echo "-- /etc/resolv.conf --"; ls -la "$RO/etc/resolv.conf" 2>/dev/null

echo
echo "########## apt sources 明细 ##########"
for f in "$RO"/etc/apt/sources.list.d/*.list "$RO"/etc/apt/sources.list.d/*.sources; do
  [ -f "$f" ] && { echo "--- $(basename "$f")"; grep -vE '^\s*#|^\s*$' "$f" | head -12; }
done

echo
echo "########## 是否有 u-boot 工具 / u-boot-update ##########"
ls -la "$RO/usr/bin/u-boot-update" 2>/dev/null || echo "(无 u-boot-update)"
echo "-- linux-image 包列表 --"
awk '/^Package: linux-/ {p=$2} /^Status: install ok installed/ {print p}' "$RO/var/lib/dpkg/status" 2>/dev/null | sort -u
