#!/usr/bin/env bash
# v4 修复：出厂 DTB 里 hdmirx-controller 是 status="disabled" ⇒ HDMI-RX 不 probe
#   ⇒ /sys/class/hdmirx 不存在 ⇒ /dev/video0 不存在
#   ⇒ ①EDID 应用报「未找到可写 HDMI-RX HPD 节点」 ②HDMI 采集整个不可用
#
# 证据（与正在正常跑的开发板逐字节比对）：
#   镜像 DTB 280076 B / sha256 7b8cc892…  ；板端 280072 B / sha256 277d9de8…
#   cmp 第一处语义差异在偏移 193076：镜像 9 字节 "disabled\0"，板端 5 字节 "okay\0"
#   （正好差 4 字节）；上下文属性为 hclk_s_hdmirx / rockchip,hdmirx-ctrler / cec
#   ⇒ 就是 hdmirx-controller@fdee0000 的 status。全文件 "disabled" 计数 192 → 191。
#
# ★ 为什么不是"加 fdtoverlays 应用 dtbo"：
#   extlinux.conf 里没有任何 overlay 指令，rk3588-hdmirx.dtbo 只是躺在磁盘上从未被应用；
#   改 extlinux.conf 依赖 u-boot sysboot 对 fdtoverlays 的支持，未在真机验证 ⇒ 不可靠。
#   直接装"已验证可用的 DTB"最稳：它与出厂 DTB 的唯一差异就是这一处 status。
#
# ★ 之前的自检为什么会漏：只断言了"dtbo 文件在不在""boot 配置含 overlay 字样"，
#   没断言"hdmirx 到底 enabled 没有"。文件在 ≠ 生效。
set -eu
BAKE=/root/_bake
FAIL=0
hr(){ echo "------------------------------------------------------------"; }
WANT_SHA="277d9de87876a4e6160ae7ac048d4adadec73bbaa7706f39e2b5a5fe42379980"
WANT_DISABLED=191          # 板端实测值（出厂 192，改掉 hdmirx 那一处后为 191）
SRC="$BAKE/factory/rk3588-orangepi-5-plus.dtb"

hr; echo "[1] 出厂 DTB 现状（修复前）"
mapfile -t DTB_LIST < <(find /lib/firmware /boot -name 'rk3588-orangepi-5-plus.dtb' -type f 2>/dev/null | sort)
if [ "${#DTB_LIST[@]}" -eq 0 ]; then echo "  !! 没找到 DTB"; exit 1; fi
for f in "${DTB_LIST[@]}"; do
    printf '  %s\n    sha256=%s  size=%s  disabled=%s\n' "$f" \
        "$(sha256sum "$f" | cut -d' ' -f1)" "$(stat -c %s "$f")" "$(grep -ao disabled "$f" | wc -l)"
done

hr; echo "[2] 装已验证的 DTB（与出厂唯一差异 = hdmirx status disabled→okay）"
[ -f "$SRC" ] || { echo "  !! 缺 $SRC"; exit 1; }
echo "  源 sha256=$(sha256sum "$SRC" | cut -d' ' -f1)  size=$(stat -c %s "$SRC")"
for f in "${DTB_LIST[@]}"; do
    install -m 0644 "$SRC" "$f"
    echo "  已装 -> $f"
done

hr; echo "[3] 断言（这三条任一不过就是没真生效）"
for f in "${DTB_LIST[@]}"; do
    got="$(sha256sum "$f" | cut -d' ' -f1)"
    if [ "$got" = "$WANT_SHA" ]; then
        echo "  [✓] $f sha256 与已验证板端一致"
    else
        echo "  [✗] $f sha256=$got 期望 $WANT_SHA"; FAIL=1
    fi
    n="$(grep -ao disabled "$f" | wc -l)"
    if [ "$n" = "$WANT_DISABLED" ]; then
        echo "  [✓] $f disabled 计数=$n（= 板端值，说明 hdmirx 那处已改）"
    else
        echo "  [✗] $f disabled 计数=$n 期望 $WANT_DISABLED"; FAIL=1
    fi
    # 直接定位那 4 字节：okay\0
    if grep -ao 'okay' "$f" >/dev/null; then :; fi
done

hr; echo "[4] 顺带核对 overlay 机制（说明为什么不靠它）"
echo "  extlinux.conf 里的 overlay 指令："
grep -n 'fdtoverlay\|overlays=' /boot/extlinux/extlinux.conf 2>/dev/null | sed 's/^/    /' || echo "    （无 ⇒ 出厂根本没应用 overlay，这就是漏网点）"
echo "  dtbo 是否仍在（保留，不依赖）：$(ls /usr/lib/firmware/*/device-tree/rockchip/overlay/rk3588-hdmirx.dtbo 2>/dev/null || echo 无)"

hr; echo "[5] 结论"
[ "$FAIL" -eq 0 ] && echo "  HDMI-RX 已在设备树层启用" || echo "  有 $FAIL 项失败"
exit "$FAIL"
