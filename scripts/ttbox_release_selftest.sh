#!/usr/bin/env bash
# ttbox_release_selftest.sh — T1.01 发布布局自测（27 项断言；含 1 项条件断言 A27/E3）
#
# 覆盖：正常浇筑/切换/校验 + 反证（篡改 sha256 / RUNPATH≠$ORIGIN 逐段 / 中途 kill 不留半截版本
#       / bin/ 非闭集成员被拒[E2]）+ --selftest-stall 双条件锁 + TRANSITIONAL_LINKS 双份一致
#       + 回滚 + 幂等重发布 + 对**真实交叉产物**跑 verify RUNPATH[A27/E3，缺产物则 SKIP 并计数]。
#
# ─────────────────────────────────────────────────────────────────────────────
# ★ 命名空间声明（2026-09-17 主理人裁决：零改名，按"限定词"收口）
#   本脚本断言编号属【本地自测域 `selftest A##`】，与交付文档仓库
#   `ttbox-vs-yu-program/m1-acceptance-checklist.md` 的【契约域 `A#`（无编号上界，条数以 §9 为准）】**同名不同域**。
#   **引用本脚本编号时必须写 `selftest` 前缀。**
#   已知同名对不同义（务必区分）：
#     • selftest A20 = "RUNPATH≠$ORIGIN 被 verify 判 FAIL"（本脚本负例）
#       vs 契约 A20 = "启动非致命化：EDID 改错不阻断 core"（T1.04）
#     • selftest A26 = "bin/ 非闭集成员被 verify 判 FAIL"（E2 负例）
#       vs 契约 A26 = "T1.14 引擎接线层 3 用例"（板端专属）
#     • selftest A27 = "对真实交叉产物跑 verify RUNPATH"（E3）
#       vs 契约 A27a/A27b = "T1.14 陷阱二回归锁 / 板端真链路"（板端专属）
#   脚本结尾另打印 `NAMESPACE:` 行，便于任何日志摘录自带归属。
# ─────────────────────────────────────────────────────────────────────────────
#
# 运行环境：**Linux / WSL**（依赖 aarch64-linux-gnu-gcc 造 aarch64 ELF 夹具 + python3 造 manifest）。
#   wsl -d Ubuntu-22.04 -- bash /mnt/c/.../TTBOX-Module-Edition-main/scripts/ttbox_release_selftest.sh
#
# 安全：所有操作都在 mktemp 出来的临时前缀里（TTBOX_PREFIX / TTBOX_ETC / TTBOX_RUN），
#       **绝不触碰真实 /opt/ttbox**。
#
# 回归锚点（T1.16）：A20 断言依赖 ttbox_release_verify.sh 的 RUNPATH 判据文案
#       —— 已由「整串前缀匹配」改为「按 : 逐段校验」，故断言匹配 'RUNPATH 段'。
#       若再次改 verify 文案，必须同步更新 A20，否则本条会假 FAIL。
# 回归锚点（E2）：A26 断言依赖 verify 的 '非闭集成员' 文案；夹具 bin/ 闭集 = { ttbox_core_main }
#       （Web 迁 C++ 后闭集扩为 { ttbox_core_main, ttbox_web }；夹具不造 ttbox_web 不影响
#       A26 负例 —— 闭集判据只查"出现非成员"、不查"成员齐全"）。
#
# 退出码：全部通过=0；有任一 FAIL=1（SKIP 不计 FAIL，但显式打印并计入 SKIP 计数）。
set -u

# ---- 仓库根定位：优先 TTBOX_REPO，否则由脚本自身位置推导 ----
if [ -n "${TTBOX_REPO:-}" ]; then
  REPO="$TTBOX_REPO"
else
  REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
fi

# ---- 预检：缺工具则明确报错，避免"静默造不出夹具导致假 FAIL" ----
missing=""
for t in aarch64-linux-gnu-gcc python3 mktemp awk grep diff sed readlink; do
  command -v "$t" >/dev/null 2>&1 || missing="$missing $t"
done
if [ -n "$missing" ]; then
  echo "缺少必需工具:$missing" >&2
  echo "请在 Linux/WSL 下运行（需要 aarch64-linux-gnu-gcc 与 python3）。" >&2
  exit 2
fi
if [ ! -x "$REPO/scripts/ttbox_release_install.sh" ] && [ ! -f "$REPO/scripts/ttbox_release_install.sh" ]; then
  echo "找不到 $REPO/scripts/ttbox_release_install.sh（TTBOX_REPO 指向对吗？）" >&2
  exit 2
fi

WORK=$(mktemp -d /tmp/ttbox-selftest-XXXXXX)
PASS=0; FAIL=0; SKIPPED=0; declare -a FAILED=()

record() { # $1=label $2=rc [$3=extra]
  if [ "$2" -eq 0 ]; then PASS=$((PASS+1)); printf '[ OK ] %s\n' "$1";
  else FAIL=$((FAIL+1)); FAILED+=("$1"); printf '[FAIL] %s  %s\n' "$1" "${3:-}"; fi
}
mkprefix() { mktemp -d /tmp/ttbox-pfx-XXXXXX; }

# --- CR 归一化拷贝真实脚本（Windows 工作区可能带 CR） ---
sed 's/\r$//' "$REPO/scripts/ttbox_release_install.sh" > "$WORK/install.sh"
sed 's/\r$//' "$REPO/scripts/ttbox_release_verify.sh"  > "$WORK/verify.sh"
chmod +x "$WORK/install.sh" "$WORK/verify.sh"

GOODRP='-Wl,-rpath,$ORIGIN/../lib'
BADRP='-Wl,-rpath,/opt/ttbox/lib'

# 生成 RELEASE_MANIFEST.json（覆盖目录内全部文件）；由 make_payload / A26 复用，避免重复。
gen_manifest() { # $1=root $2=ver
  python3 - "$1" "$2" <<'PY'
import hashlib, json, os, sys
root, ver = sys.argv[1], sys.argv[2]
files = {}
for dp, dn, fn in os.walk(root):
    for f in fn:
        full = os.path.join(dp, f)
        rel = os.path.relpath(full, root)
        if rel == "RELEASE_MANIFEST.json":
            continue
        files[rel] = hashlib.sha256(open(full, "rb").read()).hexdigest()
json.dump({"version": ver, "built_at": "2026-09-16T00:00:00Z",
           "git_sha": "0"*40, "files_sha256": files},
          open(os.path.join(root, "RELEASE_MANIFEST.json"), "w"),
          indent=2, sort_keys=True)
PY
}

make_payload() { # $1=dir $2=rpath [$3=real_core_main]
  local p="$1" rp="$2" real="${3:-}"
  rm -rf "$p"; mkdir -p "$p/bin" "$p/plugins/web/bin" "$p/plugins/preview/bin" \
                        "$p/scripts/edid" "$p/deploy/systemd" "$p/lib"
  printf 'int main(void){return 0;}\n' > "$p/_t.c"
  aarch64-linux-gnu-gcc -o "$p/bin/ttbox_core_main" "$p/_t.c" "$rp" 2>/dev/null
  rm -f "$p/_t.c"
  chmod +x "$p/bin/ttbox_core_main"
  # ★ E2：bin/ 为【白名单闭集】（verify 会点名校验；此前夹具额外造 ipc_ping，与 E2 判据
  #   自相矛盾——已删除）。Web 迁 C++ 后真发货闭集 = { ttbox_core_main, ttbox_web }；
  #   夹具只造 ttbox_core_main（E2 只查"出现非成员"，不查"成员齐全"，故不影响 A26）。
  # ★ E3：可选以**真实交叉产物**替换合成 core_main（对真货跑 verify，而非合成夹具）。
  if [ -n "$real" ]; then cp -f "$real" "$p/bin/ttbox_core_main"; chmod +x "$p/bin/ttbox_core_main"; fi
  printf '#!/bin/sh\nexit 0\n' > "$p/plugins/web/bin/ttbox-web";           chmod +x "$p/plugins/web/bin/ttbox-web"
  printf '#!/bin/sh\nexit 0\n' > "$p/scripts/edid/edid_apply.sh";          chmod +x "$p/scripts/edid/edid_apply.sh"
  local u
  for u in ttbox-core ttbox-web; do
    sed 's/\r$//' "$REPO/deploy/systemd/$u.service" > "$p/deploy/systemd/$u.service"
  done
  gen_manifest "$p" "$(basename "$p")"
}

do_install() { # $1=prefix $2=ver $3=payload  extra args...
  local pfx="$1" ver="$2" pay="$3"; shift 3
  TTBOX_PREFIX="$pfx" TTBOX_ETC="$WORK/etc/ttbox" TTBOX_RUN="$WORK/run/ttbox" TTBOX_SYSTEMD=0 \
    bash "$WORK/install.sh" "$ver" "$pay" "$@"
}
do_verify() { # $1=prefix $2=ver
  TTBOX_PREFIX="$1" bash "$WORK/verify.sh" "$2"
}

# ===========================================================================
echo "== REPO=$REPO =="
echo "== WORK=$WORK =="
PFX_A=$(mkprefix); PAY_A="$WORK/payload-a"; make_payload "$PAY_A" "$GOODRP"

# ---- A01 install publishes release ----
do_install "$PFX_A" 1.0.0 "$PAY_A" --activate > "$WORK/a01.log" 2>&1; rc=$?
[ $rc -eq 0 ] && [ -d "$PFX_A/releases/1.0.0" ]; record "A01 install --activate 发布 releases/1.0.0" $? "rc=$rc"

# ---- A02 no .staging residue ----
! compgen -G "$PFX_A/releases/*.staging" >/dev/null; record "A02 成功后无 *.staging 残留" $?

# ---- A03 current symlink -> releases/1.0.0 ----
[ "$(basename "$(readlink -f "$PFX_A/current" 2>/dev/null)")" = "1.0.0" ]; record "A03 current 指向 releases/1.0.0" $?

# ---- A04 bin/ttbox_core_main executable in release ----
[ -x "$PFX_A/releases/1.0.0/bin/ttbox_core_main" ]; record "A04 release 内 bin/ttbox_core_main 可执行" $?

# ---- A05 TRANSITIONAL_LINKS 双份一致 ----
get_tl() { awk '/^TRANSITIONAL_LINKS=\(/{f=1;next} f&&/^\)/{f=0} f{print}' "$1" | grep -o '"[^"]*"' || true; }
get_tl "$WORK/install.sh" > "$WORK/tl_install.txt"; get_tl "$WORK/verify.sh" > "$WORK/tl_verify.txt"
# ★ 非空下限：`|| true` 使抽取失败也返回空串；若两脚本都抽空，"空 == 空" 会**假 PASS**。
#   故先要求**两侧均非空**（空 ⇒ 抽取已失效，直接 FAIL），再比一致性——否则断言没有牙齿。
if [ -s "$WORK/tl_install.txt" ] && [ -s "$WORK/tl_verify.txt" ] \
   && diff -q "$WORK/tl_install.txt" "$WORK/tl_verify.txt" >/dev/null; then
  a05_rc=0
else
  a05_rc=1
fi
record "A05 TRANSITIONAL_LINKS install/verify 双份一致（且均非空）" $a05_rc

# ---- A06/A07 transitional links ----
[ "$(readlink -f "$PFX_A/plugins")" = "$(readlink -f "$PFX_A/current")/plugins" ]; record "A06 过渡软链 plugins -> current/plugins" $?
[ "$(readlink -f "$PFX_A/scripts")" = "$(readlink -f "$PFX_A/current")/scripts" ]; record "A07 过渡软链 scripts -> current/scripts" $?

# ---- A08 verify PASS ----
do_verify "$PFX_A" 1.0.0 > "$WORK/a08.log" 2>&1; rc=$?
grep -q 'RESULT: PASS' "$WORK/a08.log" && [ $rc -eq 0 ]; record "A08 verify.sh 退出 0 且 RESULT: PASS" $? "rc=$rc"

# ---- A09..A17 verify 明细断言（grep 摘要） ----
V="$WORK/a08.log"
grep -q 'manifest 全量 sha256 通过' "$V";                       record "A09 verify: manifest 全量 sha256 通过" $?
grep -q 'ttbox-core.service: ExecStart 可执行' "$V";           record "A10 verify: core ExecStart 可执行" $?
grep -q 'ttbox-web.service: ExecStart 可执行' "$V";            record "A11 verify: web ExecStart 可执行" $?
grep -q 'RUNPATH 自包含' "$V";                                 record "A13 verify: RUNPATH 自包含 OK" $?
grep -q 'ttbox-core.service: StartLimitBurst=5 且位于 \[Unit\]' "$V";        record "A14 verify: core StartLimitBurst=5 [Unit]" $?
grep -q 'ttbox-core.service: StartLimitIntervalSec=300 且位于 \[Unit\]' "$V"; record "A15 verify: core StartLimitIntervalSec=300 [Unit]" $?
grep -q '过渡软链: .*plugins' "$V";                            record "A16 verify: 过渡软链 plugins 断言 OK" $?
grep -q '过渡软链: .*scripts' "$V";                            record "A17 verify: 过渡软链 scripts 断言 OK" $?

# ---- A18 幂等重发布同版本 ----
do_install "$PFX_A" 1.0.0 "$PAY_A" --activate > "$WORK/a18.log" 2>&1; rc=$?
! compgen -G "$PFX_A/releases/*.old.*" >/dev/null && [ $rc -eq 0 ]; record "A18 幂等重发布同版本且无 *.old.* 残留" $? "rc=$rc"

# ---- A19 反证：篡改 payload bin -> install step1 拒绝 ----
# 篡改**闭集成员** bin/ttbox_core_main（E2 后 bin/ 闭集只有它，不再有 ipc_ping 可篡改）。
PFX_B=$(mkprefix); PAY_B="$WORK/payload-b"; make_payload "$PAY_B" "$GOODRP"
printf 'X' >> "$PAY_B/bin/ttbox_core_main"
do_install "$PFX_B" 1.0.0 "$PAY_B" --activate > "$WORK/a19.log" 2>&1; rc=$?
[ $rc -ne 0 ] && grep -q '校验失败' "$WORK/a19.log"; record "A19 篡改 payload bin/ 被 install 拒绝" $? "rc=$rc"

# ---- A20 反证：RUNPATH=/opt/ttbox/lib -> verify FAIL ----
# verify 判据已改「按 : 逐段校验」（T1.16），失败文案含 'RUNPATH 段'。
PFX_C=$(mkprefix); PAY_C="$WORK/payload-c"; make_payload "$PAY_C" "$BADRP"
do_install "$PFX_C" 1.0.0 "$PAY_C" --activate > "$WORK/a20i.log" 2>&1
do_verify "$PFX_C" 1.0.0 > "$WORK/a20.log" 2>&1; rc=$?
[ $rc -ne 0 ] && grep -q 'RUNPATH 段' "$WORK/a20.log"; record "A20 RUNPATH≠\$ORIGIN 被 verify 判 FAIL" $? "rc=$rc"

# ---- A21 反证：篡改 release 内文件 -> verify FAIL(sha) ----
printf 'X' >> "$PFX_A/releases/1.0.0/bin/ttbox_core_main"
do_verify "$PFX_A" 1.0.0 > "$WORK/a21.log" 2>&1; rc=$?
[ $rc -ne 0 ] && grep -q 'sha256 不符' "$WORK/a21.log"; record "A21 release 文件被篡改 -> verify FAIL(sha256)" $? "rc=$rc"

# ---- A22 --selftest-stall 单条件（无环境变量）被拒 ----
PFX_D=$(mkprefix); PAY_D="$WORK/payload-d"; make_payload "$PAY_D" "$GOODRP"
env -u TTBOX_RELEASE_SELFTEST TTBOX_PREFIX="$PFX_D" TTBOX_SYSTEMD=0 \
  bash "$WORK/install.sh" 2.0.0 "$PAY_D" --selftest-stall 3 > "$WORK/a22.log" 2>&1; rc=$?
[ $rc -ne 0 ] && grep -q '仅限自测' "$WORK/a22.log"; record "A22 --selftest-stall 缺环境变量被拒(生产不可达)" $? "rc=$rc"

# ---- A23 中途 kill 不留半截版本 ----
TTBOX_PREFIX="$PFX_D" TTBOX_SYSTEMD=0 TTBOX_RELEASE_SELFTEST=1 \
  bash "$WORK/install.sh" 2.0.0 "$PAY_D" --selftest-stall 6 > "$WORK/a23.log" 2>&1 &
pid=$!
for i in $(seq 1 60); do [ -d "$PFX_D/releases/2.0.0.staging" ] && break; sleep 0.2; done
sleep 0.3; kill -9 "$pid" 2>/dev/null; wait "$pid" 2>/dev/null
[ ! -d "$PFX_D/releases/2.0.0" ] && [ -d "$PFX_D/releases/2.0.0.staging" ]; record "A23 中途 kill：无 releases/2.0.0，仅 .staging 残留" $?

# ---- A24 kill 后重跑可恢复 ----
do_install "$PFX_D" 2.0.0 "$PAY_D" --activate > "$WORK/a24.log" 2>&1; rc=$?
do_verify "$PFX_D" 2.0.0 > "$WORK/a24v.log" 2>&1
[ $rc -eq 0 ] && [ -d "$PFX_D/releases/2.0.0" ] && grep -q 'RESULT: PASS' "$WORK/a24v.log"; record "A24 kill 后重跑发布并 verify PASS" $? "rc=$rc"

# ---- A25 回滚 ----
PFX_E=$(mkprefix); PAY_E="$WORK/payload-e"; make_payload "$PAY_E" "$GOODRP"
do_install "$PFX_E" 1.0.0 "$PAY_E" --activate >/dev/null 2>&1
do_install "$PFX_E" 1.1.0 "$PAY_E" --activate >/dev/null 2>&1
cur_before="$(basename "$(readlink -f "$PFX_E/current")")"
TTBOX_PREFIX="$PFX_E" TTBOX_SYSTEMD=0 bash "$WORK/install.sh" --rollback > "$WORK/a25.log" 2>&1; rc=$?
cur_after="$(basename "$(readlink -f "$PFX_E/current")")"
[ "$cur_before" = "1.1.0" ] && [ "$cur_after" = "1.0.0" ] && [ $rc -eq 0 ]; record "A25 --rollback 将 current 切回上一版本(1.1.0->1.0.0)" $? "before=$cur_before after=$cur_after rc=$rc"

# ---- A26 反证：bin/ 出现非闭集成员 -> verify FAIL（E2 执行点判据）----
# manifest 重新生成（含新成员，sha 自洽）⇒ 失败**只能**归因 E2 闭集，而非 sha，证明判据有牙齿。
PFX_F=$(mkprefix); PAY_F="$WORK/payload-f"; make_payload "$PAY_F" "$GOODRP"
printf 'int main(void){return 0;}\n' > "$WORK/_extra.c"
aarch64-linux-gnu-gcc -o "$PAY_F/bin/ipc_ping" "$WORK/_extra.c" "$GOODRP" 2>/dev/null; rm -f "$WORK/_extra.c"
chmod +x "$PAY_F/bin/ipc_ping"
gen_manifest "$PAY_F" "$(basename "$PAY_F")"
do_install "$PFX_F" 1.0.0 "$PAY_F" --activate > "$WORK/a26i.log" 2>&1
do_verify "$PFX_F" 1.0.0 > "$WORK/a26.log" 2>&1; rc=$?
[ $rc -ne 0 ] && grep -q '非闭集成员' "$WORK/a26.log"; record "A26 bin/ 非闭集成员被 verify 判 FAIL(E2)" $? "rc=$rc"

# ---- A27（E3）对**真实交叉产物**跑 verify 的 RUNPATH 逐段检查 ----
# 设计约束：产物不存在时**不得静默跳过**——打印醒目 SKIP 行并把跳过计入总结（静默跳过=病）。
# 默认取"最新的 build-aarch64* 产物"（不再硬编码某个轮次编号的构建目录：那类目录每轮出货
# 都会被 rm -rf 重建、事后又被清理 ⇒ 写死会直接退化成 SKIP，而 SKIP 是"没验证"不是"通过"）
_real_auto="$(ls -1dt "$REPO"/build-aarch64*/ttbox_core_main 2>/dev/null | head -1 || true)"
REAL_CORE_MAIN="${TTBOX_REAL_CORE_MAIN:-${_real_auto:-$REPO/build-aarch64/ttbox_core_main}}"
if [ -x "$REAL_CORE_MAIN" ]; then
  PFX_R=$(mkprefix); PAY_R="$WORK/payload-real"; make_payload "$PAY_R" "$GOODRP" "$REAL_CORE_MAIN"
  echo "---- E3 真产物 readelf -d 原始输出（$REAL_CORE_MAIN）----"
  readelf -d "$REAL_CORE_MAIN" 2>/dev/null | grep -E '\((RUNPATH|RPATH)\)' || echo '  (无 RUNPATH 段)'
  do_install "$PFX_R" 1.0.0 "$PAY_R" --activate > "$WORK/a27i.log" 2>&1
  do_verify "$PFX_R" 1.0.0 > "$WORK/a27.log" 2>&1; rc=$?
  grep -q 'RUNPATH 自包含（逐段）: bin/ttbox_core_main' "$WORK/a27.log" && [ $rc -eq 0 ]
  record "A27 真产物 verify RUNPATH 逐段 OK 且整体 PASS(E3)" $? "rc=$rc"
else
  printf '===== [SKIP] A27(E3)：真实交叉产物不存在，未对真货验证 RUNPATH =====\n'
  printf '===== [SKIP]   期望路径: %s（先交叉构建，或用 TTBOX_REAL_CORE_MAIN 覆盖）=====\n' "$REAL_CORE_MAIN"
  SKIPPED=$((SKIPPED+1))
fi

# ===========================================================================
echo "------------------------------------------------------------"
printf 'RESULT: %d/%d PASS\n' "$PASS" "$((PASS+FAIL))"
printf 'SKIP: %d（条件断言未执行——静默跳过=病，故显式计数）\n' "$SKIPPED"
printf 'NAMESPACE: selftest A##（本脚本本地自测域；契约域见交付文档仓库 ttbox-vs-yu-program/m1-acceptance-checklist.md，条数以该清单为准）\n'
if [ ${#FAILED[@]} -gt 0 ]; then
  printf 'FAILED:'; for f in "${FAILED[@]}"; do printf ' [%s]' "$f"; done; printf '\n'
fi
echo "artifacts in: $WORK"
[ "$FAIL" -eq 0 ]
