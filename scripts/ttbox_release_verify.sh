#!/usr/bin/env bash
# ttbox_release_verify.sh — TTBOX 发布独立校验器（只读，退出码表结论）
#
# 用途：对某个已发布的 release 树做"可回滚性"体检，任何一项不满足即退出非 0。
# 用法：
#   ttbox_release_verify.sh [<ver>]        # 缺省校验 current 指向的版本
#
# 校验项（design §B1.4 / §B4 / tasks.md T1.01 验收）：
#   1. RELEASE_MANIFEST.json 存在，且 files_sha256 全量比对通过
#   2. 每个 unit 的 ExecStart 首 token 经 current 解析后 -x（存在且可执行）
#   3. 每个 unit 的 WorkingDirectory 存在且 -d
#   4. RUNPATH 自包含断言：release 内 ELF 的 RUNPATH 必须以 $ORIGIN 起头
#      （出现 /opt/ttbox/lib 这类跨版本绝对路径即失败——否则"换指针回滚"是假回滚）
#   5. ldd 中设备专有库解析路径必须落在本 release 的 lib/ 内
#   6. StartLimit 断言（按风险规则）：任何 Restart= 不为 no 的 unit 必须在 [Unit] 段
#      显式声明 StartLimitBurst=5 / StartLimitIntervalSec=300；Restart=no（含 oneshot）
#      允许缺省。任何 unit 一旦声明 StartLimit* 则校验取值与段位
#      （写在 [Service] 段 systemd 会静默忽略）
#   7. 迁移期过渡软链断言：TRANSITIONAL_LINKS 清单逐条断言 可解析 + 指向本 release 内
#
# 退出码：0 = 全部通过；1 = 存在失败项；2 = 用法/环境错误
#
set -uo pipefail

TTBOX_PREFIX="${TTBOX_PREFIX:-/opt/ttbox}"
TTBOX_RUN="${TTBOX_RUN:-/run/ttbox}"

RELEASES_DIR="${TTBOX_PREFIX}/releases"
CURRENT_LINK="${TTBOX_PREFIX}/current"
UNIT_REL_DIR="deploy/systemd"

# ★ 迁移期过渡软链清单——与 scripts/ttbox_release_install.sh 的 TRANSITIONAL_LINKS
#   保持同步（install 浇筑、本脚本逐条断言）。断链/指向 release 外即 FAIL：
#   目的是抓住"新增一个硬编码路径却忘了补软链"的回归。
TRANSITIONAL_LINKS=(
    "plugins:current/plugins"   # 插件发现 + 旧 web unit 路径
    "scripts:current/scripts"   # edid 工具链 import（wifi_manager / lan_blocklist 已移出）
)

# 设备专有库名单：其解析路径必须落在本 release 的 lib/ 内（可覆盖）
DEVICE_LIBS="${TTBOX_DEVICE_LIBS:-librknnrt.so}"

rc=0
fail() { printf '[verify][FAIL] %s\n' "$*"; rc=1; }
ok()   { printf '[verify][ OK ] %s\n' "$*"; }
warn() { printf '[verify][WARN] %s\n' "$*"; }
info() { printf '[verify] %s\n' "$*"; }

remap_path() {
    local p="$1"
    if [[ "$TTBOX_PREFIX" != "/opt/ttbox" ]]; then
        printf '%s' "${p/#\/opt\/ttbox/$TTBOX_PREFIX}"
    else
        printf '%s' "$p"
    fi
}

sha256_of() { sha256sum -- "$1" | cut -d' ' -f1; }

manifest_pairs() {
    python3 - "$1" <<'PY'
import json, sys
with open(sys.argv[1], "r", encoding="utf-8") as fh:
    m = json.load(fh)
for path, digest in sorted((m.get("files_sha256") or {}).items()):
    print(f"{digest}\t{path}")
PY
}

# ---------------------------------------------------------------------------
# 解析目标 release 目录
# ---------------------------------------------------------------------------
VER="${1:-}"
if [[ -z "$VER" ]]; then
    if [[ -L "$CURRENT_LINK" ]]; then
        VER="$(basename -- "$(readlink -f -- "$CURRENT_LINK")")"
    else
        echo "[verify][ERROR] current 软链不存在且未指定 <ver>" >&2
        exit 2
    fi
fi

RELEASE_DIR="${RELEASES_DIR}/${VER}"
if [[ ! -d "$RELEASE_DIR" ]]; then
    echo "[verify][ERROR] release 目录不存在: $RELEASE_DIR" >&2
    exit 2
fi
info "校验版本 ${VER}  ->  ${RELEASE_DIR}"

# ---------------------------------------------------------------------------
# 1. manifest + 全量 sha256
# ---------------------------------------------------------------------------
MANIFEST="${RELEASE_DIR}/RELEASE_MANIFEST.json"
if [[ ! -f "$MANIFEST" ]]; then
    fail "缺少 RELEASE_MANIFEST.json"
else
    n=0; bad=0
    while IFS=$'\t' read -r digest path; do
        [[ -z "$path" ]] && continue
        n=$(( n + 1 ))
        fp="${RELEASE_DIR}/${path}"
        if [[ ! -f "$fp" ]]; then
            fail "manifest 声明文件缺失: ${path}"
            bad=$(( bad + 1 ))
            continue
        fi
        actual="$(sha256_of "$fp")"
        if [[ "$actual" != "$digest" ]]; then
            fail "sha256 不符: ${path}"
            bad=$(( bad + 1 ))
        fi
    done < <(manifest_pairs "$MANIFEST")
    if (( n == 0 )); then
        fail "manifest files_sha256 为空"
    elif (( bad == 0 )); then
        ok "manifest 全量 sha256 通过（${n} 项）"
    fi
fi

# ---------------------------------------------------------------------------
# 2 & 3. unit 断言（ExecStart 首 token -x / WorkingDirectory -d）
# ---------------------------------------------------------------------------
UNIT_DIR="${RELEASE_DIR}/${UNIT_REL_DIR}"
if [[ ! -d "$UNIT_DIR" ]]; then
    fail "release 内缺少 unit 目录: ${UNIT_REL_DIR}"
else
    for f in "$UNIT_DIR"/*.service; do
        [[ -f "$f" ]] || continue
        unit="$(basename -- "$f")"

        exec_raw="$(awk -F= '/^ExecStart=/{sub(/^ExecStart=/,"");print;exit}' "$f")"
        exec_raw="${exec_raw#-}"; exec_raw="${exec_raw#@}"
        exec_raw="${exec_raw#+}"; exec_raw="${exec_raw#!}"
        exec_bin="${exec_raw%% *}"
        exec_bin="$(remap_path "$exec_bin")"
        if [[ -x "$exec_bin" ]]; then
            ok "${unit}: ExecStart 可执行 (${exec_bin})"
        else
            fail "${unit}: ExecStart 首 token 不可执行: ${exec_bin}"
        fi

        wd="$(awk -F= '/^WorkingDirectory=/{sub(/^WorkingDirectory=/,"");print;exit}' "$f")"
        if [[ -n "$wd" ]]; then
            wd="$(remap_path "$wd")"
            if [[ -d "$wd" ]]; then
                ok "${unit}: WorkingDirectory 存在 (${wd})"
            else
                fail "${unit}: WorkingDirectory 不存在: ${wd}"
            fi
        fi
    done
fi

# ---------------------------------------------------------------------------
# 4. RUNPATH 自包含断言
# ---------------------------------------------------------------------------
check_runpath_file() {
    local f="$1" rp seg oldifs raw
    file -b -- "$f" 2>/dev/null | grep -q ELF || return 0   # 非 ELF 跳过（§7.2）
    # 先取到 RUNPATH/RPATH 的**原始行**，据此区分两种"拿不到值"，两者都 FAIL 但文案分开：
    #   (a) 根本没有 RUNPATH/RPATH 段        → "缺失"
    #   (b) 段存在但值为空串（显示 []）      → "为空串"
    # （旧实现把两者都报"缺失"：功能对，但定位难——E1 要求区分。）
    raw="$(readelf -d -- "$f" 2>/dev/null | grep -E '\((RUNPATH|RPATH)\)' | head -1)"
    if [[ -z "$raw" ]]; then
        fail "RUNPATH 缺失（无 RUNPATH/RPATH 段，要求 \$ORIGIN 自包含）: ${f#"$RELEASE_DIR"/}"
        return 1
    fi
    rp="$(printf '%s' "$raw" | sed -n 's/.*\[\(.*\)\].*/\1/p')"
    if [[ -z "$rp" ]]; then
        fail "RUNPATH 段存在但为空串（[]，ld.so 语义下不等价于 \$ORIGIN 自包含）: ${f#"$RELEASE_DIR"/}"
        return 1
    fi
    # ★ E1 空段显式拒绝：`for seg in $rp`（IFS=':'）对**尾随单个冒号**不产生空字段
    #   ⇒ `$ORIGIN/../lib:` 会被漏放。空段在 ld.so 语义里等于"回退当前工作目录"，属实质风险。
    #   首冒号 / 尾冒号 / 连续冒号（含整体仅一个 ':'）都必须判 FAIL。
    case "$rp" in
        :*|*:|*::*)
            fail "RUNPATH 含空段（首/尾/连续冒号；ld.so 语义下空段=回退 CWD）: ${f#"$RELEASE_DIR"/} => [${rp}]"
            return 1
            ;;
    esac
    # ★ T1.16 §2.2 逐段校验：RUNPATH 按 ':' 拆开，**每一段**都必须以 $ORIGIN/${ORIGIN}
    #   起头；出现任何绝对路径段（如 /opt/ttbox/lib）即 FAIL。
    #   旧实现用 `case "$rp" in \$ORIGIN*` **只匹配整串前缀** ⇒ 形态 B
    #   （$ORIGIN/../lib:/opt/ttbox/lib）也被放行（假回滚漏网）——逐段后尾段必被判 FAIL。
    oldifs="$IFS"; IFS=':'
    for seg in $rp; do
        case "$seg" in
            '$ORIGIN'|'$ORIGIN'/*|'${ORIGIN}'|'${ORIGIN}'/*) : ;;   # 合法段
            *)
                IFS="$oldifs"
                fail "RUNPATH 段 '${seg}' 非 \$ORIGIN 起头（含跨版本/绝对路径）: ${f#"$RELEASE_DIR"/} => [${rp}]"
                return 1
                ;;
        esac
    done
    IFS="$oldifs"
    ok "RUNPATH 自包含（逐段）: ${f#"$RELEASE_DIR"/}  =>  ${rp}"
    return 0
}

BIN_DIR="${RELEASE_DIR}/bin"
if [[ -d "$BIN_DIR" ]]; then
    found=0
    while IFS= read -r f; do
        found=1
        check_runpath_file "$f"
    done < <(find "$BIN_DIR" -maxdepth 1 -type f | sort)
    (( found )) || warn "bin/ 下没有文件，RUNPATH 断言跳过"
else
    warn "release 无 bin/ 目录，RUNPATH 断言跳过"
fi

# ---------------------------------------------------------------------------
# 4b.（E2）bin/ 显式闭集判据：bin/ 只允许 ttbox_core_main
# ---------------------------------------------------------------------------
# 为什么必须把这条写在**执法点**（本 verify），而不是靠 sync_tree 的隐式行为：
#   - T1.16 的 RUNPATH 属性只加在 ttbox_core_main 上；ipc_ping / imgdetect 在任何构建里
#     都**没有任何 RUNPATH**。sync_tree 的 bin/ 闭集目前恰为 {ttbox_core_main}，所以它们
#     暂不发货——但那是**隐式**的。
#   - deploy/RELEASE_MANIFEST.template.json 曾列 `bin/ipc_ping`。一旦有人"按模板对齐"把
#     无 RUNPATH 的 ELF 纳入发货，上面的 RUNPATH 检查会把它判 FAIL（好），可若检查被人
#     误删/绕过，就没有第二道护栏。
#   - 判据写在执法点，才能防住"旁人改 sync 列表"。故显式点名闭集成员。
# Web 迁 C++（V1.0.47）后 bin/ 闭集扩为两成员：ttbox_core_main + ttbox_web。
#   ttbox_web 是纯 HTTP 壳 + IPC 客户端，同样带 RUNPATH \$ORIGIN（同 ttbox_core_main 口径）。
# ★ V1.0.53 起扩为三成员：+ ttbox_edid（EDID 应用入口由 shell 换成 C++ 二进制），
#   同样是带 \$ORIGIN RUNPATH 的 ELF。
BIN_ALLOWED=("ttbox_core_main" "ttbox_web" "ttbox_edid")
if [[ -d "$BIN_DIR" ]]; then
    while IFS= read -r f; do
        [[ -f "$f" ]] || continue
        bnm="$(basename -- "$f")"
        allowed=0
        for a in "${BIN_ALLOWED[@]}"; do [[ "$bnm" == "$a" ]] && allowed=1; done
        if [[ "$allowed" == 1 ]]; then
            ok "bin/ 闭集成员: ${bnm}"
        else
            fail "bin/ 出现非闭集成员: ${bnm}（bin/ 只允许 ${BIN_ALLOWED[*]}）"
        fi
    done < <(find "$BIN_DIR" -maxdepth 1 -type f | sort)
fi

# ---------------------------------------------------------------------------
# 5. 设备专有库解析路径必须落在本 release 的 lib/ 内
# ---------------------------------------------------------------------------
check_device_libs_file() {
    local f="$1"
    file -b -- "$f" 2>/dev/null | grep -q ELF || return 0
    local out lib line resolved rel
    # ★ 必须先 canon 再比前缀：ldd 回显的是 $ORIGIN **展开后的字面量**，形如
    #   /opt/ttbox/releases/<ver>/bin/./../lib/librknnrt.so —— 带 ".." 的路径拿去
    #   和 `${RELEASE_DIR}/lib` 做字符串前缀比较，healthy release 也会被判"release 外"
    #   （板端实测 rc=1 假阳性）。readlink -f 归一化两边后再比。
    rel="$(readlink -f -- "${RELEASE_DIR}/lib" 2>/dev/null || printf '%s' "${RELEASE_DIR}/lib")"
    out="$( ( cd "$(dirname -- "$f")" && ldd "$(basename -- "$f")" ) 2>/dev/null || true)"
    for lib in $DEVICE_LIBS; do
        line="$(printf '%s\n' "$out" | grep -F "$lib" || true)"
        if [[ -z "$line" ]]; then
            warn "${f#"$RELEASE_DIR"/}: ldd 未解析到设备库 ${lib}（可能未链接，跳过）"
            continue
        fi
        resolved="$(printf '%s' "$line" | sed -n 's/.*=> \(.*\) (0x.*/\1/p')"
        if [[ -z "$resolved" ]]; then
            fail "${f#"$RELEASE_DIR"/}: 设备库 ${lib} 未解析（${line}）"
            continue
        fi
        # 归一化（消掉 ./ 与 ../）后再比前缀，见上方注释
        resolved="$(readlink -f -- "$resolved" 2>/dev/null || printf '%s' "$resolved")"
        case "$resolved" in
            "$rel"/*)
                ok "设备库 ${lib} 解析落在本 release lib/ 内: ${resolved}"
                ;;
            *)
                fail "设备库 ${lib} 解析到 release 外: ${resolved}（应在本 release lib/ 内）"
                ;;
        esac
    done
}

if [[ -d "$BIN_DIR" ]]; then
    while IFS= read -r f; do
        check_device_libs_file "$f"
    done < <(find "$BIN_DIR" -maxdepth 1 -type f | sort)
fi

# ---------------------------------------------------------------------------
# 6. StartLimit* 值 + 段位断言（必须在 [Unit] 段）
# ---------------------------------------------------------------------------
check_startlimit() {
    local f="$1" unit
    unit="$(basename -- "$f")"
    # 规则（按风险，不按 unit 名豁免）：任何 Restart= 不为 no 的 unit（即会自动重启、
    # 有"失败重启无限刷"风险的）必须在 [Unit] 段显式声明 StartLimitBurst=5 与
    # StartLimitIntervalSec=300；Restart=no 或未声明（systemd 默认即 no，含 oneshot）
    # 允许缺省。一旦任何 unit 声明了 StartLimit*，则无论 Restart 为何都校验段位与取值。
    local urestart
    urestart="$(awk -F= '/^Restart=/{sub(/^Restart=/,"");print;exit}' "$f")"
    local needs_limit=0
    if [[ -n "$urestart" && "$urestart" != "no" ]]; then
        needs_limit=1
    fi

    local burst="" interval="" burst_sec="" interval_sec=""
    # awk 跟踪当前所在段，分别取出 [Unit] 段与错段的值
    local out
    out="$(awk '
        /^\[/ { sec=$0; next }
        /^StartLimitBurst=/ {
            v=substr($0, index($0,"=")+1)
            if (sec=="[Unit]") print "burst_unit=" v; else print "burst_wrong=" v " in " sec
        }
        /^StartLimitIntervalSec=/ {
            v=substr($0, index($0,"=")+1)
            if (sec=="[Unit]") print "int_unit=" v; else print "int_wrong=" v " in " sec
        }
    ' "$f")"

    burst="$(printf '%s\n' "$out" | sed -n 's/^burst_unit=//p')"
    interval="$(printf '%s\n' "$out" | sed -n 's/^int_unit=//p')"
    burst_sec="$(printf '%s\n' "$out" | sed -n 's/^burst_wrong=//p')"
    interval_sec="$(printf '%s\n' "$out" | sed -n 's/^int_wrong=//p')"

    [[ -n "$burst_sec" ]]    && fail "${unit}: StartLimitBurst 不在 [Unit] 段（${burst_sec}，systemd 会静默忽略）"
    [[ -n "$interval_sec" ]] && fail "${unit}: StartLimitIntervalSec 不在 [Unit] 段（${interval_sec}，systemd 会静默忽略）"

    if (( ! needs_limit )) && [[ -z "$burst" && -z "$interval" ]]; then
        ok "${unit}: StartLimit* N/A（Restart=${urestart:-no}，不自动重启，无刷屏风险）"
        return 0
    fi

    if [[ "$burst" == "5" ]]; then
        ok "${unit}: StartLimitBurst=5 且位于 [Unit]"
    else
        fail "${unit}: StartLimitBurst 期望 5，实测 '${burst:-<缺失>}'（[Unit] 段）"
    fi
    if [[ "$interval" == "300" ]]; then
        ok "${unit}: StartLimitIntervalSec=300 且位于 [Unit]"
    else
        fail "${unit}: StartLimitIntervalSec 期望 300，实测 '${interval:-<缺失>}'（[Unit] 段）"
    fi
}

if [[ -d "$UNIT_DIR" ]]; then
    for f in "$UNIT_DIR"/*.service; do
        [[ -f "$f" ]] || continue
        check_startlimit "$f"
    done
fi

# ---------------------------------------------------------------------------
# 7. 迁移期过渡软链断言：逐条断言 可解析 + 指向本 release 内
# ---------------------------------------------------------------------------
check_transitional_links() {
    # 参照系 = current 实际指向的 release（过渡软链经 current 解析，属全局状态）。
    # 校验 current 版本时参照系即 RELEASE_DIR；校验未激活版本时软链指向 current 属预期。
    local current_release
    current_release="$(readlink -f -- "$CURRENT_LINK" 2>/dev/null || true)"
    if [[ -z "$current_release" ]]; then
        fail "current 软链断链，过渡软链断言跳过（先修 current）"
        return 0
    fi
    local entry link resolved
    for entry in "${TRANSITIONAL_LINKS[@]}"; do
        link="${TTBOX_PREFIX}/${entry%%:*}"
        if [[ ! -L "$link" ]]; then
            fail "过渡软链缺失或不是软链: ${link}（应 -> ${entry#*:}）"
            continue
        fi
        # readlink -f 要求全路径组件存在：断链/目标缺失返回空
        resolved="$(readlink -f -- "$link" 2>/dev/null || true)"
        if [[ -z "$resolved" ]]; then
            fail "过渡软链断链（无法解析）: ${link} -> $(readlink -- "$link")"
            continue
        fi
        case "$resolved" in
            "$current_release"/*)
                ok "过渡软链: ${link} -> ${resolved}"
                ;;
            *)
                fail "过渡软链指向 current release 之外: ${link} -> ${resolved}（应在 ${current_release}/ 内）"
                ;;
        esac
    done
}

check_transitional_links

# ---------------------------------------------------------------------------
# 8. 口径门禁：配置 · 常量 · 路径「无补丁」回归（Task #2 / T-E）
# ---------------------------------------------------------------------------
# 定性：口径门禁是**源码树级**检查（需 core/src、plugins/web/lib 等源码在场以断言
#   单点真源）。本 verify 也常跑在板端 release 树（只有 bin/ + plugins/ 打包态），
#   故仅在**源码树在场**时执行；否则跳过（不影响"换指针回滚"体检）。
#   跳过开关：TTBOX_SKIP_CONVENTIONS_GATE=1。详见 scripts/ttbox_conventions_gate.sh。
GATE_SH="$(cd "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)/ttbox_conventions_gate.sh"
if [[ "${TTBOX_SKIP_CONVENTIONS_GATE:-0}" == "1" ]]; then
    warn "口径门禁已按 TTBOX_SKIP_CONVENTIONS_GATE=1 跳过"
elif [[ -f "$GATE_SH" && -f "$(dirname -- "$GATE_SH")/../core/src/common/Paths.hpp" ]]; then
    if gate_out="$(bash "$GATE_SH" 2>&1)"; then
        ok "口径门禁 PASS（scripts/ttbox_conventions_gate.sh）"
    else
        fail "口径门禁 FAIL（scripts/ttbox_conventions_gate.sh）"
        printf '%s\n' "$gate_out" | sed 's/^/    /'
    fi
else
    info "口径门禁跳过（源码树不在场 —— 板端 release 树的正常情形）"
fi

# ---------------------------------------------------------------------------
# 结论
# ---------------------------------------------------------------------------
echo "------------------------------------------------------------"
if (( rc == 0 )); then
    echo "[verify] RESULT: PASS (version ${VER})"
else
    echo "[verify] RESULT: FAIL (version ${VER})"
fi
exit "$rc"
