#!/usr/bin/env bash
# ttbox.sh — TTBOX 运维顶层入口（D11，2026-09-18 定案 O14/I-27）
#
# 一站式转发既有脚本；所有子命令都可在板端 root 下直接执行。
#
# ★ 分发服务器地址（单点纪律）：OTA_SERVER_URL 写死在这里，必须与
#   C++ 侧 core/src/web/domain/ota_query.cpp::ota_server_url() 的默认值 **同值**
#   ——改一处必须同步改另一处；`ttbox.sh doctor` 会从 ttbox_web 二进制提取地址比对漂移。
#   （V1.0.52 前对照物是 plugins/web/bin/ttbox-web.py，该文件已随 Python 后端移除。）
#   正式服务器（2026-09-27 起）：**IP 直连** https://47.104.18.178:10086/ota。
#   2026-09-27 实锤：业主移动宽带按「域名家族+IP」过滤 cctv2.top/xiaotian.help
#   （80 端口回 ICP 拦截页、TLS 全端口掐真域名 SNI），域名访问必须等 ICP 备案通过；
#   IP 直连不发 SNI 可绕过。服务器侧已有 LE IP 证书（shortlived，acme.sh 自动续），
#   nginx 10086 default_server 块接管无 SNI 握手。备案通过后可切回域名。
#   地址必须带端口。
OTA_SERVER_URL="https://47.104.18.178:10086/ota"

set -euo pipefail

CUR="${TTBOX_CURRENT:-/opt/ttbox/current}"
SCRIPTS="${CUR}/scripts"
STATE_DIR="${TTBOX_STATE:-/opt/ttbox/state}"

log()  { printf '[ttbox] %s\n' "$*"; }
warn() { printf '[ttbox][warn] %s\n' "$*" >&2; }
die()  { printf '[ttbox][FATAL] %s\n' "$*" >&2; exit 1; }

require_current() {
    [ -x "${SCRIPTS}/ttbox_release_install.sh" ] || die "release 树不可用（${SCRIPTS}）——先安装出货包"
}

cmd_install() {
    shift || true
    require_current
    log "首次安装/升级（转发 ttbox_fhs_init.sh，参数：$*）"
    exec "${SCRIPTS}/ttbox_fhs_init.sh" "$@"
}

cmd_status() {
    log "== 服务状态 =="
    systemctl --no-pager --full status ttbox-core.service ttbox-web.service \
        ttbox-usbproxy.service 2>&1 | grep -E '●|Active:' || true
    log "== current 版本 =="
    readlink -f /opt/ttbox/current 2>/dev/null || warn "current 断链"
    log "== OTA 状态 =="
    cat "${STATE_DIR}/ota_status.json" 2>/dev/null || echo "（从未更新）"
    log "== 磁盘 =="
    df -h /opt/ttbox 2>/dev/null || true
}

cmd_upgrade() {
    shift || true
    require_current
    local url="${1:-}"
    if [ -z "$url" ]; then
        # 未给 URL：先查服务器取最新包地址（与 /api/update/check 同一契约）
        url="$(python3 - "$OTA_SERVER_URL" << 'PYEOF'
import json, sys, urllib.request
base = sys.argv[1].rstrip('/')
try:
    with urllib.request.urlopen(base + '/latest', timeout=6) as r:
        doc = json.loads(r.read().decode('utf-8', 'replace'))
    print(doc.get('package_url') or '')
except Exception as e:
    sys.stderr.write(f'查询分发服务器失败: {e!r}\n')
PYEOF
)"
        [ -n "$url" ] || die "未能从分发服务器（${OTA_SERVER_URL}）取得包地址"
    fi
    local jobs="/var/lib/ttbox/ota/jobs"
    mkdir -p "$jobs"; chown root:ttbox "$jobs" 2>/dev/null || true; chmod 0770 "$jobs" 2>/dev/null || true
    printf '{"url":"%s","key_id":"ttbox-ota-2026b","enqueued_at":%s}\n' \
        "$url" "$(date +%s)" > "${jobs}/job-cli-$(date +%s).json"
    log "任务文件已写入（root 更新器经 ttbox-ota.path 拉起）；跟踪状态：cat ${STATE_DIR}/ota_status.json"
}

cmd_rollback() {
    shift || true
    require_current
    exec "${SCRIPTS}/ttbox_release_install.sh" --rollback "$@"
}

cmd_backup() {
    shift || true
    exec "${SCRIPTS}/ttbox_backup.sh" "$@"
}

cmd_restore() {
    shift || true
    exec "${SCRIPTS}/ttbox_restore.sh" "$@"
}

cmd_uninstall() {
    shift || true
    exec "${SCRIPTS}/ttbox_uninstall.sh" "$@"
}

cmd_doctor() {
    shift || true
    exec "${SCRIPTS}/ttbox_doctor.sh" "$@"
}

usage() {
    cat << 'USAGE'
用法: ttbox.sh <子命令> [参数]

  install [ver]      首次安装/整包部署（转发 ttbox_fhs_init.sh）
  status             服务/版本/OTA/磁盘 一览
  upgrade [url]      升级：不带 url 则向写死的分发服务器查最新包；写任务文件触发 root 更新器
  rollback [ver]     回滚到上一版本或指定版本（转发 ttbox_release_install.sh --rollback）
  backup             备份 /etc/ttbox /var/lib/ttbox /opt/ttbox/releases（含 sha256 清单）
  restore <tgz>      从备份恢复（逐项 sha256 校验后解开）
  uninstall          卸载 TTBOX（清 /opt/ttbox /etc/ttbox /var/lib/ttbox 等，见脚本内清单）
  doctor             一键诊断（断链/服务/依赖/OTA 配置漂移）

服务器地址写死在本脚本头部 OTA_SERVER_URL（与 ttbox-web.py 同值，doctor 校验）。
USAGE
}

case "${1:-}" in
    install)  cmd_install "$@" ;;
    status)   cmd_status "$@" ;;
    upgrade)  cmd_upgrade "$@" ;;
    rollback) cmd_rollback "$@" ;;
    backup)   cmd_backup "$@" ;;
    restore)  cmd_restore "$@" ;;
    uninstall) cmd_uninstall "$@" ;;
    doctor)   cmd_doctor "$@" ;;
    -h|--help|help|"") usage ;;
    *) usage; die "未知子命令: $1" ;;
esac
