#!/usr/bin/env bash
# 阶段一 · 把镜像落到 WSL 本地 ext4 工作区，并记录原始指纹
# 说明：不在 /mnt/c(drvfs) 上直接 loop 写盘 —— 那里跑 apt/写文件系统极易出脏页与损坏；
#       改为本地工作副本，全部改完后原路写回同一路径（对外仍是"原地改"）。
set -eu

# ★ 源镜像路径是本机私有（下载目录），刻意不给默认值：给一个"看起来像"的只会让人
#   以为能直接跑。用 SRC=<xxx.img> 传。
SRC="${SRC:-}"
[ -n "$SRC" ] || { echo "需指定源镜像：SRC=<xxx.img>" >&2; exit 1; }
[ -f "$SRC" ] || { echo "源镜像不存在: $SRC" >&2; exit 1; }
WORKDIR="/root/ttbox-image"
WORK="${WORKDIR}/work.img"
SHAFILE="${WORKDIR}/original.sha256"

mkdir -p "$WORKDIR"

echo "== 0. 源镜像信息 =="
ls -l "$SRC"
if [ ! -f "$SHAFILE" ]; then
    echo "计算原始 sha256（首次，约 1 分钟）…"
    sha256sum "$SRC" | tee "$SHAFILE"
else
    echo "已有原始指纹:"; cat "$SHAFILE"
fi

echo
echo "== 1. 复制到本地工作区 =="
if [ -f "$WORK" ]; then
    echo "$WORK 已存在，跳过复制（如需重来请手动删除）"
else
    cp --sparse=always "$SRC" "$WORK"
fi
ls -lh "$WORK"
sha256sum "$WORK" | awk '{print "work.img sha256 = "$1}'

echo
echo "== 2. 本地磁盘余量 =="
df -h "$WORKDIR"
