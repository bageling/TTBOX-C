#!/usr/bin/env bash
# 宿主侧（WSL）· 成品镜像一致性校验 / 修复
#
# 用法：
#   bash 99_fsck.sh            # 只读检查（不改盘）
#   bash 99_fsck.sh --repair   # 修复（只动 /root/ttbox-image/work.img 副本，绝不动原图）
#
# ★ 坑（本脚本自己踩过并修掉）：`e2fsck ... | tail` 拿到的退出码是 **tail 的**，恒为 0，
#   会把真错误全吃掉（与 04 里 netplan 那个 `| sed` 是同一个族的错误）。
#   正确姿势：先重定向到临时文件拿 rc，再把文件落屏。
set -u
WORK="${WORK:-/root/ttbox-image/work.img}"
MODE="ro"; [ "${1:-}" = "--repair" ] && MODE="rw"
OUT="$(mktemp)"
LOOP=""

cleanup() { [ -n "$LOOP" ] && { losetup -d "$LOOP" 2>/dev/null || true; }; rm -f "$OUT"; }
trap cleanup EXIT

echo "== 挂 loop（带分区扫描） =="
LOOP="$(losetup --find --show --partscan "$WORK")"
echo "  loop=$LOOP"
sleep 1
ls -l "${LOOP}"* 2>/dev/null | awk '{print "  "$0}'

echo
if [ "$MODE" = "rw" ]; then
    echo "== e2fsck -f -y（修复模式：只动 work.img 副本）=="
    e2fsck -f -y "${LOOP}p2" >"$OUT" 2>&1
else
    echo "== e2fsck -f -n（只读，不改盘）=="
    e2fsck -f -n "${LOOP}p2" >"$OUT" 2>&1
fi
FSCK_RC=$?
sed 's/^/  /' "$OUT"

echo
echo "  e2fsck **真实**退出码 = $FSCK_RC"
case "$FSCK_RC" in
    0) echo "  语义：文件系统干净" ;;
    1) echo "  语义：发现错误并已修正" ;;
    2) echo "  语义：发现错误并已修正（建议重启，镜像场景不适用）" ;;
    4) echo "  语义：[!] 发现错误但**未修正**（只读模式）—— 需要跑 --repair" ;;
    8) echo "  语义：[!] 操作错误" ;;
   16) echo "  语义：[!] 用法/语法错误" ;;
   32) echo "  语义：[!] 被用户中断" ;;
  128) echo "  语义：[!] 共享库错误" ;;
    *) echo "  语义：[!] 组合错误码" ;;
esac

echo
echo "== 释放 loop =="
losetup -d "$LOOP" 2>/dev/null && echo "  已释放 $LOOP" || echo "  [i] $LOOP 仍附着（WSL 常见；未挂载即无害）"
LOOP=""

case "$FSCK_RC" in
    0|1) exit 0 ;;
    *)   exit "$FSCK_RC" ;;
esac
