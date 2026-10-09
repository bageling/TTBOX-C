#!/usr/bin/env bash
# 找"刷机后第一个页面就说连不上服务器"的镜像侧成因
# 假设 A：无 RTC ⇒ 开机时钟回到镜像构建日（2024-10-22）⇒ LE 证书"尚未生效" ⇒ TLS 全断
# 假设 B：models 目录空 ⇒ core 起不来/不健康 ⇒ /api/state 失败 ⇒ 主页状态徽标显示"未连接"
set -u
hr(){ echo "------------------------------------------------------------"; }

hr; echo "[A] 开机时钟来源（RK3588 无 RTC）"
echo "  /etc/fake-hwclock.data 内容: $(cat /etc/fake-hwclock.data 2>/dev/null || echo '（无）')"
echo "  ⇒ 若无/为构建日，首次开机系统时间 = $(cat /etc/fake-hwclock.data 2>/dev/null || echo 2024-10-22)"
echo "  LE 证书有效期（cctv2.top）: 2026-06-20 ~ 2026-12-18"
echo "  ⇒ 时钟停在 2024/2025 时，curl/requests 报 certificate is not yet valid，HTTPS 全断"
echo "  可用校时通道: systemd-timesyncd(NTP/UDP123) ；云激活走 TCP 10086"
ls -l /etc/systemd/system/systemd-timesyncd.service.d/ 2>/dev/null || echo "  timesyncd 无 drop-in"

hr; echo "[B] 模型是否随镜像出厂（core 健康的前提）"
echo "  10-device.json:"
cat /etc/ttbox/config.d/10-device.json 2>/dev/null | sed 's/^/    /'
echo
echo "  模型目录:"
for d in /opt/ttbox/models /opt/ttbox/current/models /var/lib/ttbox/models; do
    if [ -d "$d" ]; then
        n="$(find "$d" -maxdepth 2 -type f 2>/dev/null | wc -l)"
        echo "    $d  存在，文件数=$n"
        find "$d" -maxdepth 2 2>/dev/null | head -10 | sed 's/^/      /'
    else
        echo "    $d  不存在"
    fi
done

hr; echo "[C] web 面板与激活端点是否随镜像出厂"
echo "  ttbox-web.service: $([ -L /etc/systemd/system/multi-user.target.wants/ttbox-web.service ] && echo enabled || echo '未启用!')"
echo "  ttbox-core.service: $([ -L /etc/systemd/system/multi-user.target.wants/ttbox-core.service ] && echo enabled || echo '未启用!')"
echo "  ttbox-usbproxy.service: $([ -L /etc/systemd/system/multi-user.target.wants/ttbox-usbproxy.service ] && echo enabled || echo '未启用!')"
echo
echo "  激活端点 /api/license/activate 是否存在:"
grep -rn "license/activate" /opt/ttbox/current/plugins/web/ 2>/dev/null | head -5 | sed 's/^/    /'
echo
echo "  激活模板:"; ls -l /opt/ttbox/current/plugins/web/templates/activate.html 2>/dev/null | sed 's/^/    /'

hr; echo "[D] 云端凭据是否真的落盘（激活的命根子）"
C=/opt/ttbox/config/default.json
if [ -f "$C" ]; then
    ls -l "$C" | sed 's/^/    /'
    python3 - "$C" <<'PY'
import json,sys
try:
    d=json.load(open(sys.argv[1]))
    c=d.get('cloud',{})
    print("    license_base_url:", c.get('license_base_url'))
    print("    app_key        :", c.get('app_key'))
    s=c.get('app_secret') or ''
    print("    app_secret     : len=%d %s"%(len(s), (s[:6]+'…'+s[-4:]) if len(s)>12 else '(异常)'))
except Exception as e:
    print("    解析失败:", e)
PY
else
    echo "    !! $C 不存在 —— 客户无法在线激活"; fi

hr; echo "[E] 结论线索"
echo "  若 B 显示模型数为 0：主页『未连接』= core 的 /api/state 轮询失败，与云服务器无关"
echo "  若 A 显示时钟停在 2024：激活必失败，且报错会被前端显示成『连接服务器失败』"
exit 0
