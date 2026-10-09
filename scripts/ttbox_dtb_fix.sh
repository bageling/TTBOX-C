#!/usr/bin/env bash
# ttbox_dtb_fix.sh —— 修复出厂 DTB 里 HDMI-RX 被禁用（status=disabled）
#
# 背景（2026-09-20 现场事故 / 2026-09-21 二次事故）：
#   出厂镜像的 rk3588-orangepi-5-plus.dtb 里 hdmirx-controller@fdee0000 的
#   status="disabled"，而 extlinux.conf 没有任何 fdtoverlays ⇒ rk3588-hdmirx.dtbo
#   从未被应用 ⇒ HDMI-RX 不 probe ⇒ /sys/class/hdmirx 与 /dev/video0 都不存在
#   ⇒ ①EDID 应用报「未找到可写 HDMI-RX HPD 节点」②HDMI 采集整体不可用。
#
# ★★ 1.5.22 / 1.5.23 为什么没修好（本次重写的直接原因）★★
#   那两版的搜索面**只有一个**：
#       find "${ROOT}/lib/firmware" -maxdepth 6 -type f -path '*/device-tree/rockchip/<name>'
#   它有两个硬缺陷，任一个都足以让"修复"变成空转：
#     ① **只认 /lib/firmware，完全不碰 /boot。** 而 u-boot 读的是 /boot 下那一份
#        （镜像构建侧的 04d_hdmirx_fix.sh 是 `find /lib/firmware /boot …` 两处都修，
#        两套脚本口径不一致）。
#     ② **`-path '*/device-tree/…'` 要求路径里含 device-tree 这一节，且 find 默认
#        （-P）不下降符号链接。** 板上若 device-tree 是软链，find 一条都匹配不到。
#        —— 台账「`find <软链>` 默认不下降」已记过一次，这是同一个坑。
#   本次改为：**按文件名在整个根下多锚点搜、跟随软链（-L）、按指纹门禁逐份判定**，
#   并把全过程写成报告落盘，避免再出现"装上了但不知道有没有生效"。
#
# ★ 三条安全约束（缺一不可）：
#   1. **指纹门禁**：只有当目标 DTB 的 sha256 精确等于已知的坏版本时才替换。
#      目标是好版本、或是任何不认识的版本 ⇒ 一律不动。绝不"看着像就换"。
#   2. **绝不影响安装结果**：恒退出 0（DTB 属于启动链，与运行树无关，
#      不允许因为它把一次 OTA 判失败）。
#   3. **写后立即校验**：替换完再算一次 sha256，不等于期望值就回滚原文件。
#
# ★ 生效时机：DTB 由 u-boot 在开机时读取 ⇒ 本脚本只换文件，**必须重启才生效**。
#
# 用法：
#   ttbox_dtb_fix.sh [目标根]       目标根默认 /（测试时可传假根，如 /tmp/fakeroot）
#
# 可用环境变量（全部有生产默认值，仅测试与诊断用）：
#   TTBOX_DTB_SRC / TTBOX_DTB_FIX_TEST / TTBOX_DTB_REPORT
#   TTBOX_DTB_GOOD_SHA / TTBOX_DTB_BAD_SHA   ← 仅本地测试覆盖，生产不得设置
set -u

GOOD_SHA="${TTBOX_DTB_GOOD_SHA:-277d9de87876a4e6160ae7ac048d4adadec73bbaa7706f39e2b5a5fe42379980}"
BAD_SHA="${TTBOX_DTB_BAD_SHA:-7b8cc8925552c4a261bb2207e59c005541feda6e575d706cf8a2c5171c1de9a3}"
DTB_NAME="rk3588-orangepi-5-plus.dtb"
# 报告（供面板 /api/presets/_dtbfix/export 读回；目录由 web 的 presets_dir() 决定）
REPORT="${TTBOX_DTB_REPORT:-/opt/ttbox/presets/_dtbfix.json}"

ROOT="${1:-/}"
ROOT="${ROOT%/}"
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
SRC="${TTBOX_DTB_SRC:-$HERE/../deploy/dtb/$DTB_NAME}"

log()  { printf '[ttbox-dtb-fix] %s\n' "$*"; }
warn() { printf '[ttbox-dtb-fix][WARN] %s\n' "$*" >&2; }

# ---- 报告累加器（最后一次性落盘成合法 JSON）----
R_SEEN=""
R_ACT=""
R_NOTE=""
_add() { R_SEEN="${R_SEEN}${R_SEEN:+,}\"$1\""; }
_act() { R_ACT="${R_ACT}${R_ACT:+,}\"$1\""; }
_note(){ R_NOTE="${R_NOTE}${R_NOTE:+ | }$1"; }
# 证据串净化：换行→'|'，TAB/CR→空格，再去掉引号、反斜杠与残余控制字符。
# ★ 1.5.24 实测教训：extlinux.conf 用 TAB 缩进，TAB(0x09) 是 JSON 非法控制字符，
#   直接原样写入会让整份报告无法解析（面板报 "preset is damaged: Invalid control character"）。
_esc() {
    printf '%s' "$1" | tr '\n\t\r' '|  ' | tr -d '"\\' \
        | tr -d '\000-\010\013\014\016-\037\177'
}

# ---- 启动链证据（一次就能定位 u-boot 到底读哪份 DTB）----
# ★ 只收集【结构化关键值】，不落 extlinux/app 原文 —— 原文含 TAB 会让报告 JSON 非法
#   （1.5.24 板上实测）。这些值已足够回答"u-boot 读哪份 DTB、有没有叠加 dtbo"。
BOOT_FDT=""
BOOT_DTBS=""
BOOT_OVL="false"
BOOT_KW="0"
BOOT_VIDEO0="false"
BOOT_SYSHX="false"
BOOT_CMDLINE=""
probe_boot() {
    [ -e "${ROOT}/dev/video0" ] && BOOT_VIDEO0="true"
    [ -e "${ROOT}/sys/class/hdmirx" ] && BOOT_SYSHX="true"
    if [ -r "${ROOT}/proc/cmdline" ]; then
        BOOT_CMDLINE="$(_esc "$(cat "${ROOT}/proc/cmdline" 2>/dev/null)")"
    fi
    for _ec in "${ROOT}/boot/extlinux/extlinux.conf" \
               "${ROOT}/boot/extlinux.conf" \
               "${ROOT}/extlinux/extlinux.conf"; do
        if [ -r "$_ec" ]; then
            # extlinux 关键字实际是大写，但配置可能被人手改成小写 ⇒ 提取时忽略大小写
            BOOT_FDT="$(grep -iE '^[[:space:]]*FDT[[:space:]]' "$_ec" 2>/dev/null | head -1 \
                | sed -e 's/^[[:space:]]*[Ff][Dd][Tt][[:space:]][[:space:]]*//' -e 's/[[:space:]].*$//')"
            # 配置里出现过的所有 dtb/dtbo 文件名（去重）—— 能一眼看出有没有叠加 hdmirx overlay
            BOOT_DTBS="$(grep -oE '[A-Za-z0-9_.+-]+\.dtbo?' "$_ec" 2>/dev/null \
                | sort -u | paste -sd, - )"
            grep -qiE '^[[:space:]]*FDTOVERLAYS' "$_ec" 2>/dev/null && BOOT_OVL="true"
            BOOT_KW="$(grep -ciE '^[[:space:]]*(LABEL|KERNEL|FDT|APPEND|FDTOVERLAYS)' "$_ec" 2>/dev/null || echo 0)"
            break
        fi
    done
}

write_report() {
    local replaced="$1" ts
    ts="$(date -Is 2>/dev/null || date)"
    _rd="$(dirname "$REPORT")"
    mkdir -p "$_rd" 2>/dev/null || true
    # 目录若由本脚本（root）新建，须让 ttbox 组可读（面板 /api/presets 以 ttbox 身份读）
    chgrp ttbox "$_rd" 2>/dev/null || true
    chmod 0775 "$_rd" 2>/dev/null || true
    {
        printf '{\n'
        printf '  "name": "_dtbfix",\n'
        printf '  "note": "TTBOX DTB 修复诊断报告（自动生成，非预设。读法：GET /api/presets/_dtbfix/export）",\n'
        printf '  "at": "%s",\n' "$ts"
        printf '  "uid": %s,\n' "$(id -u 2>/dev/null || echo -1)"
        printf '  "script_ver": "1.5.26",\n'
        printf '  "root": "%s",\n' "$(_esc "$ROOT")"
        printf '  "src": "%s",\n' "$(_esc "$SRC")"
        printf '  "src_sha": "%s",\n' "${SRC_SHA:-}"
        printf '  "replaced": %s,\n' "$([ "$replaced" = 1 ] && echo true || echo false)"
        printf '  "seen": [%s],\n' "$R_SEEN"
        printf '  "acted": [%s],\n' "$R_ACT"
        printf '  "boot_probe": {\n'
        printf '    "video0": %s,\n' "$BOOT_VIDEO0"
        printf '    "sys_class_hdmirx": %s,\n' "$BOOT_SYSHX"
        printf '    "fdt_from_extlinux": "%s",\n' "$(_esc "$BOOT_FDT")"
        printf '    "extlinux_dtb_refs": "%s",\n' "$(_esc "$BOOT_DTBS")"
        printf '    "extlinux_has_fdtoverlays": %s,\n' "$BOOT_OVL"
        printf '    "extlinux_kw_lines": %s,\n' "${BOOT_KW:-0}"
        printf '    "cmdline": "%s"\n' "$BOOT_CMDLINE"
        printf '  },\n'
        printf '  "note2": "%s"\n' "$(_esc "$R_NOTE")"
        printf '}\n'
    } > "$REPORT" 2>/dev/null || warn "报告写入失败: $REPORT"
    chmod 0644 "$REPORT" 2>/dev/null || true
}

# ---- 0. 前置 ----
probe_boot
SRC_SHA=""
if [ ! -f "$SRC" ]; then
    warn "源 DTB 不存在: $SRC（跳过，不影响安装）"
    _note "源 DTB 不存在"
    write_report 0
    exit 0
fi
SRC_SHA="$(sha256sum "$SRC" 2>/dev/null | cut -d' ' -f1)"
if [ "$SRC_SHA" != "$GOOD_SHA" ]; then
    warn "源 DTB 指纹不符（got=$SRC_SHA want=$GOOD_SHA），拒绝使用（跳过）"
    _note "源 DTB 指纹不符"
    write_report 0
    exit 0
fi
if [ "$(id -u)" != "0" ] && [ -z "${TTBOX_DTB_FIX_TEST:-}" ]; then
    # T1.08（2026-09-21）：非 root 只表示"本路径不改 DTB"，**不等于有问题**。
    # 旧版一律 warn 到 stderr，而 web 面板把 edid_apply.sh 的 stderr 原样拼进
    # 「EDID 应用失败: …」⇒ 即便开机证据早已显示 HDMI-RX 就绪，这条无害提示仍会被
    # 运维当成第二个故障（2026-09-21 用户反馈实录：它与真实 traceback 连成一串）。
    # 故：HDMI-RX 已就绪（/dev/video0 + /sys/class/hdmirx 都在 ⇒ DTB 已是好版本）时
    # 降级为普通日志（stdout）；只有 HDMI-RX 确实未就绪才保留 WARN（那才真需要 root）。
    if [ "$BOOT_VIDEO0" = "true" ] && [ "$BOOT_SYSHX" = "true" ]; then
        log "非 root：跳过 DTB 修复（HDMI-RX 已就绪，无需修复）"
    else
        warn "非 root，跳过 DTB 修复（HDMI-RX 未就绪，需 root 路径修复）"
    fi
    _note "非 root 跳过（web 路径正常现象；root 路径见开机 ttbox-edid.service）"
    # 不要用"非 root 跳过"覆盖掉开机 root 写的真实报告
    if [ -f "$REPORT" ] && grep -q '"uid": 0' "$REPORT" 2>/dev/null; then
        log "已存在 root 报告，保留不覆盖: $REPORT"
    else
        write_report 0
    fi
    exit 0
fi

# ---- 1. 定位全部候选 ----
#   为什么要 -L：板上 device-tree / dtb / dtb-<ver> 任一节都可能是软链，
#   默认 find 不下降 ⇒ 一条都找不到（1.5.22/1.5.23 空转的成因之一）。
#   为什么要多锚点：u-boot 实际读的是 /boot 下那一份，只修 /lib/firmware 等于没修。
#   为什么不用 -xdev：/lib、/usr/lib、/boot 之间可能互为软链或跨挂载点，
#   限制同设备会漏掉实体；用 -maxdepth 8 限深即可。
CANDS=""
_add_cand() {
    [ -n "$1" ] || return 0
    CANDS="${CANDS}${CANDS:+
}$1"
}
for anchor in "$ROOT/lib/firmware" "$ROOT/usr/lib/firmware" "$ROOT/boot"; do
    [ -d "$anchor" ] || continue
    found="$(find -L "$anchor" -maxdepth 8 -type f -name "$DTB_NAME" 2>/dev/null)"
    if [ -n "$found" ]; then
        while IFS= read -r one; do _add_cand "$one"; done <<EOF
$found
EOF
    fi
done
# 1b. extlinux.conf 里 FDT 直接点名的那一份（最接近 u-boot 实际读取路径）
#     FDT 的语义是「相对 extlinux.conf 所在分区根」，该分区通常挂在 /boot，
#     故两种解释都要试：/boot/<rel>（正常）与 /<rel>（分区根即 / 的例外情形）。
if [ -n "$BOOT_FDT" ]; then
    _fdt_rel="${BOOT_FDT#/}"
    _add_cand "${ROOT}/boot/${_fdt_rel}"
    _add_cand "${ROOT}/${_fdt_rel}"
fi
# 去重（软链可能让同一实体出现两次）
TARGETS="$(printf '%s\n' "$CANDS" | sed '/^$/d' | awk '!seen[$0]++')"

if [ -z "$TARGETS" ]; then
    warn "未找到目标 DTB（已搜 lib/firmware、usr/lib/firmware、boot 及 extlinux FDT），跳过"
    _note "多锚点搜索未命中任何 $DTB_NAME"
    write_report 0
    exit 0
fi
_note "候选 $(printf '%s\n' "$TARGETS" | wc -l) 份"

rc=0
REPLACED=0
while IFS= read -r dst; do
    [ -n "$dst" ] || continue
    if [ ! -f "$dst" ]; then
        _add "$dst|不存在"
        continue
    fi
    cur="$(sha256sum "$dst" 2>/dev/null | cut -d' ' -f1)"
    _add "$dst|$cur"
    case "$cur" in
        "$GOOD_SHA")
            log "$dst 已是正确的 DTB，无需改动"
            ;;
        "$BAD_SHA")
            log "$dst 是出厂坏版本（HDMI-RX disabled），开始替换"
            bak="${dst}.ttbox-bak-$(date +%Y%m%d%H%M%S)"
            if ! cp -a -- "$dst" "$bak" 2>/dev/null; then
                warn "备份失败，放弃替换: $dst"; rc=0; _act "$dst|备份失败"; continue
            fi
            if ! cat "$SRC" > "$dst" 2>/dev/null; then
                warn "写入失败，回滚: $dst"; cat "$bak" > "$dst" 2>/dev/null
                _act "$dst|写入失败已回滚"; continue
            fi
            chmod 0644 "$dst" 2>/dev/null || true
            got="$(sha256sum "$dst" 2>/dev/null | cut -d' ' -f1)"
            if [ "$got" != "$GOOD_SHA" ]; then
                warn "替换后校验不符（got=$got），回滚: $dst"
                cat "$bak" > "$dst" 2>/dev/null
                _act "$dst|校验不符已回滚"; continue
            fi
            log "已替换为可用 DTB（备份 $bak）"
            _act "$dst|已替换"
            REPLACED=1
            ;;
        *)
            warn "$dst 指纹未登记（got=$cur），不敢动，跳过"
            _act "$dst|指纹未登记跳过"
            ;;
    esac
done <<EOF
$TARGETS
EOF

# ---- 2. 真换过才安排重启 ----
# 为什么必须自动重启：DTB 由 u-boot 开机时读取，只换文件不重启 ⇒ 修复**永远不生效**，
# 客户会再报一遍同样的 EDID 错，而线上看"OTA 已经装上了"——比不修更难查。
# 只在「确实发生了替换」时安排（已是正确的版本 ⇒ 不动 ⇒ 绝不反复重启）；
# 留 2 分钟缓冲，让 OTA 的 activate/健康检查先正常收尾（可被 shutdown -c 取消）。
if [ "$REPLACED" = 1 ]; then
    if [ -n "${TTBOX_DTB_FIX_TEST:-}" ]; then
        log "[TEST] 已跳过重启安排（TTBOX_DTB_FIX_TEST）"
        _note "TEST 模式，未安排重启"
    else
        MARK="/var/lib/ttbox/dtb-reboot-pending"
        mkdir -p /var/lib/ttbox 2>/dev/null
        { date -Is 2>/dev/null || date; } > "$MARK" 2>/dev/null || true
        if command -v shutdown >/dev/null 2>&1; then
            if shutdown -r +2 "TTBOX: DTB 已更新，重启后 HDMI 采集生效" 2>/dev/null; then
                log "已安排 2 分钟后重启（取消命令：shutdown -c）"
                _note "已安排 2 分钟后重启"
            else
                warn "shutdown 调用失败 ⇒ 请手动重启，否则 DTB 修复不生效"
                _note "shutdown 失败，需手动重启"
            fi
        else
            warn "系统无 shutdown 命令 ⇒ 请手动重启，否则 DTB 修复不生效"
            _note "无 shutdown，需手动重启"
        fi
    fi
fi

write_report "$REPLACED"
exit 0
