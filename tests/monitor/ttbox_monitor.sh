#!/usr/bin/env bash
# TTBOX 连续运行监控（E2E 第三轮）：10 分钟采样内存/FD/线程/检测/延迟/温度
# 用法: nohup bash /tmp/ttbox_monitor.sh > /tmp/ttbox_monitor.log 2>&1 &
set -u
OUT=/tmp/ttbox_monitor.csv
echo "ts,core_rss_kb,core_fd,core_threads,web_rss_kb,web_fd,web_threads,cpu_percent,capture_fps,e2e_ms,infer_run_ms,temp_c" > "$OUT"
END=$(( $(date +%s) + 600 ))
SAMPLES=0
while [ "$(date +%s)" -lt "$END" ]; do
    CORE_PID=$(pgrep -f "ttbox_core_main" | head -1)
    WEB_PID=$(pgrep -f "ttbox-web.py" | head -1)
    CRSS=0; CFD=0; CTH=0; WRSS=0; WFD=0; WTH=0
    if [ -n "$CORE_PID" ]; then
        CRSS=$(awk '/VmRSS/{print $2}' /proc/$CORE_PID/status 2>/dev/null || echo 0)
        CFD=$(ls /proc/$CORE_PID/fd 2>/dev/null | wc -l)
        CTH=$(ls /proc/$CORE_PID/task 2>/dev/null | wc -l)
    fi
    if [ -n "$WEB_PID" ]; then
        WRSS=$(awk '/VmRSS/{print $2}' /proc/$WEB_PID/status 2>/dev/null || echo 0)
        WFD=$(ls /proc/$WEB_PID/fd 2>/dev/null | wc -l)
        WTH=$(ls /proc/$WEB_PID/task 2>/dev/null | wc -l)
    fi
    CPU=$(top -bn1 2>/dev/null | grep "Cpu(s)" | awk '{print $2}' | cut -d. -f1)
    # IPC 指标
    IPC=$(python3 -c "
import socket, json
try:
    s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM); s.settimeout(3)
    s.connect(__import__('os').environ['TTBOX_IPC_SOCKET'])
    s.sendall(json.dumps({'type':'GET_STATUS'}).encode()+b'\n')
    buf=b''
    while b'\n' not in buf: buf += s.recv(65536)
    m = json.loads(buf.decode())['data']['metrics']
    print(round(m.get('capture_fps',0),1), round(m.get('e2e_ms',0),2), round(m.get('infer_run_ms',0),2))
except Exception as e:
    print('0 0 0')
" 2>/dev/null)
    CFPS=$(echo "$IPC" | awk '{print $1}')
    E2E=$(echo "$IPC" | awk '{print $2}')
    RUN=$(echo "$IPC" | awk '{print $3}')
    TEMP=$(cat /sys/class/thermal/thermal_zone0/temp 2>/dev/null || echo 0)
    TEMP_C=$(echo "scale=1; $TEMP/1000" | bc 2>/dev/null || echo 0)
    echo "$(date +%H:%M:%S),$CRSS,$CFD,$CTH,$WRSS,$WFD,$WTH,$CPU,$CFPS,$E2E,$RUN,$TEMP_C" >> "$OUT"
    SAMPLES=$((SAMPLES+1))
    sleep 10
done
echo "DONE samples=$SAMPLES" >> "$OUT"
