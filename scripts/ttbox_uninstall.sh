#!/usr/bin/env bash
# ttbox_uninstall.sh — TTBOX 卸载（D08，2026-09-18 定案 O14/I-28）
#
# 清单（逐项核对，缺什么跳什么）：
#   9 个 unit（含 ttbox-ensure.timer 与 ttbox-ota.path/service）+ unit 文件
#   /opt/ttbox  /etc/ttbox  /var/lib/ttbox  /var/log/ttbox  /run/ttbox
#   /run/ttbox-mouse-passthrough（usbproxy cmd.sock/event.sock 的 RuntimeDirectory）
#   /lib/firmware/ttbox（hdmirx_edid.bin，edid 服务装载）
#   ttbox 用户与组
#
# ⚠ 本脚本不可逆：授权（/var/lib/ttbox/license）、模型、配置全删。
#   卸载前如需保留数据，先跑 `ttbox.sh backup`。
# 用法: ttbox_uninstall.sh [--yes]   （不带 --yes 只打印将删除的清单，不执行）
set -euo pipefail

log()  { printf '[uninstall] %s\n' "$*"; }

UNITS=(
    ttbox-core.service ttbox-web.service
    ttbox-usbproxy.service ttbox-edid.service
    ttbox-ensure.service ttbox-ensure.timer
    ttbox-ota.path ttbox-ota.service
)
DIRS=(/opt/ttbox /etc/ttbox /var/lib/ttbox /var/log/ttbox /run/ttbox
      /run/ttbox-mouse-passthrough /lib/firmware/ttbox)

plan() {
    echo "== 将执行 =="
    for u in "${UNITS[@]}"; do echo "  disable --now  $u"; done
    for u in "${UNITS[@]}"; do echo "  rm -f          /etc/systemd/system/$u"; done
    for d in "${DIRS[@]}"; do echo "  rm -rf --one-file-system  $d"; done
    echo "  userdel/groupdel ttbox"
}

if [ "${1:-}" != "--yes" ]; then
    plan
    echo ""
    echo "这是演练（dry-run）。确认无误后执行: ttbox_uninstall.sh --yes"
    echo "⚠ 卸载不可逆：授权、模型、配置全删。需要保留数据先跑 ttbox.sh backup。"
    exit 0
fi

[ "$(id -u)" = "0" ] || { echo "请以 root 运行" >&2; exit 1; }

for u in "${UNITS[@]}"; do
    systemctl disable --now "$u" 2>/dev/null || true
done
systemctl daemon-reload 2>/dev/null || true
rm -f /etc/systemd/system/"${UNITS[@]}"

for d in "${DIRS[@]}"; do
    if [ -e "$d" ]; then
        log "删除 ${d}"
        rm -rf --one-file-system "$d"
    fi
done

# ttbox 用户/组（属主已无目录，可安全删除）
userdel ttbox 2>/dev/null && log "已删除用户 ttbox" || log "用户 ttbox 不存在，跳过"
groupdel ttbox 2>/dev/null && log "已删除组 ttbox" || log "组 ttbox 不存在，跳过"

log "卸载完成。核对残留: ls /opt/ttbox /etc/ttbox /var/lib/ttbox /var/log/ttbox /run/ttbox-mouse-passthrough /lib/firmware/ttbox 2>&1; systemctl list-units 'ttbox-*'; ls /etc/systemd/system/ttbox-* 2>&1"
