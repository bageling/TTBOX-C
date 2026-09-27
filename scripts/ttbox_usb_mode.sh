#!/bin/sh
# ttbox_usb_mode.sh — USB 鼠标透传模式的【运维入口】（show | set full）
#
# ─────────────────────────────────────────────────────────────────────────────
# 1.5.62：合成模式已删除（业主 09-27 定案）。
#   usb-proxy 现在只有一条路——接物理鼠标做完整透传；找不到物理鼠标就一直等，
#   不再伪造一个虚拟鼠标。`USB_PROXY_MODE` 环境变量随之废弃：单元里已不再设置，
#   旧的 drop-in（ttbox-usbproxy.service.d/10-mode.conf）若是残留也会被忽略。
#
# 为什么还要这个脚本：
#   `show` 是排障第一手——一眼看出 usb-proxy 进程到底在不在、是不是在等鼠标；
#   `set full` 用来清掉历史 drop-in，避免旧配置让人误以为还有两种模式。
#
# 用法：
#   ttbox_usb_mode.sh show              # 打印当前模式 + 进程实际状态
#   sudo ttbox_usb_mode.sh set full     # 回到唯一模式（清掉历史 drop-in 并重启服务）
#
# 退出码：0 成功；1 用法/参数错；2 需要 root；3 systemctl 操作失败
set -eu

UNIT=ttbox-usbproxy
UNITS_DIR=${TTBOX_UNIT_DIR:-/etc/systemd/system}
SYSTEMD=${TTBOX_SYSTEMD:-1}
DROPIN_DIR="$UNITS_DIR/$UNIT.service.d"
DROPIN="$DROPIN_DIR/10-mode.conf"
# 自测钩子：放行非 root 写入（仅离线夹具；生产不得设置）。
TEST_MODE=${TTBOX_USB_MODE_TEST:-}
# 1.5.62 起只有这一种模式（合成模式已删除）。
ONLY_MODE=full

usage()
{
	cat <<EOF
用法:
  $0 show
  sudo $0 set full

说明: 1.5.62 起只有【物理透传】一种模式（合成模式已删除）。
      盒子必须插物理 USB 鼠标；没插时 usb-proxy 会一直等，不会伪造虚拟鼠标。
      show 会打印进程实际在跑的模式，以及是否在等鼠标。
EOF
}

# 进程实际跑的模式：命令行带 --vendor_id 即物理透传（与 web 端同一判据）。
effective_mode()
{
	for d in /proc/[0-9]*; do
		[ -r "$d/cmdline" ] || continue
		cmd=$(tr '\0' ' ' < "$d/cmdline" 2>/dev/null || true)
		case "$cmd" in
		*usb-proxy*) ;;
		*) continue ;;
		esac
		case "$cmd" in
		*--vendor_id*) printf 'full_passthrough\n'; return 0 ;;
		esac
	done
	return 0
}

do_show()
{
	eff=$(effective_mode)
	printf '模式(唯一): %s\n' "$ONLY_MODE"
	printf '进程实际值: %s\n' "${eff:-<未运行或在等鼠标>}"
	if [ -f "$DROPIN" ]; then
		printf '注意: 存在历史 drop-in %s（1.5.62 起 USB_PROXY_MODE 已废弃、会被忽略）。\n' "$DROPIN"
		printf '      清理: sudo %s set full\n' "$0"
	fi
	if [ -z "$eff" ]; then
		printf '排查: lsusb | grep -i mouse ; journalctl -u %s -n 50\n' "$UNIT"
	fi
}

do_set()
{
	mode=$1
	case "$mode" in
	full) ;;
	synthetic)
		printf '合成模式已在 1.5.62 删除：没有物理鼠标时不再伪造虚拟鼠标。\n' >&2
		printf '本版本只有 full（物理透传）一种模式。\n' >&2
		exit 1
		;;
	*)
		printf '模式必须是 full（收到: %s）\n' "$mode" >&2
		exit 1
		;;
	esac

	if [ "$(id -u)" != "0" ] && [ -z "$TEST_MODE" ]; then
		printf '需要 root：清理 drop-in 需写 %s 并重启 %s\n' "$DROPIN_DIR" "$UNIT" >&2
		printf '  sudo %s set %s\n' "$0" "$mode" >&2
		exit 2
	fi

	# 唯一模式 ⇒ drop-in 没有存在意义，删掉即可（幂等）。
	if [ -f "$DROPIN" ]; then
		rm -f "$DROPIN"
		printf '已删除历史 drop-in %s\n' "$DROPIN"
	else
		printf '无需清理：%s 不存在（已是唯一模式 full）\n' "$DROPIN"
	fi

	if [ "$SYSTEMD" = "0" ]; then
		printf '[自测] TTBOX_SYSTEMD=0，跳过 daemon-reload/restart\n'
	else
		if ! systemctl daemon-reload; then
			printf 'systemctl daemon-reload 失败\n' >&2
			exit 3
		fi
		if ! systemctl restart "$UNIT"; then
			printf 'systemctl restart %s 失败（看 journalctl -u %s）\n' "$UNIT" "$UNIT" >&2
			exit 3
		fi
	fi
	do_show
}

[ $# -ge 1 ] || { usage; exit 1; }
case "$1" in
show) do_show ;;
set)  [ $# -ge 2 ] || { usage; exit 1; }; do_set "$2" ;;
-h|--help|help) usage ;;
*) usage; exit 1 ;;
esac
