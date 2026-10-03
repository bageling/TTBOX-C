#!/bin/bash
# A9: UDC / typec / gadget 状态只读检查
echo "===== /sys/class/udc ====="
ls -la /sys/class/udc/ 2>&1
for u in /sys/class/udc/*; do
  echo "-- $u --"
  cat $u/uevent 2>/dev/null
  cat $u/device/uevent 2>/dev/null | grep -E 'OF_FULLNAME'
done

echo "===== typec port0 ====="
ls /sys/class/typec/port0/ 2>&1
cat /sys/class/typec/port0/data_role 2>/dev/null
cat /sys/class/typec/port0/power_role 2>/dev/null
cat /sys/class/typec/port0/port_type 2>/dev/null
echo "-- partner --"
cat /sys/class/typec/port0-partner/data_role 2>/dev/null
cat /sys/class/typec/port0-partner/power_role 2>/dev/null

echo "===== dwc3 角色（extcon / typec）====="
# 控制器名不写死（P8）：列举所有 *.usb 平台设备，不再硬编码 fc000000.usb / fc400000.usb
for c in /sys/bus/platform/devices/*.usb; do
  [ -e "$c" ] || continue
  echo "-- $(basename "$c") --"
  ls "$c" 2>/dev/null | grep -iE 'role|extcon|connector'
done

echo "===== dwc3 debug: current role ====="
ls /sys/kernel/debug/usb/ 2>&1
for d in /sys/kernel/debug/usb/*; do
  echo "-- $d --"
  cat $d/current_role 2>/dev/null
  cat $d/mode 2>/dev/null
done

echo "===== configfs usb_gadget 现有 ====="
ls -la /sys/kernel/config/usb_gadget/ 2>&1

echo "===== usb1/usb2 是否 dwc3 host ====="
cat /sys/bus/usb/devices/usb1/device/uevent 2>/dev/null | grep -E 'DRIVER|OF_FULLNAME'
cat /sys/bus/usb/devices/usb2/device/uevent 2>/dev/null | grep -E 'DRIVER|OF_FULLNAME'
readlink /sys/bus/usb/devices/usb1 2>/dev/null
readlink /sys/bus/usb/devices/usb2 2>/dev/null
