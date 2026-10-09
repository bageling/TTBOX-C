#!/usr/bin/env bash
# ttbox_git_mode_check.sh — 可执行位门禁（读 **git 索引**，非工作区）· T1.06 / DEP-04
#
# 背景：Windows / 交叉平台上 git 检出会丢 +x；若 `git add` 时未配 `--chmod=+x`，
#       索引里会以 **100644** 入库 ⇒ 新机器 clone 后板端脚本不可执行（T1.06 零手工 chmod）。
# 本脚本读 `git ls-files -s` 的**索引模式**，断言"必须可执行"集合一律为 **100755**，
# 且**反向闭集**：索引中不存在 allowlist 之外的 100755（防"新增可执行文件被静默放过"）。
#
# ★ 为什么读**索引**而非工作区：Windows 工作区文件位本就不由 git 管，要锁的是
#   **入库的那一位**——它才是 clone / 部署 / `tar` 后生效的模式。工作区 +x 在这里不可信。
#
# ★ 运行环境：**仅 Linux / WSL**。
#   Windows 原生 Git Bash 不可用：`pwd`/`cd` 产出 MSYS 形式路径（`/c/Users/...`），而
#   `git -C` 需要 Windows 形式（`C:/Users/...`）⇒ git 报 `fatal: cannot change to ...`。
#   本脚本据此**以 exit 2（环境错误）退出**，绝不把它误报成 [FAIL]（契约：exit 2 = 环境错误）。
#
# 用法：
#   scripts/ttbox_git_mode_check.sh            # 在仓库内跑（自动定位仓库根）
#   scripts/ttbox_git_mode_check.sh <repo>     # 指定仓库根
# 退出码：0 = 全部为 100755 且闭集外 0 个；1 = 存在违规；2 = 用法 / 环境错误。

set -uo pipefail

REPO="${1:-}"
if [ -z "$REPO" ]; then
    REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
fi

rc=0
fail() { printf '[mode][FAIL] %s\n' "$*"; rc=1; }
ok()   { printf '[mode][ OK ] %s\n' "$*"; }
info() { printf '[mode] %s\n' "$*"; }
err()  { printf '[mode][ERROR] %s\n' "$*" >&2; exit 2; }

[ -d "$REPO/.git" ] || err "非 git 仓库（无 .git）：$REPO"
command -v git >/dev/null 2>&1 || err "缺 git"

# ★ 环境自检：`git -C` 必须能定位仓库。Windows Git Bash 下 $REPO 为 MSYS 路径，git 会
#   `fatal: cannot change to ...` ⇒ 判**环境错误**（exit 2），而非误报 FAIL。
git -C "$REPO" rev-parse --git-dir >/dev/null 2>&1 || \
    err "git 无法定位仓库（路径形式？Windows Git Bash 请在 WSL 运行）：$REPO"

# 必须可执行的入库文件范围（相对仓库根；git pathspec，`*` 可跨 `/`）：
#   - scripts/**/*.sh     板端 / 部署 / 维护脚本（含 edid、a9_*、lan_blocklist、ttbox_*）
#   - usbproxy/board/*.sh usbproxy 板端启动脚本
#   - usbproxy/usb-proxy  预编译二进制（T1.06④ 随包交付）
#   - plugins/*/bin/*     插件可执行入口（shebang 脚本或二进制）
EXEC_GLOBS=(
    "scripts/*.sh"
    "scripts/*/*.sh"
    "usbproxy/board/*.sh"
    "usbproxy/usb-proxy"
    "plugins/*/bin/*"
)

info "仓库：$REPO"
info "判据：以下集合在 **索引** 中必须为 100755（否则 clone 后不可执行）"

# 索引快照：path -> mode（stage 0）
declare -A IDX_MODE=()
while read -r m p; do
    [ -n "${p:-}" ] || continue
    IDX_MODE["$p"]="$m"
done < <(git -C "$REPO" ls-files -s | awk '{print $1" "$4}')

# 收集 allowlist 命中。★ **必须去重**：git pathspec 的 `*` 会跨 `/`，同一文件可能被
#   多个 glob 同时命中（如 scripts/edid/x.sh 命中 `scripts/*.sh` 与 `scripts/*/*.sh`），
#   若按 glob 累加计数会重复（曾把 35 误报成 37）。
declare -A ALLOW=()
for g in "${EXEC_GLOBS[@]}"; do
    while IFS= read -r p; do
        [ -n "$p" ] || continue
        ALLOW["$p"]=1
    done < <(git -C "$REPO" ls-files -- "$g")
done

n=${#ALLOW[@]}
if [ "$n" -eq 0 ]; then
    fail "可执行集合为空（通配未匹配到任何入库文件）—— 判据失效"
fi

# 正向：allowlist 内每个文件都必须是 100755
for p in "${!ALLOW[@]}"; do
    m="${IDX_MODE[$p]:-}"
    if [ -z "$m" ]; then
        fail "索引中不存在（未入库 / 路径错）：$p"
    elif [ "$m" = "100755" ]; then
        ok "$p = 100755"
    else
        fail "$p = $m（应为 100755；修复：git update-index --chmod=+x -- \"$p\"）"
    fi
done

# 反向闭集：索引中**不允许** allowlist 之外的 100755；顺带统计索引里 100755 总数。
#   防漂移：将来有人在 deploy/ 或别处新增一个可执行文件，门禁**照样绿** —— 闭集才拦得住。
idx755=0; stray=0
for p in "${!IDX_MODE[@]}"; do
    m="${IDX_MODE[$p]}"
    [ "$m" = "100755" ] || continue
    idx755=$((idx755+1))
    if [ -z "${ALLOW[$p]:-}" ]; then
        stray=$((stray+1))
        fail "allowlist 外的 100755：$p（应纳入 EXEC_GLOBS 或改回 644）"
    fi
done

# ★ 反向闭集（lib 二进制）：lib/ 下**不得**有任何入库的 .so*（T1.18 / lib-scope-ruling.md §6）。
#   release 作用域库现由 payload（releases/<ver>/lib/）提供；仓库副本（librknnrt.so 1.5.2 过期残留）
#   已删 —— `git ls-files 'lib/*.so*'` 必须为空：一旦有人误把仓库副本 add 回索引，本门禁立刻 FAIL。
#   契约：本脚本 exit 2 仅表示用法/环境错误；此处一律用 fail()（rc=1），绝不用 2 表示 FAIL。
libso=0
while IFS= read -r p; do
    [ -n "$p" ] || continue
    libso=$((libso+1))
    fail "lib/ 下不允许入库的 .so* ：$p（作用域见 lib-scope-ruling.md §6；应从索引移除）"
done < <(git -C "$REPO" ls-files -- 'lib/*.so*')
[ "$libso" -eq 0 ] && ok "lib/ 无入库 .so*（release 作用域库由 payload 提供）"

echo "------------------------------------------------------------"
info "索引中 100755 共 ${idx755} 个；allowlist 命中 ${n} 个（已去重）；闭集外 ${stray} 个；lib/*.so* 入库 ${libso} 个"
if [ "$rc" -eq 0 ]; then
    echo "[mode] RESULT: PASS（${n} 个入库可执行文件均为 100755；闭集外 0；lib/*.so* 入库 0）"
else
    echo "[mode] RESULT: FAIL"
fi
exit "$rc"
