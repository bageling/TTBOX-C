#!/usr/bin/env bash
# 只读抽查：出厂镜像的**联网能力**（激活要联网，没网 = 激活页连不上服务器）
set -u
IMG="${IMG:-${1:-}}"
[ -n "$IMG" ] && [ -f "$IMG" ] || { echo "需指定镜像：IMG=<xxx.img> 或第一个参数" >&2; exit 1; }
M="${M:-/mnt/final}"
mkdir -p "$M"
LOOP="$(losetup --find --show --partscan "$IMG")"
trap 'umount "$M" 2>/dev/null; rmdir "$M" 2>/dev/null; losetup -d "$LOOP" 2>/dev/null' EXIT
echo "loop=$LOOP"
sleep 1
mount -o ro "${LOOP}p2" "$M"

echo
echo "== 1. netplan 配置 =="
ls -l "$M/etc/netplan/" 2>&1
cat "$M/etc/netplan/01-ttbox.yaml" 2>&1 | sed 's/^/  /'

echo
echo "== 2. 网络栈谁在跑（enable 态 = .wants 落链）=="
for u in systemd-networkd.service systemd-resolved.service NetworkManager.service \
         systemd-networkd-wait-online.service; do
    if [ -e "$M/etc/systemd/system/multi-user.target.wants/$u" ] ||
       [ -e "$M/etc/systemd/system/network-online.target.wants/$u" ] ||
       [ -e "$M/etc/systemd/system/sysinit.target.wants/$u" ]; then
        echo "  [enable] $u"
    else
        echo "  [  --  ] $u  ← 未 enable"
    fi
done
echo "  -- multi-user.target.wants 全清单 --"
ls "$M/etc/systemd/system/multi-user.target.wants/" 2>/dev/null | sed 's/^/     /'

echo
echo "== 3. DNS（能不能解析 cctv2.top）=="
ls -l "$M/etc/resolv.conf" 2>&1
if [ -f "$M/etc/resolv.conf" ] && [ ! -L "$M/etc/resolv.conf" ]; then
    sed 's/^/  /' "$M/etc/resolv.conf"
fi
echo "  systemd-resolved 是否装了: $([ -f "$M/lib/systemd/system/systemd-resolved.service" ] && echo yes || echo NO)"

echo
echo "== 4. 有没有别的网络管理器在抢 =="
ls "$M/etc/systemd/system/" 2>/dev/null | grep -iE 'network|netplan|connman|wicd' | sed 's/^/  /'

echo
echo "== 5. 出厂是否留了 cloud-init 的 netplan 残留 =="
ls -l "$M/etc/netplan/" "$M/run/netplan/" 2>/dev/null | sed 's/^/  /'
