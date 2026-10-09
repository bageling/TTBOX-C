#!/usr/bin/env bash
# 在镜像 chroot 内执行 image/steps/<脚本>
# 用法: image/run_in_img.sh <steps 下的脚本名> [参数...]
#
# 自愈：WSL2 默认 60s 空闲即关 VM，挂载与 loop 随之消失 ⇒ 每次先确保挂载。
# 归档：staging 组装在 WSL 本地 ext4（见 prepare_stage.sh 头注释），再同步到镜像
#       /root/_bake/ —— 避免 drvfs 把权限带成 777。
set -eu
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
IMG_ROOT="${IMG_ROOT:-/mnt/img}"
STAGE="${TTBOX_STAGE_DIR:-/root/ttbox-image/_stage}"
STEP="${1:-}"; shift || true

[ -n "$STEP" ] || { echo "用法: run_in_img.sh <steps 下的脚本名> [参数...]" >&2; exit 2; }
[ -f "$HERE/steps/$STEP" ] || { echo "ERROR: 找不到 image/steps/$STEP" >&2; exit 1; }

if ! mountpoint -q "$IMG_ROOT"; then
    echo "[mount] $IMG_ROOT 未挂载，自动挂载…"
    bash "$HERE/20_mount.sh" >/dev/null
fi

bash "$HERE/prepare_stage.sh" >/dev/null

echo "[stage] 同步 $STAGE -> 镜像 ${IMG_ROOT}/root/_bake"
rm -rf "$IMG_ROOT/root/_bake"
mkdir -p "$IMG_ROOT/root/_bake"
cp -a "$STAGE/." "$IMG_ROOT/root/_bake/"
# 兜底设权：cp 跨文件系统的 mode 语义不完全可靠，显式再设一遍
find "$IMG_ROOT/root/_bake/steps" -type f -name '*.sh' -exec chmod 0755 {} + 2>/dev/null || true
find "$IMG_ROOT/root/_bake/scripts" -type f -name '*.sh' -exec chmod 0755 {} + 2>/dev/null || true

echo "===== [chroot] 执行 $STEP ====="
set +e
chroot "$IMG_ROOT" /bin/bash "/root/_bake/steps/$STEP" "$@"
rc=$?
set -e
echo "===== [chroot] $STEP 退出码=$rc ====="
exit $rc
