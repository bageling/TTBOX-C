#!/usr/bin/env bash
# 卸载镜像（逆序）；幂等。
set -u
IMG_ROOT="${IMG_ROOT:-/mnt/img}"
LOOPFILE="${LOOPFILE:-/root/ttbox-image/loop.dev}"

echo "== 还原 resolv.conf（若曾临时覆盖）=="
if [ -f /root/ttbox-image/resolv.conf.orig ] && mountpoint -q "$IMG_ROOT"; then
    cp -a /root/ttbox-image/resolv.conf.orig "$IMG_ROOT/etc/resolv.conf" 2>/dev/null \
        && echo "  已还原" || echo "  还原跳过"
fi

echo "== 卸载 =="
for d in run sys proc dev/pts dev; do
    if mountpoint -q "$IMG_ROOT/$d" 2>/dev/null; then
        umount -l "$IMG_ROOT/$d" 2>/dev/null && echo "  umount $d" || echo "  umount $d 失败"
    fi
done
if mountpoint -q "$IMG_ROOT" 2>/dev/null; then
    umount "$IMG_ROOT" 2>/dev/null && echo "  umount $IMG_ROOT" || umount -l "$IMG_ROOT" 2>/dev/null || echo "  umount $IMG_ROOT 失败"
fi

if [ -f "$LOOPFILE" ]; then
    LOOP="$(cat "$LOOPFILE")"
    if [ -b "$LOOP" ]; then
        losetup -d "$LOOP" 2>/dev/null && echo "  detach $LOOP" || echo "  detach $LOOP 失败"
    fi
    rm -f "$LOOPFILE"
fi

echo "== 剩余挂载检查 =="
mount | grep -E "${IMG_ROOT}" || echo "  (无残留)"
losetup -a | grep "$(basename "${WORK:-work.img}")" || echo "  (无残留 loop)"
echo "完成"
