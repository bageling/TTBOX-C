#!/usr/bin/env bash
# 收口复核：精确冒烟（302 重定向目标 / 激活页可用性）+ 清理本轮探测留下的痕迹
set -u
hr(){ echo "------------------------------------------------------------"; }
WEB=/opt/ttbox/current/plugins/web/bin/ttbox-web

hr; echo "[1] 启动面板并确认"根路径"把未激活用户送去哪"
mkdir -p /run/ttbox; chown root:ttbox /run/ttbox; chmod 0750 /run/ttbox
su -s /bin/sh ttbox -c "cd /opt/ttbox/current/plugins/web && exec '$WEB'" >/root/_w.log 2>&1 &
PID=$!
for i in $(seq 1 40); do curl -s -o /dev/null -m 1 http://127.0.0.1:8000/activate && break; sleep 0.5; done

LOC=$(curl -s -o /dev/null -D - -m 5 http://127.0.0.1:8000/ 2>/dev/null | grep -i '^location:' | tr -d '\r' | head -1)
echo "  /       → ${LOC:-（无 Location，即直接 200）}"
case "$LOC" in
    *activate*) echo "  [OK] 未激活会被送去激活页 —— 这就是客户看到的『第一个页面』" ;;
    *) echo "  [提示] 重定向目标不是 activate，见上" ;;
esac

hr; echo "[2] 激活页本体"
code=$(curl -s -o /tmp/_a.html -w '%{http_code}' -m 5 http://127.0.0.1:8000/activate)
echo "  /activate HTTP=$code  $(wc -c < /tmp/_a.html 2>/dev/null) B"
grep -q 'license/activate' /tmp/_a.html && echo "  [OK] 页内含 /api/license/activate 调用点" || echo "  [!!] 找不到激活端点调用"
grep -qi 'cctv2.top' /tmp/_a.html && echo "  [提示] 页面里直接出现了服务器域名" || echo "  [OK] 页面不含硬编码域名（走后端）"

hr; echo "[3] 停面板"
kill "$PID" 2>/dev/null; sleep 1; kill -9 "$PID" 2>/dev/null; echo "  已停"

hr; echo "[4] 清理本轮探测痕迹（否则下次烘焙会带进去）"
rm -rf /root/_bake /root/_w.log /root/_web_smoke.log /root/_tg_test
rm -rf /run/ttbox
find /opt/ttbox /var/lib/ttbox -name '__pycache__' -type d -exec rm -rf {} + 2>/dev/null
find /opt/ttbox /var/lib/ttbox -name '*.pyc' -delete 2>/dev/null
for p in /root/_bake /root/_w.log /run/ttbox; do
    [ -e "$p" ] && echo "  [!!] 仍残留 $p" || echo "  [OK] 无 $p"
done
LEFT="$(find /opt/ttbox /var/lib/ttbox -name '__pycache__' -o -name '*.pyc' 2>/dev/null)"
[ -z "$LEFT" ] && echo "  [OK] 无 __pycache__ / *.pyc" || { echo "  [!!] 残留:"; echo "$LEFT"; }
rm -f /tmp/_a.html
exit 0
