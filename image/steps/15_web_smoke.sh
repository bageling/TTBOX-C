#!/usr/bin/env bash
# 在镜像内真把 web 面板跑起来，用 HTTP 打一遍（之前只做过静态检查，没验证过"真的能出页面"）
# chroot 共享宿主网络命名空间 ⇒ 绑 127.0.0.1:8000 是真实可用的
set -u
hr(){ echo "------------------------------------------------------------"; }
FAIL=0
WEB=/opt/ttbox/current/plugins/web/bin/ttbox-web
[ -x "$WEB" ] || WEB=/opt/ttbox/current/plugins/web/bin/ttbox-web.py
[ -e "$WEB" ] || { echo "!! 找不到 web 入口"; exit 1; }
echo "web 入口: $WEB"

hr; echo "[1] 以 ttbox 身份启动面板（与 systemd 同身份）"
mkdir -p /run/ttbox 2>/dev/null; chown root:ttbox /run/ttbox 2>/dev/null; chmod 0750 /run/ttbox 2>/dev/null
LOG=/root/_web_smoke.log
su -s /bin/sh ttbox -c "cd /opt/ttbox/current/plugins/web && exec '$WEB'" >"$LOG" 2>&1 &
PID=$!
echo "  pid=$PID，日志 $LOG"
for i in $(seq 1 40); do
    if curl -s -o /dev/null -m 1 http://127.0.0.1:8000/ 2>/dev/null; then break; fi
    sleep 0.5
done
sleep 1
if ! kill -0 "$PID" 2>/dev/null; then
    echo "  !! 进程已退出，日志尾部："; tail -25 "$LOG" | sed 's/^/    /'; FAIL=1
fi

hr; echo "[2] HTTP 实测"
probe(){ # $1=路径 $2=期望状态码
    code=$(curl -s -o /tmp/_p.html -w '%{http_code}' -m 5 "http://127.0.0.1:8000$1" 2>/dev/null)
    size=$(wc -c < /tmp/_p.html 2>/dev/null || echo 0)
    printf '  %-24s HTTP=%-4s %8s B  %s\n' "$1" "$code" "$size" \
        "$([ "$code" = "$2" ] && echo '[OK]' || echo '[!! 期望 '"$2"']')"
    [ "$code" = "$2" ] || FAIL=1
}
probe / 200
probe /activate 200
probe /api/license/status 200

hr; echo "[3] 首页是不是把未激活用户送去激活页"
head -c 400 /tmp/_p.html >/dev/null 2>&1
curl -s -m 5 http://127.0.0.1:8000/ -o /tmp/_home.html
if grep -qi 'activate' /tmp/_home.html; then
    echo "  [OK] 首页含 activate 引用（未激活会被引导到激活页）"
else
    echo "  [提示] 首页未出现 activate 字样（$(wc -c < /tmp/_home.html) B）"
fi

hr; echo "[4] 真的走一次激活请求（用一张假卡，看服务器有没有回话）"
resp=$(curl -s -m 15 -X POST http://127.0.0.1:8000/api/license/activate \
        -H 'Content-Type: application/json' -d '{"license_key":"SMOKE-TEST-0000-0000"}' 2>/dev/null)
echo "  返回: $resp" | head -c 600
echo
case "$resp" in
    *card_invalid*|*无效*|*未找到*) echo "  [OK] 面板→云端链路打通（服务器拒绝了这张假卡）" ;;
    *拒绝*|*refused*) echo "  [!!] 连不出去（connection refused）" ; FAIL=1 ;;
    "") echo "  [!!] 空响应" ; FAIL=1 ;;
    *) echo "  [提示] 见上方返回体" ;;
esac

hr; echo "[5] 收尾"
kill "$PID" 2>/dev/null; sleep 1; kill -9 "$PID" 2>/dev/null
echo "  已停面板"
echo "  --- 面板日志尾部（排障用）---"; tail -15 "$LOG" 2>/dev/null | sed 's/^/    /'
rm -f /tmp/_p.html /tmp/_home.html
exit "$FAIL"
