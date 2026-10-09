#!/usr/bin/env bash
# 步骤 00 · chroot 环境自检：确认 arm64 用户态真的能跑，apt 源可达，磁盘够用
# 全绿方可进入后续步骤。
echo "===== 身份与环境 ====="
echo "chroot 内 whoami : $(whoami)"
echo "dpkg 架构        : $(dpkg --print-architecture)"
echo "内核架构(syscall): $(uname -m)   <- 宿主内核，qemu 下显示 x86_64 属正常"
echo "容器内 ostree    : $(cat /etc/os-release | sed -n 's/^PRETTY_NAME=//p')"
echo

echo "===== arm64 用户态可执行性 ====="
printf 'python3 --version  : '; python3 --version 2>&1
printf 'bash --version     : '; bash --version 2>&1 | head -1
printf '/bin/true 运行     : '; if /bin/true 2>/dev/null; then echo OK; else echo FAIL; fi
echo

echo "===== 关键工具是否齐备 ====="
for t in dpkg apt-get python3 systemctl useradd groupadd install tar readelf sha256sum; do
  printf '%-12s %s\n' "$t" "$(command -v "$t" 2>/dev/null || echo MISSING)"
done
echo

echo "===== DNS 与 apt 源连通性 ====="
cat /etc/resolv.conf 2>/dev/null | sed 's/^/  /'
echo "-- getent --"
for h in ports.ubuntu.com ppa.launchpadcontent.net; do
  printf '%-32s ' "$h"
  getent hosts "$h" >/dev/null 2>&1 && echo "OK" || echo "FAIL"
done
echo

echo "===== apt 源清单 ====="
grep -rhE '^(deb|URIs:)' /etc/apt/sources.list /etc/apt/sources.list.d/ 2>/dev/null | sed 's/^/  /'
echo

echo "===== 磁盘余量 ====="
df -h /
echo

echo "===== 现有 ttbox 痕迹（应为空）====="
echo "-- /opt --"; ls -la /opt/ 2>/dev/null
echo "-- /etc/ttbox --"; ls -la /etc/ttbox 2>/dev/null || echo "  (无)"
echo "-- /var/lib/ttbox --"; ls -la /var/lib/ttbox 2>/dev/null || echo "  (无)"
echo "-- ttbox 用户 --"; id ttbox 2>/dev/null || echo "  (无)"
echo

echo "===== 板级事实（决定后续步骤）====="
echo "-- 内核能力 --"
grep -E 'CONFIG_VIDEO_ROCKCHIP_HDMIRX=|CONFIG_USB_CONFIGFS_F_HID=|CONFIG_USB_RAW_GADGET=' /boot/config-* 2>/dev/null | sed 's/^/  /'
echo "-- hdmirx dtbo --"
ls -l /usr/lib/firmware/*/device-tree/rockchip/overlay/rk3588-hdmirx.dtbo 2>/dev/null | sed 's/^/  /' || echo "  (未找到)"
echo "-- netplan --"
ls -la /etc/netplan/ 2>/dev/null | sed 's/^/  /'
echo "-- cloud-init 开关 --"
ls -l /etc/cloud/cloud-init.disabled 2>/dev/null || echo "  (未禁用，首启会跑 cloud-init)"
echo "-- ssh 服务 --"
ls -l /etc/systemd/system/multi-user.target.wants/ssh.service /lib/systemd/system/ssh.service 2>/dev/null | sed 's/^/  /'
echo "-- 现有用户(uid>=1000) --"
awk -F: '$3>=1000 && $3<65000' /etc/passwd | sed 's/^/  /'
echo "-- 有密码的账号 --"
awk -F: '$2 ~ /^\$/ {print "  "$1}' /etc/shadow 2>/dev/null
echo "（空 = 镜像内没有任何密码，全靠 cloud-init 首次生成）"
