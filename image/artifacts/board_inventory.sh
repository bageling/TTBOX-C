#!/usr/bin/env bash
# 板端系统级清单：找出所有"镜像里没有、但 TTBOX 依赖"的机制
echo "########## 1. 所有引用 ttbox 的 systemd 单元 ##########"
grep -rl 'ttbox' /etc/systemd/system/ /lib/systemd/system/ 2>/dev/null | sort | while read -r f; do
  echo "--- $f"
  grep -nE 'ExecStart|ExecStartPre|User=|Group=|WantedBy|Environment' "$f" 2>/dev/null | sed 's/^/      /'
done

echo
echo "########## 2. /etc/systemd/system 全量（找非 unit 的定制文件）##########"
find /etc/systemd/system -maxdepth 1 -type f | sort

echo
echo "########## 3. drop-in 目录 ##########"
find /etc/systemd/system -maxdepth 2 -name '*.d' -type d 2>/dev/null | sort | while read -r d; do
  echo "--- $d"; ls -la "$d" | sed 's/^/      /'
done

echo
echo "########## 4. udev 规则 ##########"
echo "-- /etc/udev/rules.d --"; ls -la /etc/udev/rules.d/ 2>/dev/null
echo "-- 内容里提到 hdmirx/ttbox/rga/rknpu/video 的 --"
grep -rlnE 'hdmirx|ttbox|rga|rknpu' /etc/udev/rules.d/ /lib/udev/rules.d/ 2>/dev/null | sort

echo
echo "########## 5. modules-load / modprobe 配置 ##########"
echo "-- /etc/modules-load.d --"; ls -la /etc/modules-load.d/ 2>/dev/null
for f in /etc/modules-load.d/*.conf; do [ -f "$f" ] && { echo "--- $f"; cat "$f" | sed 's/^/      /'; }; done
echo "-- /etc/modprobe.d 提到 raw_gadget/dwc3 --"
grep -rlE 'raw_gadget|dwc3' /etc/modprobe.d/ 2>/dev/null

echo
echo "########## 6. cron / timer ##########"
ls -la /etc/cron.d/ 2>/dev/null
systemctl list-timers --all --no-pager 2>/dev/null | head -20

echo
echo "########## 7. /etc/rc.local ##########"
ls -la /etc/rc.local 2>/dev/null || echo "  (无)"

echo
echo "########## 8. sysctl / 其他 /etc 定制 ##########"
ls -la /etc/sysctl.d/ 2>/dev/null
echo "-- /etc/default 里 ttbox 相关 --"
grep -rl ttbox /etc/default/ 2>/dev/null || echo "  (无)"

echo
echo "########## 9. /etc 下所有 mtime 晚于镜像日期的文件（= 后来加的）##########"
find /etc -newermt '2024-11-01' -type f 2>/dev/null | grep -vE '^/etc/(ssl|ssh/ssh_host|machine-id|shadow|passwd|group|gshadow|subgid|subuid|sudoers|resolv|hosts|hostname|ld.so.cache|apt|dpkg|alternatives|cron.d/.+~|systemd/system/multi-user.target.wants/ttbox)' | sort | head -60

echo
echo "########## 10. 当前运行的服务状态 ##########"
systemctl is-active ttbox-core ttbox-web ttbox-usbproxy ttbox-edid ttbox-ota.path ttbox-ota.service ttbox-ensure.timer 2>&1 | paste -d' ' <(echo "core web usbproxy edid ota.path ota.service ensure.timer" | tr ' ' '\n') -

echo
echo "########## 11. hdmirx sysfs 节点当前权限 ##########"
for p in /sys/class/hdmirx/hdmirx /sys/devices/platform/fdee0000.hdmirx-controller/hdmirx/hdmirx; do
  [ -d "$p" ] && { echo "--- $p"; ls -l "$p" 2>/dev/null | sed 's/^/      /'; }
done

echo
echo "########## 12. 是否有 boot 时跑 fhs_init 的机制 ##########"
grep -rl 'fhs_init' /etc/ /lib/systemd/ 2>/dev/null || echo "  (没有开机跑 fhs_init 的机制)"
