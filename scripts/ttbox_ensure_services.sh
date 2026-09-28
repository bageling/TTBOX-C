#!/usr/bin/env bash
# ttbox_ensure_services.sh — TTBOX 幂等自愈（DEP-01 / T1.03）
#
# 作用：把 5 个 ttbox unit（core/web/preview/usbproxy/edid）从【发布树】同步到
#       /etc/systemd/system/ —— 现场 unit 被误删/误改后能自动长回（配 ttbox-ensure.timer，
#       每 10 分钟巡检一次；fhs_init 尾部也会调用一次）。
#
# 架构借鉴竞品 yu 的 `ensure_makcu_service.sh`（幂等、cmp -s 后才 install、二进制不存在
# 就 exit 0），但**零字节复制**，并对其做两处加固：
#   ★ 比 yu 多一步：unit 源取自 /opt/ttbox/current/deploy/systemd/（随版本交付）——
#     current 断链时【全部跳过】（此时自愈无意义，发布系统才是病根，不该乱写 unit）。
#   ★ 只为【已具备可执行产物】的 unit 写文件：ExecStart 首 token 经 current 解析后不可执行
#     时跳过该 unit，绝不产生"装上就 203/EXEC"的僵尸服务。
#
# 契约（design §B3.1，逐条落实）：
#   1. 非 root（id -u != 0）            → exit 0（不制造半套自愈）
#   2. current 断链 / unit 源缺失       → 告警 + 非零退出，且不写任何 unit（D15）
#   3. ExecStart 二进制缺失（! -x）     → 跳过该 unit，不写
#   4. 同文件系统 mktemp 落候选 → cmp -s 自证完整 → 与已装 cmp -s 比对；
#      一致跳过；不一致 **原子 mv -f 替换** + changed=1
#      ★ 不变量：UNIT_DIR 内只有「完整 unit」或「缺席」两种状态；
#        既有完整 unit **绝不因一次写失败而被销毁**（自愈是「补」，不是「删」）。
#      ★ 禁止任何 `rm <dst>` 分支 —— 写失败时 dst 必须保持原样。
#   5. changed 时才 systemctl daemon-reload
#   6. systemctl enable <unit> --now（|| true，幂等；已启动也不报错）
#
# 幂等：重复运行无副作用。D15（2026-09-18 定案）：current 断链 = 自愈失能本身，
#       改为【告警 + 非零退出】可观测；其余失败仍不中断（不得让 timer 失败刷屏）。
#
# 可测性：TTBOX_PREFIX / TTBOX_UNIT_DIR / TTBOX_SYSTEMD 均可覆盖，便于临时前缀自测。
#
# 用法：
#   sudo scripts/ttbox_ensure_services.sh
#   自测: TTBOX_PREFIX=/tmp/pfx TTBOX_UNIT_DIR=/tmp/units TTBOX_SYSTEMD=0 bash scripts/ttbox_ensure_services.sh

set -uo pipefail

TTBOX_PREFIX="${TTBOX_PREFIX:-/opt/ttbox}"
UNIT_DIR="${TTBOX_UNIT_DIR:-/etc/systemd/system}"
SYSTEMD_MODE="${TTBOX_SYSTEMD:-auto}"

# 受管 unit（收敛后每 unit 全仓仅一份，见 T1.05；此处为按依赖顺序的显式清单）
# OTA 特权通道两个 unit（2026-09-18 定案）：path 负责监听触发，service 是 root 执行端。
#   path unit 无 ExecStart ⇒ ExecStart 预检自然放行（unit_execstart_bin 返回空）。
UNITS="ttbox-core.service ttbox-web.service ttbox-preview.service ttbox-usbproxy.service ttbox-edid.service ttbox-ota.path ttbox-ota.service"
UNIT_SRC_REL="deploy/systemd"

log()  { printf '[ensure] %s\n' "$*"; }
warn() { printf '[ensure][WARN] %s\n' "$*" >&2; }

have_systemd() {
    case "$SYSTEMD_MODE" in
        0|no|off|false) return 1 ;;
        1|yes|on|true)  return 0 ;;
    esac
    [[ -d /run/systemd/system ]] && command -v systemctl >/dev/null 2>&1
}

# 把 unit 内硬编码的 /opt/ttbox 前缀重映射到当前 TTBOX_PREFIX（仅自测时生效）
remap_path() {
    local p="$1"
    if [[ "$TTBOX_PREFIX" != "/opt/ttbox" ]]; then
        printf '%s' "${p/#\/opt\/ttbox/$TTBOX_PREFIX}"
    else
        printf '%s' "$p"
    fi
}

# unit 的 ExecStart 首 token（去掉 systemd exec 前缀符号 - @ + !）
unit_execstart_bin() {
    local f="$1" raw
    raw="$(awk -F= '/^ExecStart=/{sub(/^ExecStart=/,"");print;exit}' "$f")"
    raw="${raw#-}"; raw="${raw#@}"; raw="${raw#+}"; raw="${raw#!}"
    printf '%s' "$(remap_path "${raw%% *}")"
}

# ---------------------------------------------------------------------------
# 1. 非 root → exit 0
# ---------------------------------------------------------------------------
if [ "$(id -u)" -ne 0 ]; then
    log "非 root（id=$(id -u)）：自愈需写 ${UNIT_DIR} 与调用 systemctl —— 跳过，exit 0"
    exit 0
fi

CURRENT_LINK="${TTBOX_PREFIX}/current"
SRC_DIR="${CURRENT_LINK}/${UNIT_SRC_REL}"

# ---------------------------------------------------------------------------
# 2. current 断链 / unit 源缺失 → 告警 + 非零退出（D15，2026-09-18 定案 I-32）
#    旧行为是静默 exit 0 —— 自愈失效时 timer 每 10 分钟静默空转，无人知晓。
#    断链意味着"自愈机制失能"本身，必须可观测：告警 + 非零退出。
# ---------------------------------------------------------------------------
current_resolved="$(readlink -f -- "$CURRENT_LINK" 2>/dev/null || true)"
if [ ! -L "$CURRENT_LINK" ] || [[ -z "$current_resolved" ]] || [ ! -d "$SRC_DIR" ]; then
    log "ALERT: current 断链或 ${SRC_DIR} 不存在 —— 自愈失效，请立即检查发布树（ttbox.sh doctor 可诊断）"
    echo "[ttbox-ensure][ALERT] current 断链：自愈失效（current=${CURRENT_LINK}）" >&2
    exit 1
fi
log "unit 源：${SRC_DIR}（current -> ${current_resolved}）"

# ---------------------------------------------------------------------------
# 3/4/5. 逐 unit：比对 → 安装 → daemon-reload
# ---------------------------------------------------------------------------
changed=0
installed=""
for unit in $UNITS; do
    src="${SRC_DIR}/${unit}"
    if [ ! -f "$src" ]; then
        warn "${unit}: 源缺失 ${src} —— 跳过"
        continue
    fi

    exec_bin="$(unit_execstart_bin "$src")"
    # 仅对绝对路径做可执行性预检（自测前缀下可能故意指向别处）
    if [[ "$exec_bin" == /* ]] && [ ! -x "$exec_bin" ]; then
        warn "${unit}: ExecStart 不可执行（${exec_bin}）—— 跳过，不写 unit（防僵尸服务）"
        continue
    fi

    dst="${UNIT_DIR}/${unit}"
    # ① 目标目录提前创建（原在 install 前才 mkdir，太晚——下一步 mktemp 就要用它）
    mkdir -p -- "$UNIT_DIR"
    # ② 候选文件与 dst **同文件系统**（tmp 落 ${UNIT_DIR} 内）—— T5b 的**根因修复**：
    #    mktemp 默认落 ${TMPDIR:-/tmp}，与 ${UNIT_DIR} 不同 fs ⇒ "就地写 dst" 只能拷+删、**非原子**。
    #    末段是 `.XXXXXX`（不是 `.service`）⇒ systemd 扫描 *.{service,unit,...} 时不会把它当 unit。
    tmp="$(mktemp "${UNIT_DIR}/.${unit}.XXXXXX")" || {
        warn "${unit}: 无法在 ${UNIT_DIR} 建候选文件 —— 跳过，dst 不动"
        continue
    }
    # ③ 候选写入完整性护栏：cat 必须成功 **且** 与源逐字节一致，否则跳过、dst 不动。
    if ! cat -- "$src" > "$tmp" || ! cmp -s -- "$tmp" "$src"; then
        warn "${unit}: 候选写入不完整（磁盘满 / 写限？）—— 跳过，dst 不动"
        rm -f -- "$tmp"
        continue
    fi
    # ④ 已是最新 → 跳过（原语义）
    if [ -f "$dst" ] && cmp -s -- "$tmp" "$dst"; then
        rm -f -- "$tmp"
        log "${unit}: 已是最新（cmp -s 一致），跳过"
        installed="${installed} ${unit}"
        continue
    fi
    # ⑤ 设定候选文件的权限与属主（失败只告警，不阻断替换）
    chmod 0644 -- "$tmp" 2>/dev/null || true
    chown root:root -- "$tmp" 2>/dev/null || warn "${unit}: chown 失败（继续）"
    # ⑥ **原子替换**：同 fs 的 `mv` 走 rename(2)——要么完整新 unit，要么保持旧 unit，
    #    **绝不 `rm` 目标文件**。不变量：${UNIT_DIR} 内只有"完整 unit"或"缺席"两种状态，
    #    且**既有完整 unit 绝不因一次写失败而被销毁**（自愈是"补"，不是"删"）。
    if mv -f -- "$tmp" "$dst"; then
        if cmp -s -- "$src" "$dst"; then
            log "${unit}: 安装/更新 -> ${dst}"
            changed=1
            installed="${installed} ${unit}"
        else
            warn "${unit}: 落盘后复核不一致（极端情况）—— 文件已替换，仍标记 changed"
            changed=1
        fi
    else
        warn "${unit}: mv 失败 —— **dst 未被修改（保持原样）**"
        rm -f -- "$tmp"
    fi
done

if (( changed )) && have_systemd; then
    if systemctl daemon-reload; then
        log "systemctl daemon-reload"
    else
        warn "daemon-reload 失败（继续）"
    fi
elif (( changed )); then
    log "SKIP：无 systemd，跳过 daemon-reload"
fi

# ---------------------------------------------------------------------------
# 6. enable --now（|| true，幂等）
# ---------------------------------------------------------------------------
if have_systemd; then
    for unit in $installed; do
        # 2026-09-19 板端死锁实证：OTA 浇筑流程（ttbox-ota.service ExecStart 里）对自己
        # enable --now ⇒ start 任务排在当前 oneshot 运行之后 ⇒ 互相等待，安装卡死。
        # 检测到 ttbox-ota.service 的 start 任务正在运行（= 我们就在它里面）时跳过。
        if [[ "$unit" == "ttbox-ota.service" ]]; then
            if systemctl list-jobs --no-pager 2>/dev/null | grep -q 'ttbox-ota\.service[[:space:]]*start[[:space:]]*running'; then
                log "SKIP：enable --now ttbox-ota.service（其 start 任务运行中＝本脚本由它拉起，自启会死锁）"
                continue
            fi
        fi
        if systemctl enable --now "$unit" >/dev/null 2>&1; then
            log "enable --now ${unit} OK"
        else
            warn "enable --now ${unit} 失败（已启动/无权限等，已忽略）"
        fi
    done
else
    log "SKIP：无 systemd，跳过 enable --now"
fi

# ---------------------------------------------------------------------------
# 7. DTB 修复（★ 2026-09-28 从 scripts/edid/edid_apply.sh 的入口处搬到这里）
# ---------------------------------------------------------------------------
# 为什么搬家：edid_apply.sh 被 systemd 的 `timeout` 卡着（开机只有几十秒预算），
#   而 ttbox_dtb_fix.sh 在开机窗口要吃掉 12 秒（多锚点 find -L + 两份 DTB sha256sum；
#   同样的脚本在系统就绪后只要 0.231 秒——差异来自开机期 CPU/IO 争抢，不是脚本慢）。
#   实测后果：09-27 12:27 那次开机，EDID 直到 12:27:53 才写完，12:28:01 就被 timeout
#   精确杀死，而内核日志显示再晚 1~2 秒 HPD 重协商就 lock ok 了。
#   搬迁后 EDID 不再等它，DTB 修复也不再被 EDID 的预算挤压。
# 为什么放周期巡检（本脚本由 ttbox-ensure.timer 每 10 分钟拉起）够用：
#   DTB 由 u-boot 在开机时读取，换完本来就必须重启才生效；晚 10 分钟没有任何实际影响。
#   而"什么时候真的需要修"只有一种情形：出厂镜像的 DTB 是 hdmirx disabled 的坏版本，
#   修过一次之后永远命中 GOOD_SHA 分支（replaced=false），之后再跑只是几次 sha256sum。
# 安全：脚本自带指纹门禁（只认已知坏版本 GOOD/BAD_SHA，其余一律不动）+ **恒退出 0**
#   ⇒ 绝不允许它把一次自愈判失败。
# 幂等：本块重复执行无副作用。
DTB_FIX="${CURRENT_LINK}/scripts/ttbox_dtb_fix.sh"
if [ -x "$DTB_FIX" ]; then
    if "$DTB_FIX"; then
        log "dtb_fix: 已执行（报告见 /opt/ttbox/presets/_dtbfix.json）"
    else
        warn "dtb_fix: 返回非零（已忽略；DTB 属启动链，不影响自愈结果）"
    fi
else
    log "SKIP：${DTB_FIX} 不存在或不可执行"
fi

log "完成（changed=${changed}）"
exit 0
