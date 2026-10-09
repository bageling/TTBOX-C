#!/usr/bin/env bash
# ttbox_release_install.sh — TTBOX 发布浇筑 + 原子切换 + 回滚（DEP-06 / T1.01）
#
# 背景：全仓库此前没有任何脚本会创建 /opt/ttbox/current 软链，而 5 个 systemd unit
# 里有 7 处路径都指向 /opt/ttbox/current/...。于是换一块新板烧录必然 203/EXEC。
# 本脚本把"完整运行树装进 releases/<ver>/ + 原子换 current 指针"这套流程固定下来。
#
# 布局（design §B1.1）：
#   ${TTBOX_PREFIX}/releases/<ver>/        完整运行树（原子整体：bin/ lib/ plugins/ ...）
#   ${TTBOX_PREFIX}/current -> releases/<ver>   原子切换的唯一指针
#   ${TTBOX_PREFIX}/<子目录> -> current/<子目录> 迁移期过渡软链（见 TRANSITIONAL_LINKS 清单：
#       plugins / scripts——旧布局硬编码路径兼容；源码侧改造完成后可删）
#
# 用法：
#   ttbox_release_install.sh <ver> <payload_dir> [--activate]
#   ttbox_release_install.sh --rollback
#
# 流程（幂等，可重跑，design §B1.2）：
#   1. 校验 payload：RELEASE_MANIFEST.json 存在 + files_sha256 抽验（bin/ 全验）
#   2. tar 管道把 payload 拷到 releases/<ver>.staging/（排除 __pycache__ / *.pyc / .git）
#   3. 对 staging 做全量 sha256 校验
#   4. 原子发布：mv releases/<ver>.staging releases/<ver>（同分区 rename = 原子）
#   5. --activate：原子换链 -> 断言 unit -> 过渡软链 -> daemon-reload+restart -> 30s 健康检查
#                  （失败自动回切上一版本并重启）
#   6. 保留策略：成功后清理第 3 旧版本（默认保留 2 版）+ 循环清 *.staging / *.old.* 残留
#
# 可测性：所有绝对前缀参数化（TTBOX_PREFIX / TTBOX_ETC / TTBOX_RUN），
# 便于在 WSL / 容器里用临时前缀跑通 1-4 步并验证"切换中途掉电不留半截版本"。
# 带 systemd 的第 5 步在无 systemd 环境（WSL）会打印 SKIP 并跳过（绝不假装跑过）。
#
set -euo pipefail
# 2026-09-17 板端实测订正：非登录 SSH（umask 077）下 staging mkdir 产出 0700 root 目录，
# User=ttbox 的服务 200/CHDIR。发布树必须全局可遍历——强制 022，不信任调用方 umask。
umask 022

# ---------------------------------------------------------------------------
# 路径前缀（全部可覆盖：本机无 systemd / 无 /opt，用临时前缀即可真跑）
# ---------------------------------------------------------------------------
TTBOX_PREFIX="${TTBOX_PREFIX:-/opt/ttbox}"
TTBOX_ETC="${TTBOX_ETC:-/etc/ttbox}"
TTBOX_RUN="${TTBOX_RUN:-/run/ttbox}"

RELEASES_DIR="${TTBOX_PREFIX}/releases"
CURRENT_LINK="${TTBOX_PREFIX}/current"

# release 树内 unit 目录（相对 current），unit 是版本产物的一部分（DEP-07 前置）
UNIT_REL_DIR="deploy/systemd"

# 可调参数
KEEP_VERSIONS="${TTBOX_KEEP_VERSIONS:-2}"
HEALTH_TIMEOUT="${TTBOX_HEALTH_TIMEOUT:-30}"
WEB_PORT="${TTBOX_WEB_PORT:-8000}"
RESTART_UNITS="${TTBOX_RESTART_UNITS:-ttbox-core ttbox-web ttbox-usbproxy}"
# systemd 开关：auto（默认，探测 PID1）/ 1 强制使用 / 0 强制跳过（WSL、容器、本机验证）
SYSTEMD_MODE="${TTBOX_SYSTEMD:-auto}"
#
# ★ 测试钩子（生产不可达，双条件锁定）：
#   环境变量 TTBOX_RELEASE_SELFTEST=1 【且】命令行显式传 --selftest-stall <秒>，
#   两者同时满足才会在 staging 完成后停顿 N 秒（用于"中途 kill 不留半截版本"验证）。
#   生产环境两者缺一即完全不可达——防止该变量从 profile/systemd Environment/CI 泄漏
#   导致发布脚本在生产静默卡住（OTA 半挂）。
SELFTEST_ENABLED="${TTBOX_RELEASE_SELFTEST:-0}"
SELFTEST_STALL=0   # 唯一赋值点：--selftest-stall 参数解析处（且需 SELFTEST_ENABLED=1）
#
# ★ 迁移期过渡软链清单（design §B1.1 + DEP-06 全仓硬编码路径普查结论）：
#   每项 "旧布局硬编码路径:release 树内目标（经 current 解析）"。
#   ⚠ 只收录【随版本交付的代码/工具目录】；客户数据（config/models/presets/…）
#   绝不入列——指进 release 树会在下次发布被整体替换，造成用户数据丢失
#   （数据类硬编码见普查报告【应改造】档，归源码侧修复）。
#   ⚠ 与 scripts/ttbox_release_verify.sh 的同名清单保持同步（verify 逐条断言）。
TRANSITIONAL_LINKS=(
    # 插件发现（framework plugin root）+ 旧 web unit 停血路径
    "plugins:current/plugins"
    # edid 工具链 import（ttbox-web.py、scripts/edid/hdmirx_edid.py:9、
    # edid_apply.sh:17；wifi_manager 与 lan_blocklist 已随 S1/D04 移出出货包）
    "scripts:current/scripts"
)

# tar 排除项（不依赖 rsync）；E02（2026-09-18）：--exclude=tests 与 fhs_init tcopy 的
# payload 侧口径镜像，两处一起改。
# P8（2026-10-01）补 --exclude=.registry.json：插件注册表是**运行期状态**（PluginRegistry
# 在 <plugins_root>/.registry.json 现场生成，内含绝对路径 —— 实测泄漏过一条 dev 树路径进
# payload）。剔除集三处同一口径：本文件 / ttbox_fhs_init.sh::tcopy / ttbox_pack_ota.sh。
TAR_EXCLUDES=(--exclude=__pycache__ --exclude='*.pyc' --exclude=.git --exclude=tests
              --exclude=.registry.json)

# 记录切换前的版本，供健康检查失败时回切
PREV_VERSION=""

log()  { printf '[release] %s\n' "$*"; }
warn() { printf '[release][WARN] %s\n' "$*" >&2; }
die()  { printf '[release][ERROR] %s\n' "$*" >&2; exit 1; }

usage() {
    cat <<'EOF'
用法:
  ttbox_release_install.sh <ver> <payload_dir> [--activate]
  ttbox_release_install.sh --rollback

参数:
  <ver>              版本号（如 1.2.0），仅允许 [A-Za-z0-9._-]
  <payload_dir>      发布 payload 目录，必须含 RELEASE_MANIFEST.json
  --activate         发布后原子切换 current 指针并重启服务（含 30s 健康检查）
  --rollback [ver]   current 指回上一版本（或指定 <ver>，D14）并重启；
                     回滚后健康检查失败 ⇒ 非零退出（不假绿）
  --selftest-stall N 仅自测用：需同时设 TTBOX_RELEASE_SELFTEST=1 才生效（生产不可达），
                     staging 完成后停顿 N 秒，用于验证"中途 kill 不留半截版本"

环境变量（均可覆盖，便于本机/容器验证）:
  TTBOX_PREFIX          默认 /opt/ttbox
  TTBOX_ETC             默认 /etc/ttbox
  TTBOX_RUN             默认 /run/ttbox
  TTBOX_KEEP_VERSIONS   默认 2（保留版本数）
  TTBOX_HEALTH_TIMEOUT  默认 30（秒）
  TTBOX_WEB_PORT        默认 8000
  TTBOX_RESTART_UNITS   默认 "ttbox-core ttbox-web ttbox-usbproxy"
  TTBOX_SYSTEMD         auto(默认) | 1(强制用 systemd) | 0(强制跳过 restart/健康检查)

迁移期过渡软链（--activate 时按 TRANSITIONAL_LINKS 清单浇筑，verify 逐条断言）:
  /opt/ttbox/plugins -> current/plugins    插件发现 + 旧 web unit 路径兼容
  /opt/ttbox/scripts -> current/scripts    wifi/EDID/LAN-blocklist 工具链 import
EOF
}

# ---------------------------------------------------------------------------
# 工具函数
# ---------------------------------------------------------------------------

# 把 release 内硬编码的 /opt/ttbox 前缀重映射到当前 TTBOX_PREFIX。
# 仅当 TTBOX_PREFIX 非默认值时生效——让断言在 WSL 临时前缀下也真实有效。
remap_path() {
    local p="$1"
    if [[ "$TTBOX_PREFIX" != "/opt/ttbox" ]]; then
        printf '%s' "${p/#\/opt\/ttbox/$TTBOX_PREFIX}"
    else
        printf '%s' "$p"
    fi
}

sha256_of() { sha256sum -- "$1" | cut -d' ' -f1; }

# 用 python3 解析 manifest（比 grep 稳健；板端/本机均有 python3）
manifest_pairs() {
    python3 - "$1" <<'PY'
import json, sys
with open(sys.argv[1], "r", encoding="utf-8") as fh:
    m = json.load(fh)
for path, digest in sorted((m.get("files_sha256") or {}).items()):
    print(f"{digest}\t{path}")
PY
}

manifest_field() {
    python3 - "$1" "$2" <<'PY'
import json, sys
with open(sys.argv[1], "r", encoding="utf-8") as fh:
    m = json.load(fh)
val = m.get(sys.argv[2], "")
print(val if isinstance(val, str) else json.dumps(val))
PY
}

have_systemd() {
    case "$SYSTEMD_MODE" in
        0|no|off|false) return 1 ;;
        1|yes|on|true)  return 0 ;;
    esac
    [[ -d /run/systemd/system ]] && command -v systemctl >/dev/null 2>&1
}

current_version() {
    [[ -L "$CURRENT_LINK" ]] || return 0
    local target
    target="$(readlink -f -- "$CURRENT_LINK" 2>/dev/null || true)"
    [[ -n "$target" ]] && basename -- "$target"
}

# 列出已发布版本目录，按 mtime 从新到旧（排除 *.staging / *.old.* 残留）
list_versions() {
    [[ -d "$RELEASES_DIR" ]] || return 0
    find "$RELEASES_DIR" -mindepth 1 -maxdepth 1 -type d \
        ! -name '*.staging' ! -name '*.old.*' -printf '%T@\t%f\n' 2>/dev/null \
        | sort -rn | cut -f2
}

# ---------------------------------------------------------------------------
# 步骤 1：校验 payload 的 manifest（bin/ 全验）
# ---------------------------------------------------------------------------
validate_payload_manifest() {
    local payload="$1"
    local manifest="${payload}/RELEASE_MANIFEST.json"
    [[ -f "$manifest" ]] || die "payload 缺少 RELEASE_MANIFEST.json: $manifest"

    local n=0 bad=0 digest path fp actual
    while IFS=$'\t' read -r digest path; do
        [[ -z "$path" ]] && continue
        # 本步只对 bin/ 全验（其余文件在第 3 步对 staging 全量验）
        [[ "$path" == bin/* ]] || continue
        n=$((n + 1))
        fp="${payload}/${path}"
        if [[ ! -f "$fp" ]]; then
            warn "payload bin/ 文件缺失: $path"
            bad=1
            continue
        fi
        actual="$(sha256_of "$fp")"
        if [[ "$actual" != "$digest" ]]; then
            warn "payload bin/ sha256 不符: $path"
            warn "  manifest=${digest}"
            warn "  actual  =${actual}"
            bad=1
        fi
    done < <(manifest_pairs "$manifest")

    if (( n == 0 )); then
        warn "manifest 未声明任何 bin/ 文件（files_sha256 为空？），bin/ 抽验跳过"
    fi
    (( bad == 0 )) || die "payload bin/ 校验失败，拒绝发布"
    log "step1 OK：payload manifest 校验通过（bin/ 已验 ${n} 项）"
}

# ---------------------------------------------------------------------------
# 步骤 2：tar 管道拷到 staging
# ---------------------------------------------------------------------------
stage_payload() {
    local payload="$1" staging="$2"
    rm -rf -- "$staging"
    mkdir -p -- "$staging"
    log "step2：tar 管道 payload -> ${staging}"
    tar -C "$payload" "${TAR_EXCLUDES[@]}" -cf - . | tar -C "$staging" --no-same-owner -xf -
    # 2026-09-17 板端实测订正：payload 根目录多来自 mktemp -d（0700），tar 会把 "." 条目的
    # 0700 模式还原到 staging ⇒ mv 后 release 根 = 0700 root，User=ttbox 的服务 200/CHDIR。
    # release 根必须全局可遍历——显式 chmod，不依赖源目录模式。
    chmod 0755 -- "$staging"
}

# ---------------------------------------------------------------------------
# 步骤 3：对 staging 做全量 sha256 校验
# ---------------------------------------------------------------------------
verify_staging() {
    local staging="$1" manifest="$2"
    local n=0 bad=0 digest path fp actual
    while IFS=$'\t' read -r digest path; do
        [[ -z "$path" ]] && continue
        n=$((n + 1))
        fp="${staging}/${path}"
        if [[ ! -f "$fp" ]]; then
            warn "staging 缺失 manifest 声明文件: $path"
            bad=1
            continue
        fi
        actual="$(sha256_of "$fp")"
        if [[ "$actual" != "$digest" ]]; then
            warn "staging sha256 不符: $path"
            bad=1
        fi
    done < <(manifest_pairs "$manifest")

    (( n > 0 )) || die "manifest files_sha256 为空，无法校验 staging 完整性"
    (( bad == 0 )) || die "staging 完整性校验失败，拒绝发布"
    log "step3 OK：staging 全量校验通过（${n} 项）"
}

# ---------------------------------------------------------------------------
# 步骤 4：原子发布（同分区 rename）
# ---------------------------------------------------------------------------
publish_release() {
    local ver="$1"
    local staging="${RELEASES_DIR}/${ver}.staging"
    local final="${RELEASES_DIR}/${ver}"

    [[ -d "$staging" ]] || die "staging 目录不存在: $staging"

    if [[ -d "$final" ]]; then
        # 已存在同版本（幂等重跑 / 覆盖发布）：先把旧目录移到 .old.* 再换入，
        # 尽量缩短 current 悬空窗口；失败则回滚旧目录。
        local trash="${final}.old.$$"
        rm -rf -- "$trash"
        mv -- "$final" "$trash"
        if mv -- "$staging" "$final"; then
            rm -rf -- "$trash"
        else
            mv -- "$trash" "$final"
            die "原子发布失败，已还原旧版本: $final"
        fi
    else
        mv -- "$staging" "$final"
    fi
    log "step4 OK：原子发布 ${ver}（staging -> ${final}）"
}

# ---------------------------------------------------------------------------
# 步骤 5a：原子换链
# ---------------------------------------------------------------------------
switch_current() {
    local ver="$1"
    ln -sfn "releases/${ver}" "${CURRENT_LINK}.new"
    mv -T -- "${CURRENT_LINK}.new" "$CURRENT_LINK"
}

# ---------------------------------------------------------------------------
# 步骤 5b：逐 unit 断言 ExecStart 首 token 可执行 + WorkingDirectory 存在
# ---------------------------------------------------------------------------
assert_units() {
    local dir="${CURRENT_LINK}/${UNIT_REL_DIR}"
    [[ -d "$dir" ]] || die "current 下缺少 unit 目录（unit 应随 release 交付）: $dir"

    local rc=0 f unit exec_raw exec_bin wd
    for f in "$dir"/*.service; do
        [[ -f "$f" ]] || continue
        unit="$(basename -- "$f")"

        exec_raw="$(awk -F= '/^ExecStart=/{sub(/^ExecStart=/,"");print;exit}' "$f")"
        # 去掉 systemd 的 exec 前缀符号 - @ + !
        exec_raw="${exec_raw#-}"; exec_raw="${exec_raw#@}"
        exec_raw="${exec_raw#+}"; exec_raw="${exec_raw#!}"
        exec_bin="${exec_raw%% *}"
        exec_bin="$(remap_path "$exec_bin")"
        if [[ ! -x "$exec_bin" ]]; then
            warn "[assert] ${unit}: ExecStart 首 token 经 current 解析后不可执行: ${exec_bin}"
            rc=1
        fi

        wd="$(awk -F= '/^WorkingDirectory=/{sub(/^WorkingDirectory=/,"");print;exit}' "$f")"
        if [[ -n "$wd" ]]; then
            wd="$(remap_path "$wd")"
            if [[ ! -d "$wd" ]]; then
                warn "[assert] ${unit}: WorkingDirectory 不存在: ${wd}"
                rc=1
            fi
        fi
    done
    return $rc
}

# ---------------------------------------------------------------------------
# 步骤 5c：迁移期过渡软链（design §B1.1 + 硬编码路径普查，清单见文件头 TRANSITIONAL_LINKS）
# ---------------------------------------------------------------------------
ensure_transitional_links() {
    local entry link target
    for entry in "${TRANSITIONAL_LINKS[@]}"; do
        link="${TTBOX_PREFIX}/${entry%%:*}"
        target="${entry#*:}"
        if [[ -e "$link" && ! -L "$link" ]]; then
            local bak="${link}.legacy.$(date +%s)"
            warn "迁移：${link} 是实体目录，移动到 ${bak} 并改为软链"
            mv -- "$link" "$bak"
        fi
        ln -sfnT "$target" "$link"
        log "step5c：过渡软链 ${link} -> ${target}"
    done
}

# ---------------------------------------------------------------------------
# 步骤 5b2：同步 systemd unit 与运行时目录权限（升级场景必备）
# ---------------------------------------------------------------------------
sync_units_and_runtime_perms() {
    log "step5b2：同步 systemd unit 与运行时目录权限"
    local ensure_script="${TTBOX_PREFIX}/current/scripts/ttbox_ensure_services.sh"
    if [[ -x "$ensure_script" ]]; then
        if bash "$ensure_script"; then
            log "unit 同步完成"
        else
            warn "unit 同步返回非 0（继续；release 仍可能可用）"
        fi
    else
        warn "找不到 ensure 脚本 ${ensure_script}，跳过 unit 同步"
    fi

    # V-04：升级上来的旧机器 /var/lib/ttbox/models 子目录可能是 root:root 755，
    # 导致 web(ttbox) 写入 _incoming 报 EACCES。幂等修正。
    if [[ -d /var/lib/ttbox/models ]]; then
        install -d -o ttbox -g ttbox -m 0775 /var/lib/ttbox/models/installed \
            /var/lib/ttbox/models/staging /var/lib/ttbox/models/registry \
            /var/lib/ttbox/models/cache /var/lib/ttbox/models/quarantine \
            /var/lib/ttbox/models/_incoming 2>/dev/null || true
        find /var/lib/ttbox/models -maxdepth 3 -type d -exec chown ttbox:ttbox {} + 2>/dev/null || true
        find /var/lib/ttbox/models -maxdepth 3 -type d -exec chmod 0775 {} + 2>/dev/null || true
        log "模型库运行时权限已修正"
    fi
}

# ---------------------------------------------------------------------------
# 步骤 5c/5d：daemon-reload + restart
# ---------------------------------------------------------------------------
reload_and_restart() {
    if ! have_systemd; then
        warn "SKIP：未检测到 systemd（非 PID1 或无 systemctl）——跳过 daemon-reload/restart"
        return 0
    fi
    # 2026-09-19：板端实测 r2 轮在 ensure 之后、任何 restart 日志之前沉默 6 分钟
    # 被单元 600s 超时杀掉 ⇒ 每步加日志定位，restart 加 --job-timeout 防止单步吃满总超时
    log "reload_and_restart：开始 daemon-reload"
    systemctl daemon-reload
    log "reload_and_restart：daemon-reload OK"
    local u
    # 2026-09-20 教训：板上 systemctl 不认 --job-timeout 选项 ⇒ 1.5.4~1.5.6 三轮安装
    # 重启全部静默失败，web 一直是孤儿老进程（cwd 被清理后面板 500）。改用 GNU timeout 包裹。
    for u in $RESTART_UNITS; do
        if timeout 90 systemctl restart "$u"; then
            log "restart ${u} OK"
        else
            warn "restart ${u} 失败（继续，交由健康检查判定）"
        fi
    done
    # OTA 特权通道（2026-09-18 定案 §2.2）：path 单元 enable 一次即可（幂等）
    if systemctl list-unit-files 2>/dev/null | grep -q '^ttbox-ota\.path'; then
        systemctl enable --now ttbox-ota.path 2>/dev/null || true
    fi
}

# ---------------------------------------------------------------------------
# 步骤 5d：30s 健康检查
# ---------------------------------------------------------------------------
port_open() {
    (exec 3<>"/dev/tcp/${1}/${2}") >/dev/null 2>&1
}

health_check() {
    local deadline=$(( SECONDS + HEALTH_TIMEOUT ))
    while (( SECONDS < deadline )); do
        local ok=1
        [[ -S "${TTBOX_RUN}/core.sock" ]] || ok=0

        local ping="${CURRENT_LINK}/bin/ipc_ping"
        if [[ -x "$ping" ]]; then
            timeout 5 "$ping" --type PING --ipc "${TTBOX_RUN}/core.sock" >/dev/null 2>&1 || ok=0
        fi
        if ! port_open 127.0.0.1 "$WEB_PORT"; then
            ok=0
        fi
        (( ok )) && return 0
        sleep 1
    done
    return 1
}

# ---------------------------------------------------------------------------
# 激活（步骤 5 整体）
# ---------------------------------------------------------------------------
activate() {
    local ver="$1"
    [[ -d "${RELEASES_DIR}/${ver}" ]] || die "目标版本未发布: ${ver}"
    PREV_VERSION="$(current_version || true)"

    log "step5a：原子换链 current -> releases/${ver}"
    switch_current "$ver"

    if ! assert_units; then
        if [[ -n "$PREV_VERSION" ]]; then
            warn "unit 断言失败，回切到 ${PREV_VERSION}"
            switch_current "$PREV_VERSION"
        fi
        die "unit 断言失败（ExecStart/WorkingDirectory 经 current 解析不可用）"
    fi
    log "step5b OK：unit 断言通过"

    ensure_transitional_links

    if have_systemd; then
        sync_units_and_runtime_perms
        reload_and_restart
        log "step5d：健康检查（最长 ${HEALTH_TIMEOUT}s）…"
        if ! health_check; then
            warn "健康检查失败 —— 自动回切上一版本"
            if [[ -n "$PREV_VERSION" ]]; then
                switch_current "$PREV_VERSION"
                # ★ V1.0.47 上板故障根因 2：回切只切了 current 软链、没同步旧版本的 systemd unit，
                #   导致 unit 停在「新版本的 ExecStart」而 current 已是旧版本 ⇒ exec not found
                #   循环、面板打不开。这里补上 sync_units_and_runtime_perms（此时 current 已切回
                #   PREV_VERSION，ensure 脚本会按 PREV_VERSION 的 unit 重新同步 /etc/systemd/system）。
                sync_units_and_runtime_perms
                reload_and_restart
                die "激活失败：已回切到 ${PREV_VERSION}（请检查 core IPC socket 与 web:${WEB_PORT}）"
            else
                die "激活失败且无上一版本可回切（首次部署）"
            fi
        fi
        log "step5d OK：健康检查通过"
    else
        warn "SKIP：无 systemd，跳过 daemon-reload/restart 与健康检查（WSL/容器环境）"
    fi
}

# ---------------------------------------------------------------------------
# 步骤 6：保留策略（成功后清理第 3 旧版本）
# ---------------------------------------------------------------------------
prune_versions() {
    local cur keep="$KEEP_VERSIONS"
    cur="$(current_version || true)"
    local -a versions=()
    mapfile -t versions < <(list_versions)
    local i=0 v
    for v in "${versions[@]}"; do
        i=$(( i + 1 ))
        if (( i > keep )); then
            if [[ "$v" == "$cur" ]]; then
                continue
            fi
            log "step6：清理旧版本 ${v}"
            rm -rf -- "${RELEASES_DIR}/${v}"
        fi
    done

    # step6b：循环清残留（*.staging / *.old.* 中间态）。list_versions 明确排除这两类，
    # 故上面的保留策略永远碰不到它们；浇筑失败/中断留下的残树会逐版累积占盘
    # （2026-09-30 板端实测：根分区被 logs/snap/残树挤到 100%）。
    local residue
    while IFS= read -r residue; do
        [[ -n "$residue" ]] || continue
        log "step6b：清理残留 $(basename -- "$residue")"
        rm -rf -- "$residue"
    done < <(find "$RELEASES_DIR" -mindepth 1 -maxdepth 1 -type d \
                \( -name '*.staging' -o -name '*.old.*' \) -print 2>/dev/null)
}

# ---------------------------------------------------------------------------
# --rollback：换指针回上一版本（D14：支持 --rollback <ver> 指定目标版本）
# ---------------------------------------------------------------------------
do_rollback() {
    local cur target=""
    local want_ver="${ROLLBACK_TO:-}"
    cur="$(current_version || true)"
    [[ -n "$cur" ]] || die "current 未指向任何版本，无法回滚"

    if [[ -n "$want_ver" ]]; then
        # 指定版本回滚：必须存在于 releases/ 且不是当前版本
        [[ "$want_ver" =~ ^[A-Za-z0-9._-]+$ ]] || die "非法版本号: $want_ver"
        [[ -d "${RELEASES_DIR}/${want_ver}" ]] || die "目标版本不存在: ${RELEASES_DIR}/${want_ver}"
        [[ "$want_ver" != "$cur" ]] || die "目标版本即当前版本（${cur}），无需回滚"
        target="$want_ver"
    else
        local -a versions=()
        mapfile -t versions < <(list_versions)
        local v
        for v in "${versions[@]}"; do
            [[ "$v" == "$cur" ]] && continue
            target="$v"
            break
        done
        [[ -n "$target" ]] || die "没有可回滚的上一版本（当前 ${cur}）"
    fi

    log "回滚：current ${cur} -> ${target}"
    switch_current "$target"
    reload_and_restart
    if have_systemd; then
        if health_check; then
            log "回滚后健康检查通过"
        else
            # D14（2026-09-18 定案 I-31）：回滚后仍不健康必须非零退出——
            # 调用方（OTA 更新器 / 运维）必须知道系统仍处于坏状态，不能假绿。
            die "回滚后健康检查未通过（已切到 ${target}，请人工介入）"
        fi
    fi
}

# ---------------------------------------------------------------------------
# main
# ---------------------------------------------------------------------------
main() {
    local cmd_rollback=0 activate_flag=0
    local ver="" payload=""
    ROLLBACK_TO=""
    while [[ $# -gt 0 ]]; do
        case "$1" in
            --activate) activate_flag=1; shift ;;
            --rollback)
                cmd_rollback=1; shift
                # D14：--rollback 可带可选版本参数（缺省回上一版本）
                if [[ $# -ge 1 && "$1" != -* ]]; then
                    ROLLBACK_TO="$1"; shift
                fi
                ;;
            --selftest-stall)
                # 双条件锁：还需环境 TTBOX_RELEASE_SELFTEST=1，见文件头说明
                if [[ "$SELFTEST_ENABLED" != "1" ]]; then
                    die "--selftest-stall 仅限自测：需同时设置 TTBOX_RELEASE_SELFTEST=1（生产不可达）"
                fi
                [[ $# -ge 2 && "$2" =~ ^[0-9]+$ ]] || die "--selftest-stall 需要一个正整数参数"
                SELFTEST_STALL="$2"
                shift 2
                ;;
            -h|--help) usage; exit 0 ;;
            -*) die "未知参数: $1" ;;
            *)
                if [[ -z "$ver" ]]; then
                    ver="$1"
                elif [[ -z "$payload" ]]; then
                    payload="$1"
                else
                    die "多余参数: $1"
                fi
                shift
                ;;
        esac
    done

    if (( cmd_rollback )); then
        do_rollback
        exit 0
    fi

    [[ -n "$ver" && -n "$payload" ]] || { usage; exit 2; }
    [[ "$ver" =~ ^[A-Za-z0-9._-]+$ ]] || die "非法版本号: $ver（仅允许 [A-Za-z0-9._-]）"
    [[ -d "$payload" ]] || die "payload 目录不存在: $payload"

    log "安装版本 ${ver}  ← payload ${payload}"
    log "前缀 TTBOX_PREFIX=${TTBOX_PREFIX}  ETC=${TTBOX_ETC}  RUN=${TTBOX_RUN}"

    mkdir -p -- "$RELEASES_DIR"
    # OTA 特权通道任务目录（2026-09-18 定案 C-10）：root:ttbox 0770 ——
    # web（User=ttbox）只往这里丢任务文件，root 更新器消费；权限面就这一个目录。
    if have_systemd; then
        mkdir -p -- /var/lib/ttbox/ota/jobs
        chown root:ttbox /var/lib/ttbox/ota/jobs 2>/dev/null || true
        chmod 0770 /var/lib/ttbox/ota/jobs 2>/dev/null || true
        mkdir -p -- /opt/ttbox/state
    fi
    local staging="${RELEASES_DIR}/${ver}.staging"

    validate_payload_manifest "$payload"
    stage_payload "$payload" "$staging"

    if (( SELFTEST_STALL > 0 )); then
        warn "[selftest] staging 完成，人为停顿 ${SELFTEST_STALL}s（--selftest-stall，生产不可达）"
        sleep "$SELFTEST_STALL"
    fi

    verify_staging "$staging" "${payload}/RELEASE_MANIFEST.json"
    publish_release "$ver"

    # 1.5.22：出厂 DTB 里 HDMI-RX 是 disabled ⇒ /dev/video0 不存在、HDMI 采集不可用。
    # 放在 publish 之后、activate 之前：DTB 属于启动链，与运行树切换无关；
    # 且脚本恒退出 0 —— 绝不允许它把一次 OTA 判成失败（失败的后果比不修更糟）。
    # 注意：替换的是文件，**必须重启才生效**（u-boot 开机才读 DTB）。
    if [[ -x "${RELEASES_DIR}/${ver}/scripts/ttbox_dtb_fix.sh" ]]; then
        "${RELEASES_DIR}/${ver}/scripts/ttbox_dtb_fix.sh" \
            || warn "ttbox_dtb_fix.sh 返回非 0（已忽略，不阻断安装）"
    else
        warn "payload 缺 scripts/ttbox_dtb_fix.sh，跳过 DTB 修复"
    fi

    if (( activate_flag )); then
        activate "$ver"
        prune_versions
    else
        log "未指定 --activate：仅发布，不切换 current"
    fi

    log "完成。version=${ver} current=$(current_version || echo '<none>')"
}

main "$@"
