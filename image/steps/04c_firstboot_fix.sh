#!/usr/bin/env bash
# v3 修复：把"刷机后第一个页面就连不上服务器"的两条镜像侧成因堵死
#
#   成因 1（主凶）：RK3588 无 RTC，首次开机时钟 = /etc/fake-hwclock.data = 镜像构建日
#                  2024-10-22。云端证书有效期 2026-06-20 ~ 2026-12-18，
#                  时钟早于生效日 ⇒ TLS 报 "certificate is not yet valid"，HTTPS 全断，
#                  前端只会显示成"连接服务器失败"。
#   成因 2（次凶）/：/var/lib/ttbox/models 是空的 ⇒ core 起不来 ⇒ 主页 /api/state
#                  轮询失败 ⇒ 状态徽标"未连接"，用户会当成"没接服务器"。
#
# ★ 本脚本在 chroot 内执行。chroot 与宿主共享时间命名空间，
#   **绝不能在这里真的跑 date -s**（会改掉 WSL 宿主时钟）⇒ 只安装、只做语法校验。
set -eu
BAKE=/root/_bake
FAIL=0
hr(){ echo "------------------------------------------------------------"; }

hr; echo "[1] 时钟下限守卫（防 HTTPS 因"证书尚未生效"全断）"

# 1a. fake-hwclock 兜底值抬到证书有效期内（比 2024 构建日强，NTP 未通时也能用）
echo "2026-09-20 12:00:00" > /etc/fake-hwclock.data
echo "  fake-hwclock.data -> $(cat /etc/fake-hwclock.data)"

# 1b. 开机守卫脚本
mkdir -p /usr/local/sbin
cat > /usr/local/sbin/ttbox-time-guard.sh <<'EOS'
#!/bin/sh
# TTBOX 开机时钟下限守卫
# RK3588 无 RTC：首次开机时钟来自 /etc/fake-hwclock.data（镜像构建日）。
# 若该日期早于云端 TLS 证书的生效日，HTTPS 握手会直接失败
# （certificate is not yet valid），表现为"激活页连不上服务器"。
# 这里把时钟抬到证书有效期内，给 systemd-timesyncd 争取到同步完成的时间。
FLOOR="2026-06-21 00:00:00"   # 证书生效日 2026-06-20 之后一天，留余量
SAFE="2026-09-20 12:00:00"    # 落在证书有效期中段
now=$(date -u +%s 2>/dev/null || echo 0)
floor=$(date -u -d "$FLOOR" +%s 2>/dev/null || echo 0)
[ "$floor" -gt 0 ] || exit 0
if [ "$now" -lt "$floor" ]; then
    old=$(date -u -d "@$now" '+%Y-%m-%d %H:%M:%S' 2>/dev/null || echo '?')
    if date -u -s "$SAFE" >/dev/null 2>&1; then
        echo "ttbox-time-guard: 时钟 $old 早于证书生效日，已抬到 $SAFE UTC（待 NTP 校正）"
    else
        echo "ttbox-time-guard: 时钟 $old 偏旧且无法设置（权限/容器），跳过"
    fi
fi
exit 0
EOS
chmod 0755 /usr/local/sbin/ttbox-time-guard.sh
echo "  已安装 /usr/local/sbin/ttbox-time-guard.sh ($(wc -c < /usr/local/sbin/ttbox-time-guard.sh) B)"
sh -n /usr/local/sbin/ttbox-time-guard.sh && echo "  语法校验通过"

# 1c. 单元：必须排在 timesyncd 与 ttbox 服务之前
cat > /etc/systemd/system/ttbox-time-guard.service <<'EOS'
[Unit]
Description=TTBOX first-boot clock floor (RK3588 has no RTC)
DefaultDependencies=no
Before=systemd-timesyncd.service time-sync.target ttbox-web.service ttbox-core.service
After=systemd-remount-fs.service

[Service]
Type=oneshot
RemainAfterExit=yes
ExecStart=/usr/local/sbin/ttbox-time-guard.sh

[Install]
WantedBy=sysinit.target
EOS
chmod 0644 /etc/systemd/system/ttbox-time-guard.service
echo "  已安装 /etc/systemd/system/ttbox-time-guard.service ($(wc -c < /etc/systemd/system/ttbox-time-guard.service) B)"
mkdir -p /etc/systemd/system/sysinit.target.wants
ln -sf /etc/systemd/system/ttbox-time-guard.service \
       /etc/systemd/system/sysinit.target.wants/ttbox-time-guard.service
[ -L /etc/systemd/system/sysinit.target.wants/ttbox-time-guard.service ] \
    && echo "  已启用（sysinit.target.wants）" || { echo "  !! 启用失败"; FAIL=1; }

hr; echo "[2] 预装出厂模型 EP（否则 core 不健康 ⇒ 主页"未连接"）"
SRC="$BAKE/factory/model_EP"
DST=/var/lib/ttbox/models
WANT_SHA="$(python3 -c "import json;print(json.load(open('$SRC/manifest.json'))['sha256'])" 2>/dev/null || echo '')"
echo "  manifest 声明 sha256: $WANT_SHA"

if [ ! -f "$SRC/model.rknn" ]; then
    echo "  !! 缺 $SRC/model.rknn，跳过预装"; FAIL=1
else
    install -d -o ttbox -g ttbox -m 0775 "$DST/installed/EP/validation" "$DST/registry"
    for f in model.rknn manifest.json metadata.json; do
        install -o root -g ttbox -m 0644 "$SRC/$f" "$DST/installed/EP/$f"
    done
    install -o root -g ttbox -m 0644 "$SRC/validation/ok.json" "$DST/installed/EP/validation/ok.json"
    install -o root -g ttbox -m 0644 "$SRC/active.json"        "$DST/registry/active.json"
    echo "  已就位:"; find "$DST/installed/EP" "$DST/registry" -type f -printf '    %s\t%M %u:%g %p\n'

    GOT_SHA="$(sha256sum "$DST/installed/EP/model.rknn" | cut -d' ' -f1)"
    if [ -n "$WANT_SHA" ] && [ "$GOT_SHA" = "$WANT_SHA" ]; then
        echo "  [OK] 模型 sha256 与 manifest 一致: $GOT_SHA"
    else
        echo "  !! sha256 不符: got=$GOT_SHA want=$WANT_SHA"; FAIL=1
    fi
    # 真读一遍（core 以 ttbox 身份跑，读不到等于白装）
    if su -s /bin/sh ttbox -c "head -c 16 '$DST/installed/EP/model.rknn' >/dev/null"; then
        echo "  [OK] ttbox 用户可读模型"
    else
        echo "  !! ttbox 用户读不了模型"; FAIL=1
    fi
fi

hr; echo "[3] 结论"
[ "$FAIL" -eq 0 ] && echo "  v3 修复项全部就位" || echo "  有 $FAIL 项失败"
exit "$FAIL"
