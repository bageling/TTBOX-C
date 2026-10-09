#!/usr/bin/env bash
# 挂载镜像根分区到 /mnt/img，并 bind 挂载 chroot 所需伪文件系统
# 幂等：重复执行安全。卸载用 29_umount.sh。
set -eu

WORK="${WORK:-/root/ttbox-image/work.img}"
IMG_ROOT="${IMG_ROOT:-/mnt/img}"
LOOPFILE="${LOOPFILE:-/root/ttbox-image/loop.dev}"

echo "== 1. loop 挂载 =="
# 判据必须是"某个 loop 是否真的以本镜像为 backing file"，不能只看 loop 设备节点存在——
# /dev/loopN 节点在 WSL 里恒存在，VM 重启后旧号会误判为"已挂载"。
LOOP="$(losetup -j "$WORK" --noheadings -O NAME 2>/dev/null | head -1)"
if [ -n "$LOOP" ]; then
    echo "复用已附着本镜像的 loop: $LOOP"
else
    rm -f "$LOOPFILE"
    LOOP="$(losetup --find --show --partscan "$WORK")"
    echo "新建 loop: $LOOP"
fi
echo "$LOOP" > "$LOOPFILE"
# partscan 后分区节点可能稍晚出现
for _ in 1 2 3 4 5; do [ -b "${LOOP}p2" ] && break; sleep 0.4; done
ls -l "${LOOP}"* 2>/dev/null
[ -b "${LOOP}p2" ] || { echo "ERROR: ${LOOP}p2 未出现" >&2; exit 1; }

echo
echo "== 2. 挂根分区 =="
mkdir -p "$IMG_ROOT"
if mountpoint -q "$IMG_ROOT"; then
    echo "$IMG_ROOT 已挂载，跳过"
else
    mount "${LOOP}p2" "$IMG_ROOT"
    echo "已挂载 ${LOOP}p2 -> $IMG_ROOT"
fi

echo
echo "== 3. bind 伪文件系统（chroot/apt 必需）=="
for d in dev dev/pts proc sys run; do
    mkdir -p "$IMG_ROOT/$d"
    if mountpoint -q "$IMG_ROOT/$d"; then
        echo "  $d 已挂载"
    else
        mount --bind "/$d" "$IMG_ROOT/$d"
        echo "  bind /$d -> $IMG_ROOT/$d"
    fi
done
# /dev/ptmx 与 fd/pts 补挂（apt 交互需要）
[ -e "$IMG_ROOT/dev/ptmx" ] || true

echo
echo "== 4. DNS 可用性（chroot 内 apt 需要）=="
if [ -f "$IMG_ROOT/etc/resolv.conf" ]; then
    ls -l "$IMG_ROOT/etc/resolv.conf"
    # 若是指向 /run/systemd/resolve/stub-resolv.conf 的悬空软链，chroot 内无 systemd 会导致解析失败
    if [ ! -s "$IMG_ROOT/etc/resolv.conf" ] || ! getent -s files hosts ports.ubuntu.com >/dev/null 2>&1; then
        :
    fi
fi
# 备份并用宿主 DNS 临时覆盖（收尾时还原）
if [ ! -f /root/ttbox-image/resolv.conf.orig ]; then
    cp -a "$IMG_ROOT/etc/resolv.conf" /root/ttbox-image/resolv.conf.orig 2>/dev/null || true
fi

echo
echo "== 5. 挂载结果 =="
mount | grep -E "on ${IMG_ROOT}" | sed 's/^/  /'
echo
echo "loop 设备文件: $LOOPFILE -> $(cat "$LOOPFILE")"
echo "根分区挂载点: $IMG_ROOT"
