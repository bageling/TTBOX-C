#!/usr/bin/env bash
# 宿主侧收尾：交付前门禁 + 卸载镜像 + 清暂存 + 产出最终制品指纹
#
# 为什么单独一步（而非塞进 chroot 步骤里）：
#   步骤脚本自身就在镜像 /root/_bake/ 下执行，chroot 内删掉 _bake 会让正在读脚本的
#   bash 报 "cannot read"（bash 是流式读取，不是一次性载入）。故清理由宿主侧做。
#
# 本脚本是**门禁**而不是流水账：任何一条断言不过就以非 0 退出，且不再继续卸挂载
#   （保留现场供排查）。断言全部在"卸挂载之前"跑，因为卸了就查不了了。
set -u
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
IMG_ROOT="${IMG_ROOT:-/mnt/img}"
WORK="${WORK:-/root/ttbox-image/work.img}"
REPO_ART="${REPO_ART:-$HERE/artifacts}"
RC=0
chk() { if eval "$2"; then printf '  [✓] %s\n' "$1"; else printf '  [✗] %s\n' "$1"; RC=1; fi; }

echo "== 1. 清除镜像内构建暂存 =="
# 必须先删再查：步骤脚本自身就在 /root/_bake/ 下执行，所以跑到收尾这一步时 _bake **一定在**，
# 先做"必须无暂存"的门禁会误判。删完再断言它真的没了。
if mountpoint -q "$IMG_ROOT"; then
    if [ -d "$IMG_ROOT/root/_bake" ]; then
        rm -rf "$IMG_ROOT/root/_bake"
        echo "  已删 $IMG_ROOT/root/_bake"
    else
        echo "  (无 _bake)"
    fi
else
    echo "  $IMG_ROOT 未挂载，跳过"
fi

echo
echo "== 2. 交付门禁（卸挂载前，镜像内实测） =="
if mountpoint -q "$IMG_ROOT"; then
    # 构建暂存必须在交付前清掉：它含 steps/ 与宿主 staging 副本，不是产品内容
    chk "镜像内无构建暂存 /root/_bake"  'test ! -e "$IMG_ROOT/root/_bake"'
    # world-writable 断言排除软链（软链自身 mode 恒 0777，chmod 无效，属显示假象）
    chk "发布树无 world-writable 实体文件/目录" \
        'test -z "$(find "$IMG_ROOT/opt/ttbox" \( -type f -o -type d \) -perm -o+w -print -quit)"'
    chk "配置树无 world-writable 实体文件/目录" \
        'test -z "$(find "$IMG_ROOT/etc/ttbox" "$IMG_ROOT/var/lib/ttbox" \( -type f -o -type d \) -perm -o+w -print -quit)"'
    chk "无残留 __pycache__"             'test -z "$(find "$IMG_ROOT/opt/ttbox" -name "__pycache__" -print -quit)"'
    chk "无宿主 /root/ttbox-image 副本落进镜像" 'test ! -e "$IMG_ROOT/root/ttbox-image"'
    # ★ 不钉死某个版本号（1.5.21 是 V5 当时的版本，硬编码会让每次升版都假红）。
    #   真门禁是「current 指向一个确实浇筑完成的 release」；要钉版本就传 TTBOX_EXPECT_VER。
    CUR_VER="$(readlink "$IMG_ROOT/opt/ttbox/current" | sed 's#^releases/##')"
    chk "current 指向已浇筑的 releases/${CUR_VER:-<空>}" \
        '[ -n "$CUR_VER" ] && [ -d "$IMG_ROOT/opt/ttbox/releases/$CUR_VER" ] && [ -f "$IMG_ROOT/opt/ttbox/releases/$CUR_VER/RELEASE_MANIFEST.json" ]'
    if [ -n "${TTBOX_EXPECT_VER:-}" ]; then
        chk "浇筑版本 == 期望 $TTBOX_EXPECT_VER" '[ "$CUR_VER" = "$TTBOX_EXPECT_VER" ]'
    fi
    HK_DIR="${TTBOX_HOSTKEY_DIR:-}"
    HK_DST_SHA="$(sha256sum "$IMG_ROOT/etc/ssh/ssh_host_ed25519_key" 2>/dev/null | cut -d" " -f1)"
    HK_SRC_SHA="$(sha256sum "${HK_DIR}/ssh_host_ed25519_key" 2>/dev/null | cut -d" " -f1)"
    chk "SSH 已启用（enable 落链在位）"     '[ -L "$IMG_ROOT/etc/systemd/system/multi-user.target.wants/ssh.service" ]'
    chk "cloud-init 已禁用"              'test -f "$IMG_ROOT/etc/cloud/cloud-init.disabled"'
    chk "SSH host key = 注入的固定那套"    '[ -n "$HK_SRC_SHA" ] && [ "$HK_DST_SHA" = "$HK_SRC_SHA" ]'
    echo "  -- 镜像内 ttbox 相关体积 --"
    du -sh "$IMG_ROOT/opt/ttbox" "$IMG_ROOT/var/lib/ttbox" 2>/dev/null | sed 's/^/     /'
else
    echo "  $IMG_ROOT 未挂载 —— 跳门禁（若此时还没浇筑过，属正常）"
fi

echo
echo "== 3. 卸载镜像 =="
bash "$HERE/29_umount.sh"

echo
echo "== 4. 宿主侧 staging 清理 =="
STAGE="${TTBOX_STAGE_DIR:-/root/ttbox-image/_stage}"
[ -d "$STAGE" ] && { rm -rf "$STAGE"; echo "  已删 $STAGE"; } || echo "  (无 staging)"
echo "  注：/root/ttbox-image/work.img 与 original.sha256 保留（回写与追溯用）"

echo
echo "== 5. 自检报告落盘（宿主 /root -> 仓库 artifacts） =="
mkdir -p "$REPO_ART"
for f in selfcheck.txt; do
    if [ -f "/root/$f" ]; then
        cp -f "/root/$f" "$REPO_ART/${f%.txt}-$(date +%F).txt"
        echo "  已归档 $REPO_ART/${f%.txt}-$(date +%F).txt"
    fi
done

echo
echo "== 6. 最终制品指纹 =="
chk "work.img 与出厂镜像 sha256 不同（确实被改过）" \
    '[ "$(sha256sum "$WORK" | cut -d" " -f1)" != "$(cat /root/ttbox-image/original.sha256 | cut -d" " -f1)" ]'
echo "  出厂 sha256: $(cat /root/ttbox-image/original.sha256 2>/dev/null | cut -d' ' -f1)"
echo "  成品 sha256: $(sha256sum "$WORK" | cut -d' ' -f1)"
echo "  成品大小  : $(stat -c %s "$WORK") bytes  ($(du -h "$WORK" | cut -f1))"
sha256sum "$WORK" > /root/ttbox-image/work.sha256
echo "  指纹已存: /root/ttbox-image/work.sha256"

echo
if [ "$RC" = "0" ]; then
    echo "== 收尾门禁：全部通过 =="
else
    echo "== 收尾门禁：存在失败项（见上方 [✗]） =="
fi
exit $RC
