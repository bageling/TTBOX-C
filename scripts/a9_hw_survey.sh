#!/bin/bash
# A9 硬件只读调查脚本
echo "===== USB controller drivers ====="
# 控制器名不写死（P8）：列举所有 *.usb 平台设备，不硬编码 fc000000/fc400000/… 一套
for c in /sys/bus/platform/devices/*.usb; do
  [ -e "$c" ] || continue
  echo "-- $(basename "$c") --"
  grep -E 'DRIVER|OF_NAME|OF_FULLNAME' "$c/uevent" 2>/dev/null
  ls "$c" 2>/dev/null | head -12
done

echo "===== usbdrd3_* ====="
for c in /sys/bus/platform/devices/usbdrd3_*; do
  [ -e "$c" ] || continue
  echo "-- $(basename "$c") --"
  ls "$c" 2>&1 | head
  grep DRIVER "$c/uevent" 2>/dev/null
done

echo "===== typec ====="
ls /sys/class/typec 2>&1
ls /sys/class/typec_port 2>&1

echo "===== dwc3 控制器 subdir ====="
ls /sys/bus/platform/drivers/dwc3 2>&1

echo "===== 内核配置 ====="
if [ -f /proc/config.gz ]; then
  zcat /proc/config.gz 2>/dev/null | grep -E 'CONFIG_USB_GADGET|CONFIG_USB_CONFIGFS|CONFIG_USB_F_HID|CONFIG_USB_ROLE_SWITCH|CONFIG_HIDRAW|CONFIG_USB_HID|CONFIG_USB_DWC3|CONFIG_USB_F_|CONFIG_USB_LIBCOMPOSITE' | head -40
else
  grep -E 'CONFIG_USB_GADGET|CONFIG_USB_CONFIGFS|CONFIG_USB_F_HID|CONFIG_USB_ROLE_SWITCH|CONFIG_HIDRAW|CONFIG_USB_HID' /boot/config-* 2>/dev/null | head -40
  echo "--- 无 /proc/config.gz 且无 /boot/config ---"
fi

echo "===== 模块（可加载）====="
modinfo usb_f_hid 2>&1 | head -3
modinfo usb_f_hid_keyboard 2>&1 | head -3
modinfo dwc3 2>&1 | head -3
modinfo hid_generic 2>&1 | head -3
modinfo hidraw 2>&1 | head -3

echo "===== /proc/interrupts (USB/NPU) ====="
grep -iE 'usb|dwc|npu|rga|hdmi' /proc/interrupts 2>/dev/null | head -20

echo "===== USB 总线拓扑 ====="
for bus in usb1 usb2 usb3 usb4 usb5 usb6; do
  echo "-- $bus --"
  cat /sys/bus/usb/devices/$bus/version 2>/dev/null
  cat /sys/bus/usb/devices/$bus/devnum 2>/dev/null
done

echo "===== configfs 挂载 ====="
mount | grep configfs 2>/dev/null
ls /sys/kernel/config 2>&1 | head

echo "===== 当前 HID 相关 input (含 USB) ====="
grep -A5 -iE 'usb|hid' /proc/bus/input/devices 2>/dev/null | head -40
