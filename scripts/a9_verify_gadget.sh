#!/bin/bash
# a9_verify_gadget.sh — 验证 HID gadget 描述符/端点
echo "== report_desc 字节数（键盘应 63，鼠标应 39）=="
for f in /sys/kernel/config/usb_gadget/ttbox-hid/functions/hid.usb*; do
  echo "-- $f --"
  echo "protocol=$(cat $f/protocol) subclass=$(cat $f/subclass) report_length=$(cat $f/report_length)"
  echo "desc_bytes=$(wc -c < $f/report_desc)"
  cat $f/report_desc | xxd | head -5
done

echo "== hidg 设备 ==="
ls -la /dev/hidg* 2>&1

# UDC 名不写死硬件编号（P8）：优先 USB_PROXY_DEVICE，否则 /sys/class/udc 排序首个
UDC=${USB_PROXY_DEVICE:-$(ls /sys/class/udc 2>/dev/null | sort | head -n1)}

echo "== UDC 状态 =="
if [ -z "$UDC" ]; then
  echo "  (无 UDC：/sys/class/udc 为空)"
else
  echo "  udc: $UDC"
  cat "/sys/class/udc/$UDC/uevent" 2>/dev/null
  cat "/sys/class/udc/$UDC/state" 2>/dev/null
  cat "/sys/class/udc/$UDC/device/current_role" 2>/dev/null

  echo "== dwc3 角色 sysfs =="
  ls "/sys/bus/platform/devices/$UDC/usb_role/" 2>&1
  cat "/sys/bus/platform/devices/$UDC/usb_role/"*/role 2>&1
fi

echo "== typec port0 =="
cat /sys/class/typec/port0/data_role 2>/dev/null
cat /sys/class/typec/port0/power_role 2>/dev/null
ls /sys/class/typec/port0/port0.0/ 2>/dev/null | head
cat /sys/class/typec/port0/port0.0/uevent 2>/dev/null | grep -E 'PRODUCT|DEVICE' | head

echo "== 总线枚举（gadget 接主机前为空）=="
lsusb 2>&1

echo "== 写测试：向 hidg0 发一个空键盘报告（无主机枚举会 EAGAIN/阻塞？只测 open）=="
timeout 1 cat /dev/hidg0 2>&1 | head -1
