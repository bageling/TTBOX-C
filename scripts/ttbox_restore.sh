#!/usr/bin/env bash
# ttbox_restore.sh — TTBOX 恢复（D09，2026-09-18 定案 O14/I-29）
#
# 从 ttbox_backup.sh 产出的 <tgz> 恢复三处数据：
#   1) 校验备份包自身 sha256（.sha256 文件存在时）
#   2) 解包到暂存目录，MANIFEST.sha256 逐项复验（防止备份介质损坏悄悄恢复半套数据）
#   3) 逐项目标 cp -a 覆盖回原位（/etc/ttbox /var/lib/ttbox /opt/ttbox/releases）
#
# 用法: ttbox_restore.sh <backup.tgz>
set -euo pipefail

log()  { printf '[restore] %s\n' "$*"; }
die()  { printf '[restore][FATAL] %s\n' "$*" >&2; exit 1; }

[ "$(id -u)" = "0" ] || die "请以 root 运行"
[ $# -ge 1 ] || { echo "用法: ttbox_restore.sh <backup.tgz>"; exit 2; }
BK="$1"
[ -f "$BK" ] || die "备份包不存在: $BK"

# 1) 包完整性
if [ -f "${BK}.sha256" ]; then
    sha256sum -c "${BK}.sha256" >/dev/null 2>&1 || die "备份包 sha256 校验失败（介质损坏或被篡改）"
    log "备份包 sha256 校验通过"
fi

STAGE="$(mktemp -d /var/tmp/ttbox-restore-XXXXXX)"
trap 'rm -rf "$STAGE"' EXIT
tar -C "$STAGE" -xzf "$BK"
[ -f "${STAGE}/MANIFEST.sha256" ] || die "备份包缺 MANIFEST.sha256（不是本工具产出的备份）"

# 2) 内容完整性
( cd "$STAGE/data" && sha256sum -c ../MANIFEST.sha256 --quiet ) \
    || die "备份内容 sha256 复验失败——备份不完整，拒绝恢复"

# 3) 逐项恢复
log "停服务后恢复（避免运行中写入与恢复内容打架）"
systemctl stop ttbox-core ttbox-web ttbox-usbproxy 2>/dev/null || true
for rel in etc/ttbox var/lib/ttbox opt/ttbox/releases; do
    if [ -d "${STAGE}/data/${rel}" ]; then
        log "恢复 /${rel}"
        mkdir -p "/$(dirname "$rel")"
        rm -rf "/${rel}"
        cp -a "${STAGE}/data/${rel}" "/${rel}"
    fi
done
systemctl daemon-reload 2>/dev/null || true
systemctl start ttbox-core ttbox-web ttbox-usbproxy 2>/dev/null || true

log "恢复完成。健康检查：systemctl --failed；或 ttbox.sh doctor"
