#!/bin/sh
# run-ttbox-usb-proxy.sh — TTBOX usb-proxy 启动脚本（自研）
#
# T1.06（DEP-04③）两处修复：
#   1）以【绝对路径】定位并 exec 二进制（`$PROJECT_DIR/usb-proxy`），不再 `exec ./usb-proxy`——
#      相对路径依赖调用者 cwd，systemd 未设 WorkingDirectory 或换目录启动时就找不到。
#   2）二进制缺失时【人话报错】并给出两条可复制的修复命令；绝不落到 shell 的裸 127
#      （"not found"）——旧写法删掉二进制后只有一行费解的 127，排障无从下手。
#
# T1.07（2026-09-21 板端实测）：find_mouse 的口径过窄导致 full 模式永久卡死。
#   旧实现只认 bInterfaceProtocol=02（HID boot 鼠标），而现代复合游戏鼠标走
#   report protocol（sub=00/proto=00）—— 实机 MCHOSE A7 V3 Pro+（3837:1014）
#   三个 HID 接口全是 00/00 ⇒ 永不匹配 ⇒ 无限"等鼠标"⇒ 不建 cmd.sock/event.sock、
#   gadget 不绑定、UDC 恒 not attached、core 的 PhysicalMouseReader 也连不上。
#   新实现改为三级判定（R1 proto=02 / R2 udev ID_INPUT_MOUSE / R3 rel 能力），
#   并在等待期每 10s 打一张接口诊断表。细节见下方 mouse_device_rule 注释。
set -eu

USB_PROXY_DEVICE=${USB_PROXY_DEVICE:-fc000000.usb}
USB_PROXY_DRIVER=${USB_PROXY_DRIVER:-dwc3-gadget}
USB_PROXY_WAIT_SECONDS=${USB_PROXY_WAIT_SECONDS:-1}
USB_PROXY_EXTRA_ARGS=${USB_PROXY_EXTRA_ARGS:-}
USB_PROXY_MODE=${USB_PROXY_MODE:-full}   # full | synthetic
USB_PROXY_SOCKET_DIR=${USB_PROXY_SOCKET_DIR:-/run/ttbox-mouse-passthrough}

SCRIPT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
PROJECT_DIR=$(CDPATH= cd -- "$SCRIPT_DIR/.." && pwd)
# 二进制与脚本同属 release 树的 usbproxy/ 目录（T1.01 布局：releases/<ver>/usbproxy/）。
USB_PROXY_BIN=${USB_PROXY_BIN:-$PROJECT_DIR/usb-proxy}

# ---- 预检：二进制存在且可执行，缺失则人话报错（绝不裸 127）-------------------
if [ ! -e "$USB_PROXY_BIN" ]; then
	cat >&2 <<EOF
Stopped: usb-proxy binary missing: $USB_PROXY_BIN
Reason: the prebuilt binary must ship inside the release tree (T1.01 layout: releases/<ver>/usbproxy/).
Fix (pick ONE):
  1) Re-publish a complete payload that contains usbproxy/usb-proxy, then activate:
       scripts/ttbox_release_install.sh <ver> <payload_dir> --activate
  2) Build it on the board (needs the dev libraries):
       apt-get install -y libusb-1.0-0-dev liblua5.4-dev libjsoncpp-dev g++ && cd $PROJECT_DIR && make
EOF
	exit 1
fi
if [ ! -x "$USB_PROXY_BIN" ]; then
	printf 'Stopped: usb-proxy is not executable: %s\n' "$USB_PROXY_BIN" >&2
	printf '  Fix: chmod +x "%s"\n' "$USB_PROXY_BIN" >&2
	exit 1
fi

# T1.09（2026-09-21 板端实测）：usb-proxy 动态依赖 libjsoncpp.so.25，而部分出厂镜像
# **没有装 libjsoncpp25**（板上 `ldd` 实录 `libjsoncpp.so.25 => not found`；
# `find / -name 'libjsoncpp*'` 为空）。该二进制是**预编译的、无 RPATH**
# （`readelf -d` 只有 NEEDED、没有 RPATH/RUNPATH），所以只要走到"启动二进制"这一步
# 就必然加载失败 ⇒ systemd（Restart=always）进入崩溃重启循环。
# OTA 只能替换 /opt/ttbox 下的 release 树、**改不了镜像 rootfs**，故只能随包自带：
# 本 release 的 usbproxy/lib/ 里放了该库，这里显式把该目录放到搜索路径**最前**
# （避免被外部同名的旧版本抢先命中）。注意目录是 $PROJECT_DIR/lib（= usbproxy/lib），
# 与二进制同目录，归属清晰；Makefile 亦已补 `-Wl,-rpath,$ORIGIN/lib` 使原生重建同样自洽。
if [ -d "$PROJECT_DIR/lib" ]; then
	LD_LIBRARY_PATH="$PROJECT_DIR/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
	export LD_LIBRARY_PATH
fi

# 启动前自检动态依赖：缺库时给【人话】错误 + 可复制的修复命令，而不是等 exec 抛
# 一句费解的 "error while loading shared libraries"（与 T1.06「绝不裸报错」同源）。
if command -v ldd >/dev/null 2>&1; then
	_missing="$(ldd "$USB_PROXY_BIN" 2>/dev/null | awk '/not found/{printf "%s ", $1}')"
	if [ -n "$_missing" ]; then
		cat >&2 <<EOF
Stopped: usb-proxy has unresolved shared libraries: $_missing
Reason: the prebuilt binary links them without RPATH; a stripped image may not ship them.
Check:  ls -l $PROJECT_DIR/lib/
Fix (pick ONE):
  1) Re-publish a complete payload that contains usbproxy/lib/ (e.g. libjsoncpp.so.25), then activate:
       scripts/ttbox_release_install.sh <ver> <payload_dir> --activate
  2) Install the library on the board:
       apt-get update && apt-get install -y libjsoncpp25
EOF
		exit 1
	fi
fi

stop_conflicting_services()
{
	# TTBOX 独占 UDC：把板端其它 USB 透传服务先停掉，避免和 raw-gadget 抢控制器。
	for unit in usb-proxy.service usb-proxy-test.service mouse-passthrough.service \
		opi-mouse-gadget.service usbdevice.service; do
		if systemctl is-active --quiet "$unit" 2>/dev/null; then
			systemctl stop "$unit" >/dev/null 2>&1 || true
			printf 'Stopped conflicting service %s\n' "$unit"
		fi
	done
}

# 输入节点(eventN)所属的 USB 设备 sysfs 真实路径。
# 必须【向上走到最近的 idVendor 目录再比相等】，不能拿设备路径做前缀匹配 ——
# Hub 是鼠标的祖先，前缀匹配会把父 Hub 也判成鼠标（2026-09-21 实机踩到）。
usb_dev_of_input()
{
	_d=$(readlink -f "$1/device" 2>/dev/null) || return 1
	while [ -n "$_d" ] && [ "$_d" != "/" ] && [ "$_d" != "/sys" ]; do
		if [ -f "$_d/idVendor" ] && [ -f "$_d/idProduct" ]; then
			readlink -f "$_d"
			return 0
		fi
		_d=$(dirname "$_d")
	done
	return 1
}

# 判定单个 USB 设备是否含「鼠标」，命中则打印命中的规则名。
# 三级口径（命中任一即算）：
#   R1 bInterfaceProtocol = 02 —— 传统 HID boot 鼠标（旧实现唯一口径，保留）
#   R2 udev ID_INPUT_MOUSE=1   —— 内核/udev 公认判定（权威，需 /dev/input/eventN）
#   R3 输入节点 rel 能力非 0    —— 兜底（无 udevadm / 无 udev 属性时）
#
# 为什么必须放宽（T1.07，2026-09-21 板端实测）：
#   现代复合游戏鼠标走 report protocol，HID 接口是 sub=00/proto=00，
#   实机 MCHOSE A7 V3 Pro+（3837:1014）三个 HID 接口全是 00/00。
#   旧实现只认 proto=02 ⇒ 永不匹配 ⇒ 无限 "Waiting for a USB HID mouse"
#   ⇒ 不建 cmd.sock/event.sock、gadget 不绑定、UDC 恒 not attached、
#   core 的 PhysicalMouseReader 也连不上（透传整体不可用）。
#
# 排除项：Hub / 根 Hub（bDeviceClass=09）。
# 不需要排除 TTBOX 自身 gadget：它挂在 UDC 上，不出现在本机 host 总线（lsusb 实证）。
mouse_device_rule()
{
	_dev="$1"
	_devreal=$(readlink -f "$_dev" 2>/dev/null) || return 1
	if [ "$(cat "$_dev/bDeviceClass" 2>/dev/null || printf '00')" = "09" ]; then
		return 1
	fi

	for intf in "$_dev":*; do
		[ -f "$intf/bInterfaceClass" ] || continue
		if [ "$(cat "$intf/bInterfaceClass" 2>/dev/null)" = "03" ] &&
			[ "$(cat "$intf/bInterfaceProtocol" 2>/dev/null)" = "02" ]; then
			printf 'R1'
			return 0
		fi
	done

	for _ev in /sys/class/input/event*; do
		[ -e "$_ev" ] || continue
		_own=$(usb_dev_of_input "$_ev") || continue
		[ "$_own" = "$_devreal" ] || continue

		if command -v udevadm >/dev/null 2>&1; then
			if udevadm info --query=property --name="/dev/input/$(basename "$_ev")" 2>/dev/null |
				grep -q '^ID_INPUT_MOUSE=1$'; then
				printf 'R2'
				return 0
			fi
		fi

		_rel=$(cat "$_ev/device/capabilities/rel" 2>/dev/null || printf '')
		if [ -n "$_rel" ] && [ "$_rel" != "0" ]; then
			printf 'R3'
			return 0
		fi
	done

	return 1
}

# find_mouse：回显 "<vid> <pid> <命中规则>"。第三字段仅供日志诊断，调用方只取前两个。
find_mouse()
{
	for dev in /sys/bus/usb/devices/*; do
		[ -f "$dev/idVendor" ] || continue
		[ -f "$dev/idProduct" ] || continue
		if _rule=$(mouse_device_rule "$dev"); then
			printf '%s %s %s\n' "$(cat "$dev/idVendor")" "$(cat "$dev/idProduct")" "$_rule"
			return 0
		fi
	done

	return 1
}

# 1.5.62：降级合成后继续盯物理鼠标，一旦出现就重启本服务，自动切回物理透传。
# 为什么需要：quirk 鼠标被打掉后重新枚举很慢（板端实测 12:34 掉线、13:44 才回来），
# 服务恰好在这段空窗里启动就会永久停在合成模式，用户看到的就是「鼠标不动」。
# 冷却 90s：设备反复上下线时不会打成重启循环（unit 限流 5 次/300s，90s 冷却只可能 3 次）。
mouse_auto_recover()
{
	[ "${USB_PROXY_MOUSE_AUTO_RECOVER:-1}" = "1" ] || return 0
	unit=${USB_PROXY_SERVICE_NAME:-ttbox-usbproxy}
	stamp=/run/ttbox-usbproxy-auto-recover.stamp
	(
		rounds=0
		while [ "$rounds" -lt 720 ]; do
			sleep 5
			rounds=$((rounds + 1))
			ids2=$(find_mouse 2>/dev/null) || continue
			now=$(date +%s)
			last=$(cat "$stamp" 2>/dev/null || printf '0')
			[ $((now - last)) -ge 90 ] || continue
			printf '%s\n' "$now" >"$stamp" 2>/dev/null
			printf '[auto-recover] 检测到物理鼠标 %s，重启 %s 切回物理透传\n' "$ids2" "$unit"
			systemctl restart "$unit"
			exit 0
		done
	) &
}

# 等待期诊断表：卡在"等鼠标"时，一眼看出是"没插"还是"插了但被判成非鼠标"。
mouse_scan_report()
{
	for dev in /sys/bus/usb/devices/*; do
		[ -f "$dev/idVendor" ] || continue
		[ -f "$dev/idProduct" ] || continue
		printf '  [scan] %s %s:%s devclass=%s "%s"' \
			"$(basename "$dev")" "$(cat "$dev/idVendor")" "$(cat "$dev/idProduct")" \
			"$(cat "$dev/bDeviceClass" 2>/dev/null || printf '00')" \
			"$(cat "$dev/product" 2>/dev/null || printf '?')"
		for intf in "$dev":*; do
			[ -f "$intf/bInterfaceClass" ] || continue
			printf ' | %s c=%s s=%s p=%s' "$(basename "$intf")" \
				"$(cat "$intf/bInterfaceClass")" \
				"$(cat "$intf/bInterfaceSubClass")" \
				"$(cat "$intf/bInterfaceProtocol")"
		done
		if _r=$(mouse_device_rule "$dev"); then
			printf ' => MOUSE(%s)\n' "$_r"
		else
			printf ' => -\n'
		fi
	done
}

cd "$PROJECT_DIR"
mkdir -p "$USB_PROXY_SOCKET_DIR"
stop_conflicting_services

# 1.5.26(c)-1（并入 T1.07 版，2026-09-22）：等 UDC 加超时。旧写法（含 09-21 发布的 T1.07 版）
# 是无限死等——只能靠 systemd 超时杀，日志看不出为什么。现在超时即人话报错 + 退出，
# 交 systemd 重启 + unit 的 StartLimit 兑底。
UDC_WAIT=${USB_PROXY_UDC_WAIT_SECONDS:-60}
_waited=0
while [ ! -e "/sys/class/udc/$USB_PROXY_DEVICE" ]; do
	if [ "$_waited" -ge "$UDC_WAIT" ]; then
		printf 'Stopped: USB device controller %s 在 %ss 内未出现。\n' \
			"$USB_PROXY_DEVICE" "$UDC_WAIT" >&2
		printf '  预期驱动 %s 已加载、且该 UDC 未被其它服务占用。查：\n' "$USB_PROXY_DRIVER" >&2
		printf '    ls /sys/class/udc/ ; systemctl status ttbox-usbproxy\n' >&2
		exit 1
	fi
	printf 'Waiting for USB device controller %s... (%ss/%ss)\n' \
		"$USB_PROXY_DEVICE" "$_waited" "$UDC_WAIT"
	sleep "$USB_PROXY_WAIT_SECONDS"
	_waited=$((_waited + USB_PROXY_WAIT_SECONDS))
done

ARGS="--device=$USB_PROXY_DEVICE --driver=$USB_PROXY_DRIVER"
ARGS="$ARGS --mouse_control_cmd_socket=$USB_PROXY_SOCKET_DIR/cmd.sock"
ARGS="$ARGS --mouse_control_event_socket=$USB_PROXY_SOCKET_DIR/event.sock"

ids=""
if [ "$USB_PROXY_MODE" != "synthetic" ]; then
	# 1.5.26(c)-2（并入 T1.07 版，2026-09-22）：full 模式找物理鼠标加超时降级。旧写法（含 09-21 发布的
	# T1.07 版）是无限死等——非鼠标环境 = usb-proxy 永不启动、电脑侧看不到鼠标。
	# 现在超时后降级 synthetic：AI 注入可用、物理透传不可用，插回鼠标 restart 即恢复。
	# 1.5.62：30s → 90s。30s 太短：quirk 鼠标重新枚举常要 40s 以上，
	# 服务一错过窗口就永久降级，看着就像「鼠标坏了」。
	MOUSE_WAIT=${USB_PROXY_MOUSE_WAIT_SECONDS:-90}
	_waited=0
	while ! ids=$(find_mouse); do
		ids=""
		if [ "$_waited" -ge "$MOUSE_WAIT" ]; then
			printf 'WARN: %ss 内未找到物理 HID 鼠标，降级为合成鼠标模式。\n' "$MOUSE_WAIT" >&2
			printf '      合成模式下 AI 注入可用、物理鼠标透传不可用。\n' >&2
			printf '      已开启自动接管：物理鼠标一插上/一回来会自动重启本服务切回物理透传。\n' >&2
			mouse_auto_recover
			USB_PROXY_MODE=synthetic
			break
		fi
		printf 'Waiting for a USB HID mouse on the Orange Pi side... (%ss/%ss)\n' \
			"$_waited" "$MOUSE_WAIT"
		# 每 10 次（默认 10s）打一张全量接口表：
		# 区分「没插鼠标」与「插了但被判成非鼠标」——后者是旧实现的经典故障形态。
		if [ "$((_waited % 10))" = 0 ]; then
			mouse_scan_report
		fi
		sleep "$USB_PROXY_WAIT_SECONDS"
		_waited=$((_waited + USB_PROXY_WAIT_SECONDS))
	done
fi

if [ "$USB_PROXY_MODE" = "synthetic" ]; then
	printf 'TTBOX usb-proxy synthetic mode\n'
	ARGS="$ARGS --synthetic_mouse --enable_mouse_control"
else
	set -- $ids
	vendor_id=$1
	product_id=$2
	rule=${3:-?}

	printf 'Using USB mouse %s:%s (matched by %s)\n' "$vendor_id" "$product_id" "$rule"
	ARGS="$ARGS --vendor_id=$vendor_id --product_id=$product_id --hid_passthrough_compat --enable_mouse_control"
fi

# shellcheck disable=SC2086
exec "$USB_PROXY_BIN" $ARGS $USB_PROXY_EXTRA_ARGS
