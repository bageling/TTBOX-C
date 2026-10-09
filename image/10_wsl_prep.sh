#!/usr/bin/env bash
# 阶段一 · WSL 侧准备：qemu-user-static + binfmt，使 x86_64 能直接执行 aarch64 用户态
set -e
echo "== 1. 网络可达性 =="
for h in ports.ubuntu.com ppa.launchpadcontent.net archive.ubuntu.com; do
  printf '%-34s ' "$h"
  if getent hosts "$h" >/dev/null 2>&1; then echo "DNS OK"; else echo "DNS FAIL"; fi
done

echo
echo "== 2. 安装 qemu-user-static + binfmt-support =="
export DEBIAN_FRONTEND=noninteractive
apt-get update -qq 2>&1 | tail -3
apt-get install -y -qq qemu-user-static binfmt-support 2>&1 | tail -10

echo
echo "== 3. aarch64 binfmt 注册状态 =="
if ls /proc/sys/fs/binfmt_misc/ 2>/dev/null | grep -qi aarch64; then
  echo "已注册:"
  sed -n '1,6p' /proc/sys/fs/binfmt_misc/qemu-aarch64
else
  echo "(未注册，尝试手动注册)"
  if [ -x /usr/bin/qemu-aarch64-static ]; then
    echo ':qemu-aarch64:M::\x7fELF\x02\x01\x01\x00\x00\x00\x00\x00\x00\x00\x00\x00\x02\x00\xb7\x00:\xff\xff\xff\xff\xff\xff\xff\x00\xff\xff\xff\xff\xff\xff\xff\xff\xfe\xff\xff\xff:/usr/bin/qemu-aarch64-static:F' \
      > /proc/sys/fs/binfmt_misc/register 2>/dev/null && echo "手动注册成功" || echo "手动注册失败"
    ls /proc/sys/fs/binfmt_misc/ | grep -i aarch64 || echo "仍未见 aarch64"
  fi
fi

echo
echo "== 4. 版本 =="
/usr/bin/qemu-aarch64-static --version 2>&1 | head -1
echo "内核已注册的 binfmt（截取）:"
ls /proc/sys/fs/binfmt_misc/ 2>/dev/null | tr '\n' ' '
