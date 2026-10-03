#!/bin/bash
# a9_setup_hid_gadget.sh — 建立 USB HID Gadget（A9）
#
# 架构：TTBox USB-C (usbdrd3_0, dwc3-gadget) → 主机
#   ttbox-hid: 3 接口，克隆真实罗技 c53f 结构
#     hid.usb0: 键盘（boot，8B 报告）
#     hid.usb1: 鼠标+consumer+system（report，多 ReportID）
#     hid.usb2: HID++ vendor 接口（G HUB 识别 G304 必需，双向透传）
# 主机无需专用驱动（标准 HID）。
#
# 用法: bash a9_setup_hid_gadget.sh [enable|disable|status]
# 环境变量: A9_NO_CLONE=1 保持默认身份（不克隆真实设备）
set -e

GADGET_DIR=/sys/kernel/config/usb_gadget
GADGET=ttbox-hid
# UDC 名不写死硬件编号（P8）：优先 USB_PROXY_DEVICE（与 usbproxy 启动脚本
# usbproxy/board/run-ttbox-usb-proxy.sh 同一覆盖链，登记于
# docs/protocols/config-path-env-registry.md），否则取 /sys/class/udc 里名字排序首个。
# 编号由内核枚举顺序决定，不是板子身份 —— 换内核 / 加控制器就可能移位。
UDC=${USB_PROXY_DEVICE:-$(ls /sys/class/udc 2>/dev/null | sort | head -n1)}
if [ -z "$UDC" ]; then
  echo "[A9] 找不到 USB Device Controller（/sys/class/udc 为空）" >&2
  echo "     确认 dwc3 已加载、控制器已切到 device 模式；或用 USB_PROXY_DEVICE=<名> 显式指定" >&2
  exit 1
fi

# 真实 c53f 各接口 report descriptor（从板端 hidraw ioctl 读取）：
#   键盘 59B：标准 boot 键盘
#   鼠标 148B：ReportID 0x02 鼠标(9B) + 0x03 consumer(3B) + 0x04 system(2B)
#   HID++ 98B：vendor usage page，ReportID 0x10 短消息(7B) + 0x11 长消息(20B) + 0x20(15B) + 0x21(32B)
KEYBOARD_DESC_HEX=05010906a101050719e029e71500250175019508810281039505050819012905910295017503910195067508150026ff00050719002aff008100c0
MOUSE_DESC_HEX=05010902a10185020901a100951075011500250105091901291081029502751016018026ff7f0501093009318106950175081581257f093881069501050c0a38028106c0c0050c0901a101850395027510150126ff0219012aff028100c005010980a10185049501750215012503098209810983810075068103c006bcff0988a101850895017508150126ff00190129ff8100c0
HIDPP_DESC_HEX=0600ff0901a101851095067508150026ff000901810009019100c00600ff0902a101851195137508150026ff000902810009029100c00600ff0904a10185207508950e150026ff0009418100094191008521951f150026ff000942810009429100c0

# 把 hex 字符串写入 configfs report_desc（Python 写入避免 printf 截断）
write_desc() {  # $1=路径 $2=hex
  python3 - "$1" "$2" <<'PYEOF'
import sys
path, hexstr = sys.argv[1], sys.argv[2]
with open(path, 'wb') as f:
    f.write(bytes.fromhex(hexstr))
print("desc written", len(bytes.fromhex(hexstr)), "bytes ->", path)
PYEOF
}

# 克隆真实键鼠身份（VID/PID/制造商/产品名/序列号），让主机侧显示为真实设备。
# 三个字符串必须全部非空：serialnumber 为空（iSerialNumber 指向空字符串）
# 会让主机枚举失败停在 addressed。实测 VID/PID 可正常伪装（须配完整字符串）。
clone_usb_identity() {
  for d in /sys/bus/usb/devices/*/; do
    [ -f "$d/idVendor" ] || continue
    v=$(cat "$d/idVendor" 2>/dev/null || true)
    if [ "$v" = "1d6b" ]; then continue; fi   # 跳过 Linux root hub / 自身 gadget
    # 仅克隆带 HID 接口的真实设备（键鼠）— 接口目录形如 <dev>/<dev>:1.0
    has_hid=0
    for i in "$d"*:1.*/; do
      [ -f "$i/bInterfaceClass" ] || continue
      if [ "$(cat "$i/bInterfaceClass" 2>/dev/null || true)" = "03" ]; then has_hid=1; fi
    done
    if [ "$has_hid" != "1" ]; then continue; fi
    p=$(cat "$d/idProduct" 2>/dev/null || true)
    m=$(cat "$d/manufacturer" 2>/dev/null || true)
    pr=$(cat "$d/product" 2>/dev/null || true)
    s=$(cat "$d/serial" 2>/dev/null || true)
    echo "[A9] 克隆设备身份: $v:$p [$m] $pr"
    echo "0x$v" > idVendor        # configfs 需 0x 前缀（sysfs 为裸 hex）
    if [ -n "$p" ]; then echo "0x$p" > idProduct; fi
    mkdir -p strings/0x409
    echo "${m:-}" > strings/0x409/manufacturer
    echo "${pr:-HID Device}" > strings/0x409/product
    echo "${s:-TTBOX}" > strings/0x409/serialnumber   # 必须非空
    return 0
  done
  echo "[A9] 未找到可克隆的 HID 设备，保持默认身份"
  return 1
}

do_enable() {
  echo "[A9] 建立 HID gadget: $GADGET (UDC=$UDC)"
  cd "$GADGET_DIR"
  if [ -d "$GADGET" ]; then
    echo "  [skip] $GADGET 已存在（幂等：确保 UDC 绑定）"
    echo "$UDC" > "$GADGET_DIR/$GADGET/UDC" 2>/dev/null || true
    exit 0
  fi
  mkdir "$GADGET" && cd "$GADGET"

  # 设备描述符（先默认，随后按真实键鼠克隆覆盖）
  echo 0x1d6b > idVendor          # Linux Foundation
  echo 0x0104 > idProduct         # Multifunction Composite Gadget
  echo 0x0100 > bcdDevice
  echo 0x0200 > bcdUSB            # USB 2.0
  echo 0xEF   > bDeviceClass      # Misc
  echo 0x02   > bDeviceSubClass
  echo 0x01   > bDeviceProtocol
  if [ -z "${A9_NO_CLONE:-}" ]; then
    clone_usb_identity
  else
    echo "[A9] A9_NO_CLONE=1，保持默认身份"
  fi

  # 配置
  mkdir -p configs/c.1/strings/0x409
  echo "TTBOX HID Forwarder" > configs/c.1/strings/0x409/configuration
  echo 500 > configs/c.1/MaxPower

  # ---- Keyboard (hid.usb0, boot protocol 1) ----
  mkdir -p functions/hid.usb0
  echo 1 > functions/hid.usb0/protocol       # boot keyboard
  echo 1 > functions/hid.usb0/subclass
  echo 8 > functions/hid.usb0/report_length  # 8 字节 boot keyboard report
  write_desc "$PWD/functions/hid.usb0/report_desc" "$KEYBOARD_DESC_HEX"
  ln -s functions/hid.usb0 configs/c.1/

  # ---- Mouse+consumer+system (hid.usb1, report protocol) ----
  mkdir -p functions/hid.usb1
  echo 0 > functions/hid.usb1/protocol       # report protocol
  echo 0 > functions/hid.usb1/subclass       # No Subclass（标准 report 协议）
  echo 9 > functions/hid.usb1/report_length  # 最大报告 9B（0x02 鼠标）
  write_desc "$PWD/functions/hid.usb1/report_desc" "$MOUSE_DESC_HEX"
  ln -s functions/hid.usb1 configs/c.1/

  # ---- HID++ vendor 接口 (hid.usb2, G HUB 识别 G304 必需) ----
  mkdir -p functions/hid.usb2
  echo 0 > functions/hid.usb2/protocol
  echo 0 > functions/hid.usb2/subclass
  echo 32 > functions/hid.usb2/report_length # 最大报告 32B（0x21 长消息）
  write_desc "$PWD/functions/hid.usb2/report_desc" "$HIDPP_DESC_HEX"
  ln -s functions/hid.usb2 configs/c.1/

  # 绑定 UDC（dwc3 切到 device 模式；USB-C 接主机后主机识别）
  echo "$UDC" > UDC
  echo "[OK] gadget 已绑定 UDC=$UDC"
  ls -la /dev/hidg* 2>/dev/null || echo "  (hidg 节点由主机枚举后出现)"
}

do_disable() {
  echo "[A9] 拆除 HID gadget: $GADGET"
  cd "$GADGET_DIR/$GADGET" 2>/dev/null || { echo "  未启用"; exit 0; }
  [ -f UDC ] && echo "" > UDC 2>/dev/null || true
  rm -f configs/c.1/hid.usb0 configs/c.1/hid.usb1 configs/c.1/hid.usb2
  rmdir functions/hid.usb0 functions/hid.usb1 functions/hid.usb2 2>/dev/null || true
  rmdir strings/0x409 strings 2>/dev/null || true
  rmdir configs/c.1/strings/0x409 configs/c.1 2>/dev/null || true
  cd "$GADGET_DIR"
  rmdir "$GADGET" 2>/dev/null || true
  echo "[OK] 已拆除"
}

do_status() {
  echo "== UDC =="
  ls /sys/class/udc/ 2>&1
  echo "== gadget =="
  ls -la "$GADGET_DIR/$GADGET" 2>&1 | head
  echo "== hidg =="
  ls -la /dev/hidg* 2>/dev/null || echo "  (hidg 未枚举：主机未连接 USB-C)"
  echo "== configfs =="
  ls "$GADGET_DIR" 2>&1
}

case "${1:-status}" in
  enable)  do_enable ;;
  disable) do_disable ;;
  status)  do_status ;;
  *) echo "usage: $0 enable|disable|status"; exit 1 ;;
esac
