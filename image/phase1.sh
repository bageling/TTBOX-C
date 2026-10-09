#!/usr/bin/env bash
# 阶段一驱动：挂载镜像 + chroot 自检
set -eu
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
bash "$HERE/20_mount.sh"
echo
bash "$HERE/run_in_img.sh" 00_verify.sh
