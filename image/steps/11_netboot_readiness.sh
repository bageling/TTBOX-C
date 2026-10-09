#!/usr/bin/env bash
# 开机网络/时间就绪性静态审查（不依赖 systemctl —— chroot 内 systemctl 会假成功）
# 判据全部用"文件系统真身"：unit 是否被 symlink 进 *.target.wants / 是否被 mask
# 背景：10_activation_probe.sh 在 chroot 里跑通了，但 chroot 的 /run 是宿主 bind 的，
#       宿主 systemd-resolved 在跑 ⇒ 127.0.0.53 能应答 ⇒ 那是"借来的绿"，不能证明真机能通。
set -u
FAIL=0
hr(){ echo "------------------------------------------------------------"; }

is_enabled(){  # $1=unit 名（不含 .service）
    local u="$1"
    # mask：指向 /dev/null
    for f in /etc/systemd/system/$u.service /lib/systemd/system/$u.service; do
        if [ -L "$f" ] && [ "$(readlink -f "$f")" = "/dev/null" ]; then
            echo "masked"; return
        fi
    done
    local w
    for w in /etc/systemd/system/multi-user.target.wants/$u.service \
             /etc/systemd/system/sysinit.target.wants/$u.service \
             /lib/systemd/system/multi-user.target.wants/$u.service \
             /etc/systemd/system/network-online.target.wants/$u.service \
             /etc/systemd/system/sockets.target.wants/$u.service \
             /etc/systemd/system/timers.target.wants/$u.service; do
        [ -L "$w" ] && { echo "enabled"; return; }
    done
    # 有 [Install] 但没软链 = disabled
    if [ -f /lib/systemd/system/$u.service ] || [ -f /etc/systemd/system/$u.service ]; then
        echo "disabled"
    else
        echo "absent"
    fi
}

hr; echo "[A] /etc/resolv.conf 真身（关键：指向谁？真机开机时谁能应答）"
echo "  $(ls -l /etc/resolv.conf 2>/dev/null | sed 's/  */ /g')"
echo "  readlink -f: $(readlink -f /etc/resolv.conf 2>/dev/null)"
echo "  内容:"; sed 's/^/    /' /etc/resolv.conf 2>/dev/null | head -5
R="$(readlink -f /etc/resolv.conf 2>/dev/null)"
case "$R" in
    */systemd/resolve/stub-resolv.conf)
        echo "  → 指向 systemd-resolved 的 stub 127.0.0.53"
        echo "    ⇒ 真机开机必须 systemd-resolved 真的在跑，否则 DNS 全挂"
        ;;
    */run/systemd/resolve/resolv.conf)
        echo "  → 指向 resolved 的上游直写文件（非 stub）"
        ;;
    *) echo "  → 非 resolved 系（静态/NetworkManager）" ;;
esac

hr; echo "[B] 网络/时间相关 unit 启用状态（文件系统真身，非 systemctl）"
for u in systemd-resolved systemd-networkd systemd-timesyncd systemd-networkd-wait-online \
         NetworkManager cloud-init cloud-config cloud-final; do
    st="$(is_enabled "$u")"
    printf "  %-34s %s\n" "$u" "$st"
    case "$u:$st" in
        systemd-resolved:enabled) ;;
        systemd-resolved:disabled|systemd-resolved:masked)
            echo "    !! DNS 服务未启用，而 resolv.conf 又指向它 ⇒ 开机必解析失败"; FAIL=1 ;;
        systemd-timesyncd:disabled|systemd-timesyncd:masked)
            echo "    !! 无 RTC 的板子不校时 ⇒ 证书校验可能直接挂（HTTPS 全断）"; FAIL=1 ;;
    esac
done

hr; echo "[C] netplan 配置"
for f in /etc/netplan/*.yaml /etc/netplan/*.yml; do
    [ -f "$f" ] || continue
    echo "  --- $f"
    sed 's/^/    /' "$f"
done
[ -z "$(ls /etc/netplan/*.yaml /etc/netplan/*.yml 2>/dev/null)" ] && { echo "  !! 无 netplan 配置"; FAIL=1; }

hr; echo "[D] 校时兜底（RK3588 无 RTC）"
echo "  fake-hwclock: $(command -v fake-hwclock || echo 无)"
echo "  hwclock     : $(command -v hwclock || echo 无)"
echo "  /etc/systemd/timesyncd.conf:"
sed 's/^/    /' /etc/systemd/timesyncd.conf 2>/dev/null | grep -v '^\s*#' | grep -v '^\s*$' | head -8
echo "  （全注释 = 用默认 NTP 池，需 DNS+外网 443/123 出网）"

hr; echo "[E] DNS 硬编码兜底（resolv.conf 挂了还有没有别的活路）"
echo "  /etc/hosts:"; sed 's/^/    /' /etc/hosts 2>/dev/null | head -6
grep -q 'cctv2.top' /etc/hosts 2>/dev/null && echo "  → hosts 里有 cctv2.top 兜底" || echo "  → hosts 里没有 cctv2.top"

hr; echo "[F] 结论"
[ "$FAIL" -eq 0 ] && echo "  静态审查通过" || echo "  发现 $FAIL 类开机期隐患（见上文 !! ）"
exit "$FAIL"
