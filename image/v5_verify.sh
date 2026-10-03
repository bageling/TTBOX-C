#!/usr/bin/env bash
# V5 成品独立复验（宿主侧 · WSL）
#
# 目的：证明「交付物本身」正确，而不是「构建过程报告说正确」。
#   做法：只读挂载成品 -V5.img，独立取一切判据，不复用构建期任何变量（除手头口令）。
#
# ★ 写法约束（踩过坑）：判据一律先算进变量，再拿变量做比较。
#   不要把 `2>/dev/null` 塞进被引号包住的路径里（`"$M/x 2>/dev/null)"`）——
#   那样既改变语义（readlink 拿到带重定向字样的文件名 → 恒空 → 恒假绿），
#   又会让 bash 的 $( ) 配对解析出错（报 unexpected EOF looking for matching `)`）。
#
# 用法：
#   wsl.exe -d Ubuntu-22.04 -- bash "/mnt/g/.../image/v5_verify.sh" 2>&1 | tee /root/ttbox-image/v5-verify.log
set -uo pipefail

# ★ 两个都是本机私有路径，刻意不给默认值（用 DST= / KEYS= 传），
#   免得下一个人看到一个"看起来像"的路径就以为能直接跑。
DST="${DST:-}"
KEYS="${KEYS:-}"
[ -n "$DST" ]  || { echo "需指定待验镜像：DST=<xxx.img>" >&2; exit 1; }
[ -n "$KEYS" ] || { echo "需指定密钥目录：KEYS=<dir>（含 root-password.txt）" >&2; exit 1; }
M="/mnt/v5check"
PASS="$(cat "$KEYS/root-password.txt")"
EXP_HK_ED="9ddda0aa622722ce189b6f88dd85c05ab29df4c7313871cc26636ced5854818b"
EXP_EXT="3fab043163523909f5093ab505a1e4ef0106a0e9f6647aac6d9759417d58defc"
EXP_DTB="277d9de87876a4e6160ae7ac048d4adadec73bbaa7706f39e2b5a5fe42379980"
FAIL=0

ck() { local d="$1"; shift; if "$@" >/dev/null 2>&1; then printf '  [✓] %s\n' "$d"; else printf '  [✗] %s\n' "$d"; FAIL=1; fi; }

[ -f "$DST" ] || { echo "找不到成品 $DST"; exit 1; }

echo "################ V5 成品独立复验 @ $(date -Is) ################"
mkdir -p "$M"; umount -l "$M" 2>/dev/null
LOOP="$(losetup --find --show --partscan "$DST")" || exit 1
sleep 1
trap 'umount -l "$M" 2>/dev/null; losetup -d "$LOOP" 2>/dev/null' EXIT
mount -o ro "${LOOP}p2" "$M" || { echo "只读挂载失败"; exit 1; }
echo "  loop=$LOOP -> $M (ro)"

echo
echo "== 0. 挂的是不是那块镜像 =="
head -1 "$M/etc/os-release" | sed 's/^/  /'
CUR="$(readlink "$M/opt/ttbox/current")"
echo "  current -> $CUR"
ck "current 指向 releases/1.5.21" test "$CUR" = releases/1.5.21

echo
echo "== 1. SSH 系统层 =="
SVC_LINK="$(readlink "$M/etc/systemd/system/ssh.service" 2>/dev/null || true)"
SOCK_LINK="$(readlink "$M/etc/systemd/system/ssh.socket" 2>/dev/null || true)"
WANT_LINK="$M/etc/systemd/system/multi-user.target.wants/ssh.service"
echo "  ssh.service 软链 -> ${SVC_LINK:-<无>}"
echo "  ssh.socket  软链 -> ${SOCK_LINK:-<无>}"
ck "ssh.service enable 落链在位"     test -L "$WANT_LINK"
ck "ssh.service 未指向 /dev/null"    test "$SVC_LINK" != /dev/null
ck "ssh.socket 未指向 /dev/null"     test "$SOCK_LINK" != /dev/null
ck "旧 99-ttbox-service.conf 已移除" test ! -e "$M/etc/ssh/sshd_config.d/99-ttbox-service.conf"
ck "00-ttbox-service.conf 在位"      test -f "$M/etc/ssh/sshd_config.d/00-ttbox-service.conf"
echo "  -- 00- 内容 --"; sed 's/^/    /' "$M/etc/ssh/sshd_config.d/00-ttbox-service.conf"

echo
echo "== 2. host key（固定那套）=="
HS_ED="$(sha256sum "$M/etc/ssh/ssh_host_ed25519_key" 2>/dev/null | cut -d' ' -f1)"
HK_MODE="$(stat -c %a "$M/etc/ssh/ssh_host_ed25519_key" 2>/dev/null || true)"
echo "  成品 ed25519 sha256 = ${HS_ED:-<无>}"
echo "  记录值              = $EXP_HK_ED"
echo "  权限                = ${HK_MODE:-<无>}"
ck "host key 与记录值一致" test "$HS_ED" = "$EXP_HK_ED"
ck "host key 权限 600"     test "$HK_MODE" = 600
ck "rsa host key 在位"     test -s "$M/etc/ssh/ssh_host_rsa_key"

echo
echo "== 3. root 口令（用 openssl 从口令复算哈希 → 证明我手上的口令能进这台盒子）=="
ROOT_H="$(awk -F: '$1=="root"{print $2}' "$M/etc/shadow")"
ROOT_PREFIX="$(printf '%s' "$ROOT_H" | cut -c1-3)"
SALT="$(printf '%s' "$ROOT_H" | cut -d'$' -f3)"
CALC="$(openssl passwd -6 -salt "$SALT" "$PASS" 2>/dev/null || true)"
echo "  root 哈希前缀 = $ROOT_PREFIX  长度 ${#ROOT_H}"
echo "  盐            = $SALT"
ck "root 哈希前缀为 \$6\$"     test "$ROOT_PREFIX" = '$6$'
ck "按口令复算 == 镜像内哈希"  test "$CALC" = "$ROOT_H"

echo
echo "== 4. 构建痕迹/私钥不得落进镜像 =="
LEAK_KEY="$(find "$M/root" -name 'ssh_host_*_key' 2>/dev/null | head -1)"
LEAK_PRIV="$(find "$M" -xdev -name '*.priv.pem' 2>/dev/null | head -1)"
LEAK_TK="$(find "$M" -xdev -name '.testkeys' 2>/dev/null | head -1)"
PYC="$(find "$M/opt/ttbox" -name '__pycache__' 2>/dev/null | head -1)"
ck "无 /root/_bake"            test ! -e "$M/root/_bake"
ck "无 /root/ttbox-image 副本" test ! -e "$M/root/ttbox-image"
ck "/root 下无 host key 私钥"  test -z "$LEAK_KEY"
ck "无 __pycache__ 残留"       test -z "$PYC"
ck "无 *.priv.pem"             test -z "$LEAK_PRIV"
ck "无 .testkeys"              test -z "$LEAK_TK"

echo
echo "== 5. V4 已修项未被破坏（动了就白修）=="
DTB="$(ls "$M"/lib/firmware/*/device-tree/rockchip/rk3588-orangepi-5-plus.dtb 2>/dev/null | head -1)"
DTBS="$(sha256sum "$DTB" 2>/dev/null | cut -d' ' -f1)"
DISC="$(grep -ao disabled "$DTB" 2>/dev/null | wc -l)"
EXT_S="$(sha256sum "$M/boot/extlinux/extlinux.conf" 2>/dev/null | cut -d' ' -f1)"
echo "  DTB        : $DTB"
echo "  DTB sha256 : $DTBS"
echo "  disabled数 : $DISC"
ck "DTB 仍是已修版本(277d9de8…)"  test "$DTBS" = "$EXP_DTB"
ck "extlinux.conf 未被改"          test "$EXT_S" = "$EXP_EXT"
ck "EP 模型在位(model.rknn)"       test -s "$M/var/lib/ttbox/models/installed/EP/model.rknn"
ck "registry/active.json 在位"     test -s "$M/var/lib/ttbox/models/registry/active.json"
ck "cloud-init 已禁用"             test -f "$M/etc/cloud/cloud-init.disabled"
ck "netplan 已固化"                test -f "$M/etc/netplan/01-ttbox.yaml"
ck "journal 持久化目录在位"         test -d "$M/var/log/journal"

echo
echo "== 6. 成品文件指纹 =="
ACTUAL="$(sha256sum "$DST" | cut -d' ' -f1)"
if [ -f "$DST.sha256" ]; then
    RECORDED="$(cut -d' ' -f1 "$DST.sha256")"
    echo "  .sha256 记录: $RECORDED"
    ck "记录指纹 == 实测 sha256" test "$RECORDED" = "$ACTUAL"
else
    echo "  [i] 无 .sha256 文件"
fi
echo "  实测 sha256 = $ACTUAL"
echo "  大小        = $(stat -c %s "$DST") bytes"

echo
if [ "$FAIL" = "0" ]; then echo "== V5 成品复验：全部通过 =="; else echo "== V5 成品复验：存在失败项 =="; fi
exit $FAIL
