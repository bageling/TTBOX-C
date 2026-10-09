#!/usr/bin/env bash
# Anon/RSS 专项采样（15 分钟，60s/次）
set -u
CPID="${1:?need core pid}"
echo "ts,anon_kb,rss_kb" > /tmp/ttbox_anon.csv
END=$(( $(date +%s) + 900 ))
while [ "$(date +%s)" -lt "$END" ]; do
    A=$(grep RssAnon /proc/$CPID/status 2>/dev/null | awk '{print $2}')
    R=$(grep VmRSS /proc/$CPID/status 2>/dev/null | awk '{print $2}')
    echo "$(date +%H:%M:%S),${A:-0},${R:-0}" >> /tmp/ttbox_anon.csv
    sleep 60
done
echo "DONE" >> /tmp/ttbox_anon.csv
