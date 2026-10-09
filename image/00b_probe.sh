#!/usr/bin/env bash
# 只读探测镜像内部分区：不写盘，挂载点全用 ro。
set -u
IMG="$1"

echo "== WSL 磁盘余量 =="
df -h / /mnt/c 2>/dev/null

echo
echo "== 挂 loop（partscan，只读语义）=="
LOOP="$(losetup --find --show --partscan --read-only "$IMG")"
echo "loop=$LOOP"
trap 'losetup -d "$LOOP" 2>/dev/null || true; umount /tmp/probe_p1 /tmp/probe_p2 2>/dev/null || true' EXIT

sleep 1
ls -l "${LOOP}"* 2>/dev/null

echo
echo "== blkid 各分区 =="
for p in "${LOOP}p1" "${LOOP}p2"; do
    [ -e "$p" ] || { echo "$p 不存在"; continue; }
    printf '%-14s ' "$p"
    blkid -o value -s TYPE -s LABEL -s UUID "$p" 2>/dev/null | paste -sd' ' || echo '(blkid 无输出)'
done

echo
echo "== 分区容量 (dumpe2fs / FAT info) =="
mkdir -p /tmp/probe_p1 /tmp/probe_p2
for n in 1 2; do
    p="${LOOP}p$n"
    [ -e "$p" ] || continue
    t="$(blkid -o value -s TYPE "$p" 2>/dev/null)"
    echo "--- $p type=${t:-?}"
    case "$t" in
        ext*) dumpe2fs -h "$p" 2>/dev/null | sed -n 's/^\(Filesystem volume name\|Block count\|Block size\|Free blocks\|Filesystem state\|Last mount time\|Filesystem UUID\):/\1:/p' ;;
        vfat)
            mount -o ro "$p" /tmp/probe_p$n 2>/dev/null && {
                echo "  label/内容:"; ls -la /tmp/probe_p$n 2>/dev/null | head -20
                umount /tmp/probe_p$n 2>/dev/null
            } ;;
    esac
done

echo
echo "== 挂主要分区看目录（只读）=="
for n in 1 2; do
    p="${LOOP}p$n"
    [ -e "$p" ] || continue
    m=/tmp/probe_p$n
    mount -o ro "$p" "$m" 2>/dev/null || { echo "$p 挂载失败"; continue; }
    echo "--- $p 根目录 ---"
    ls -la "$m" 2>/dev/null | head -30
    if [ -f "$m/etc/os-release" ]; then
        echo "  [os-release]"; sed -n 's/^\(PRETTY_NAME\|VERSION\)=/\1=/p' "$m/etc/os-release"
    fi
    if [ -d "$m/boot" ]; then
        echo "  [boot/]"; ls -la "$m/boot" 2>/dev/null | head -25
    fi
    if [ -d "$m/opt" ]; then echo "  [opt/]"; ls -la "$m/opt" 2>/dev/null; fi
    if [ -f "$m/etc/group" ]; then
        echo "  [已存在 ttbox? ] $(grep -c ttbox "$m/etc/group" 2>/dev/null || echo 0) 条"
    fi
    umount "$m" 2>/dev/null
done
