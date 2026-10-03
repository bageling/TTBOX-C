#!/usr/bin/env bash
# 宿主侧（WSL）· 成品镜像回写
#
# 为什么先写 .new 再改名的：回写 5GB 是在 drvfs(9p) 上跑，中断风险真实存在。
#   直接覆盖原图 ⇒ 中断即"原图没了、成品也没了"。写临时文件 + 双向 sha256 校验通过后
#   再原子改名，才是"就地改原图"的正确姿势：要么成品就位，要么原图原封不动。
set -euo pipefail
WORK="${WORK:-/root/ttbox-image/work.img}"
# ★ 回写目标是本机私有路径，刻意不给默认值（用 DST=<xxx.img> 传）。
DST="${DST:-}"
[ -n "$DST" ] || { echo "需指定回写目标：DST=<xxx.img>" >&2; exit 1; }
TMP="${DST}.new"

echo "== 0. 前置检查 =="
[ -f "$WORK" ] || { echo "找不到 $WORK"; exit 1; }
[ -f "$DST" ]  || { echo "找不到原图 $DST"; exit 1; }
echo "  源  : $WORK  ($(stat -c %s "$WORK") bytes)"
echo "  目标: $DST  ($(stat -c %s "$DST") bytes)"
echo "  可用空间: $(df -h "$(dirname "$DST")" | tail -1 | awk '{print $4}')"

echo
echo "== 1. 记录原始 sha256（回滚凭据） =="
ORIG_SHA="$(sha256sum "$DST" | cut -d' ' -f1)"
echo "  原图 sha256 = $ORIG_SHA"

echo
echo "== 2. 写入临时文件（bs=8M，边写边校验字节数） =="
rm -f "$TMP"
dd if="$WORK" of="$TMP" bs=8M conv=fsync status=progress
sync
WANT="$(stat -c %s "$WORK")"
GOT="$(stat -c %s "$TMP")"
echo "  期望字节=$WANT  实得字节=$GOT"
[ "$WANT" = "$GOT" ] || { echo "[✗] 字节数不符，保留原图不动"; exit 1; }

echo
echo "== 3. sha256 双向校验 =="
S_WORK="$(sha256sum "$WORK" | cut -d' ' -f1)"
S_TMP="$(sha256sum "$TMP"  | cut -d' ' -f1)"
echo "  work.img   = $S_WORK"
echo "  临时成品   = $S_TMP"
[ "$S_WORK" = "$S_TMP" ] || { echo "[✗] sha256 不符，保留原图不动"; exit 1; }
echo "  [✓] 逐字节一致"

echo
echo "== 4. 原子改名（覆盖原图） =="
mv -f "$TMP" "$DST"
echo "  已就位: $DST"

echo
echo "== 5. 落盘后复验 =="
S_FINAL="$(sha256sum "$DST" | cut -d' ' -f1)"
echo "  最终 sha256 = $S_FINAL"
[ "$S_FINAL" = "$S_WORK" ] || { echo "[✗] 覆盖后 sha256 变了！"; exit 1; }
echo "  [✓] 覆盖后与成品一致"
echo
echo "  原图 sha256（已被替换，留档）: $ORIG_SHA"
echo "  成品 sha256                  : $S_FINAL"
printf '%s\n' "原图(替换前) $ORIG_SHA" "成品         $S_FINAL" > /root/ttbox-image/writeback.sha256
echo "  已存 /root/ttbox-image/writeback.sha256"
