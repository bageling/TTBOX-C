#!/usr/bin/env bash
# ttbox_ensure_selftest.sh — ttbox_ensure_services.sh 自测（T1.03 契约 + T5b 截断回归锁）
#
# 契约来源：design §B3.1（ensure 六条）+ QA T5b（ENOSPC / 写限 ⇒ **半截 unit 不得安装**）。
# 覆盖：T1 非 root→exit0 不写；T2 current 断链→exit0 不写；T3 首装 cmp -s；T4 幂等；T5 漂移自愈；
#       T6 写限（ulimit -f）下**半截 unit 不得安装**；T7 不变量锁：**既有完整 unit 不得因写失败被销毁**。
# 运行环境：Linux / WSL，**需 root**（ensure 非 root 会提前 exit 0；T2..T7 需真写盘）。
#   非 root 时：T1 照跑；T2..T7 打印醒目 `[SKIP]` 并计入 SKIP，且**总结行显式声明"不可作为 T1.03 通过证据"**。
# 安全：全部在 mktemp 临时前缀内，**绝不触碰** /opt/ttbox 或 /etc/systemd/system。
# 独立性：本自测面向 T1.03 的 ensure 交付件，与 T1.01 的 release 布局自测（ttbox_release_selftest.sh）分属不同交付物。
# 退出码：0 = 全过；1 = 有 FAIL。
set -u

REPO="${TTBOX_REPO:-}"
if [ -z "$REPO" ]; then REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"; fi
ENSURE_SRC="$REPO/scripts/ttbox_ensure_services.sh"
[ -f "$ENSURE_SRC" ] || { echo "找不到 $ENSURE_SRC" >&2; exit 2; }

WORK="$(mktemp -d /tmp/ttbox-ensure-selftest-XXXXXX)"
# T1 用 `su nobody` 验证"非 root → exit 0"：mktemp 默认 0700，nobody 穿不进来会得 rc=126（假 FAIL）⇒ 放开遍历位。
chmod 0755 "$WORK"
ENSURE="$WORK/ensure.sh"; sed 's/\r$//' "$ENSURE_SRC" > "$ENSURE"; chmod +x "$ENSURE"

PASS=0; FAIL=0; SKIPPED=0; declare -a FAILED=()
record() { if [ "$2" -eq 0 ]; then PASS=$((PASS+1)); printf '[ OK ] %s\n' "$1"; else FAIL=$((FAIL+1)); FAILED+=("$1"); printf '[FAIL] %s  %s\n' "$1" "${3:-}"; fi; }
skip()   { SKIPPED=$((SKIPPED+1)); printf '===== [SKIP] %s =====\n' "$1"; }

make_pfx() { # $1=pfx ：建 current -> releases/1.0.0/deploy/systemd + 一个合法 unit（ExecStart 可执行）
  local p="$1"
  rm -rf "$p"; mkdir -p "$p/releases/1.0.0/deploy/systemd"
  printf '[Unit]\nDescription=selftest\n\n[Service]\nExecStart=/bin/true\n' \
    > "$p/releases/1.0.0/deploy/systemd/ttbox-core.service"
  ln -sfn releases/1.0.0 "$p/current"
}
SRC_UNIT() { printf '%s/releases/1.0.0/deploy/systemd/ttbox-core.service' "$1"; }

echo "== REPO=$REPO =="
echo "== WORK=$WORK =="
echo "== uid=$(id -u)（T3..T6 需 0）=="

# ---- T1 非 root → exit 0 且不写任何 unit（用 su nobody；无 su/nobody 则 SKIP）----
PFX1="$WORK/pfx1"; UNITS1="$WORK/units1"; make_pfx "$PFX1"; mkdir -p "$UNITS1"; chmod 0777 "$UNITS1"
if [ "$(id -u)" -eq 0 ] && command -v su >/dev/null 2>&1 && id nobody >/dev/null 2>&1; then
  chmod 0755 "$PFX1" "$PFX1/releases" "$PFX1/releases/1.0.0" 2>/dev/null || true
  su -s /bin/bash nobody -c "TTBOX_PREFIX='$PFX1' TTBOX_UNIT_DIR='$UNITS1' TTBOX_SYSTEMD=0 bash '$ENSURE'" > "$WORK/t1.log" 2>&1; rc=$?
  [ $rc -eq 0 ] && [ -z "$(ls -A "$UNITS1" 2>/dev/null)" ]
  record "T1 非 root：exit 0 且不写 unit" $? "rc=$rc"
else
  skip "T1：非 root 或 无 su/nobody，无法验证非 root 契约"
fi

if [ "$(id -u)" -eq 0 ]; then
  # ---- T2 current 断链 → exit 0 且不写 ----
  PFX2="$WORK/pfx2"; UNITS2="$WORK/units2"; make_pfx "$PFX2"; mkdir -p "$UNITS2"
  rm -f "$PFX2/current"
  TTBOX_PREFIX="$PFX2" TTBOX_UNIT_DIR="$UNITS2" TTBOX_SYSTEMD=0 bash "$ENSURE" > "$WORK/t2.log" 2>&1; rc=$?
  [ $rc -eq 0 ] && [ -z "$(ls -A "$UNITS2" 2>/dev/null)" ]
  record "T2 current 断链：exit 0 且不写 unit" $? "rc=$rc"

  # ---- T3 首次安装：写入且与源逐字节一致 ----
  PFX3="$WORK/pfx3"; UNITS3="$WORK/units3"; make_pfx "$PFX3"; mkdir -p "$UNITS3"; SRC3="$(SRC_UNIT "$PFX3")"
  TTBOX_PREFIX="$PFX3" TTBOX_UNIT_DIR="$UNITS3" TTBOX_SYSTEMD=0 bash "$ENSURE" > "$WORK/t3.log" 2>&1; rc=$?
  [ $rc -eq 0 ] && [ -f "$UNITS3/ttbox-core.service" ] && cmp -s "$UNITS3/ttbox-core.service" "$SRC3"
  record "T3 首次安装：写入且 cmp -s 源一致" $? "rc=$rc"

  # ---- T4 幂等重跑：内容不变 ----
  before="$(sha256sum "$UNITS3/ttbox-core.service" | cut -d' ' -f1)"
  TTBOX_PREFIX="$PFX3" TTBOX_UNIT_DIR="$UNITS3" TTBOX_SYSTEMD=0 bash "$ENSURE" > "$WORK/t4.log" 2>&1; rc=$?
  after="$(sha256sum "$UNITS3/ttbox-core.service" | cut -d' ' -f1)"
  [ $rc -eq 0 ] && [ "$before" = "$after" ]
  record "T4 幂等重跑：内容不变" $? "rc=$rc"

  # ---- T5 漂移自愈：改坏 dst 一个字符 → 长回且 cmp -s 源一致 ----
  printf 'X' >> "$UNITS3/ttbox-core.service"
  TTBOX_PREFIX="$PFX3" TTBOX_UNIT_DIR="$UNITS3" TTBOX_SYSTEMD=0 bash "$ENSURE" > "$WORK/t5.log" 2>&1; rc=$?
  cmp -s "$UNITS3/ttbox-core.service" "$SRC3"
  record "T5 漂移自愈：改坏后 cmp -s 源一致" $? "rc=$rc"

  # ---- T6 回归锁（T5b）：写限 / ENOSPC ⇒ 半截 unit **不得安装**（ulimit -f）----
  # 造 >1 块（4096B）的大 unit 源；`ulimit -f 1`（≤1 块）令 ensure 的候选写入被截断。
  #   修复后：cmp -s 复核失败 ⇒ 跳过、不写 ⇒ UNITS6 为空 ⇒ PASS。
  #   修复前：会把 ~1 块半截 unit install 进 UNITS6 ⇒ 与源不一致 ⇒ FAIL。
  PFX6="$WORK/pfx6"; UNITS6="$WORK/units6"; make_pfx "$PFX6"; mkdir -p "$UNITS6"; SRC6="$(SRC_UNIT "$PFX6")"
  { printf '[Unit]\nDescription=big\n\n[Service]\nExecStart=/bin/true\n'; \
    printf '%*s' 4000 '' | tr ' ' '#'; printf '\n'; } > "$SRC6"
  # 用命令替换（管道）收 stdout：避免日志本身被 RLIMIT_FSIZE 截断（管道不受 RLIMIT_FSIZE 限）
  t6_out="$( ulimit -f 1 2>/dev/null; \
             TTBOX_PREFIX="$PFX6" TTBOX_UNIT_DIR="$UNITS6" TTBOX_SYSTEMD=0 bash "$ENSURE" 2>&1 )"
  printf '%s\n' "$t6_out" > "$WORK/t6.log"
  t6_rc=0
  if [ -e "$UNITS6/ttbox-core.service" ] && ! cmp -s "$UNITS6/ttbox-core.service" "$SRC6"; then
    t6_rc=1
    printf '  [T6] 半截 unit 被写入：%s（%sB vs 源 %sB）\n' \
      "$UNITS6/ttbox-core.service" "$(wc -c <"$UNITS6/ttbox-core.service")" "$(wc -c <"$SRC6")"
  fi
  record "T6 写限下半截 unit 不被安装（T5b 回归锁）" $t6_rc

  # ---- T7 不变量锁（T5b 反面）：**既有完整 unit 不得因一次写失败而被销毁** ----
  #   锁定旧实现（install + 不一致就 `rm -f dst`）引入的**更坏失败模式**：
  #     install 失败时 dst 可能根本没被碰过 / 被 O_TRUNC 截断 ⇒ "与源不同" ⇒ 旧码把**健康旧 unit 删掉**
  #     （= 服务定义从"有"到"无"），且脚本还报成功。自愈的语义是"补"，不是"删"。
  #   步骤：① 正常装好合法 unit A（fs 有空间）② 备份 A ③ **把源前进到 B**（强制一次写尝试）
  #         ④ 让目标 fs 变满 ⑤ 跑 ensure ⑥ 断言 A **仍在且与备份逐字节一致**（没被销毁、没被截断）。
  #   ⚠ 断言对象是"既有 dst（A）"而非"当前源（B）"：本场景源已前进到 B、fs 又满，B 本就不该装进去；
  #      要保护的是**已有的健康定义 A**——正是旧实现会删/毁的那个。
  PFX7="$WORK/pfx7"; UNITS7="$WORK/units7"; make_pfx "$PFX7"; mkdir -p "$UNITS7"; SRC7="$(SRC_UNIT "$PFX7")"
  if mount -t tmpfs -o size=64k tmpfs "$UNITS7" 2>/dev/null; then
    printf '[Unit]\nDescription=A\n\n[Service]\nExecStart=/bin/true\n' > "$SRC7"     # ① A（小）
    TTBOX_PREFIX="$PFX7" TTBOX_UNIT_DIR="$UNITS7" TTBOX_SYSTEMD=0 bash "$ENSURE" > "$WORK/t7a.log" 2>&1
    okA=0; [ -f "$UNITS7/ttbox-core.service" ] && cmp -s "$UNITS7/ttbox-core.service" "$SRC7" && okA=1
    cp -f "$UNITS7/ttbox-core.service" "$WORK/t7_A.bak" 2>/dev/null || true          # ② 备份 A
    { printf '[Unit]\nDescription=B\n\n[Service]\nExecStart=/bin/true\n'; \
      printf '%*s' 20000 '' | tr ' ' '#'; printf '\n'; } > "$SRC7"                    # ③ 源→B（大）
    head -c 60000 /dev/zero > "$UNITS7/filler" 2>/dev/null || true                    # ④ 变满
    TTBOX_PREFIX="$PFX7" TTBOX_UNIT_DIR="$UNITS7" TTBOX_SYSTEMD=0 bash "$ENSURE" > "$WORK/t7b.log" 2>&1; rc=$?  # ⑤
    t7_rc=0
    [ "$okA" -eq 1 ] || { t7_rc=1; printf '  [T7] 前置失败：A 未正常装好\n'; }
    if [ ! -e "$UNITS7/ttbox-core.service" ]; then
      t7_rc=1; printf '  [T7] 既有健康 unit 被**销毁**（不存在）—— 正是旧 `rm -f dst` 的失败模式\n'
    elif ! cmp -s "$WORK/t7_A.bak" "$UNITS7/ttbox-core.service"; then
      t7_rc=1; printf '  [T7] 既有健康 unit 被**破坏/截断**：%sB（应 %sB）\n' \
        "$(wc -c <"$UNITS7/ttbox-core.service")" "$(wc -c <"$WORK/t7_A.bak")"
    fi
    record "T7 既有完整 unit 不因写失败被销毁（不变量锁）" $t7_rc "rc=$rc"
    umount "$UNITS7" 2>/dev/null || true
  else
    skip "T7：本环境不支持 mount tmpfs（需 root/特权）"
  fi
else
  skip "T2..T7：非 root，跳过真写盘用例"
fi

echo "------------------------------------------------------------"
printf 'RESULT: %d/%d PASS\n' "$PASS" "$((PASS+FAIL))"
printf 'SKIP: %d（条件/权限不足未执行——静默跳过=病，故显式计数）\n' "$SKIPPED"
# ★ 非 root 时 T2..T7 全 SKIP：此时 `1/1 PASS` 极易被误读成"ensure 全过"。显式声明不构成契约绿。
if [ "$(id -u)" -ne 0 ]; then
    printf '\n*** 警告：本环境非 root，写盘用例（T2..T7）全部未执行。\n'
    printf '*** 该结果【不能】作为 T1.03 通过证据 —— 请以 root（WSL: wsl -u root）重跑。***\n'
fi
if [ ${#FAILED[@]} -gt 0 ]; then printf 'FAILED:'; for f in "${FAILED[@]}"; do printf ' [%s]' "$f"; done; printf '\n'; fi
echo "artifacts in: $WORK"
[ "$FAIL" -eq 0 ]
