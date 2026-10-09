#!/usr/bin/env bash
# 连续观察匿名 16MB 映射数量（泄漏判据：数量是否增加）
set -u
CPID="${1:?need pid}"
echo "ts,anon_total_kb,big16_count,big16_sizes" > /tmp/ttbox_big16.csv
for i in $(seq 1 30); do
    python3 - "$CPID" >> /tmp/ttbox_big16.csv << 'PYEOF'
import re, sys, time
pid = sys.argv[1]
bigs = []
anon_total = 0
cur = None
cur_rss = 0
with open(f"/proc/{pid}/smaps") as f:
    for line in f:
        if re.match(r"^[0-9a-f]+-[0-9a-f]+", line):
            if cur == "[anon]":
                anon_total += cur_rss
                if cur_rss > 14000:
                    bigs.append(cur_rss)
            parts = line.split()
            cur = parts[5] if len(parts) > 5 else "[anon]"
            cur_rss = 0
        elif line.startswith("Rss:"):
            cur_rss += int(line.split()[1])
    if cur == "[anon]":
        anon_total += cur_rss
        if cur_rss > 14000:
            bigs.append(cur_rss)
sizes = "+".join(str(b//1024) for b in sorted(bigs, reverse=True))
print(f"{time.strftime('%H:%M:%S')},{anon_total//1024},{len(bigs)},{sizes or '-'}")
PYEOF
    sleep 60
done
echo "DONE" >> /tmp/ttbox_big16.csv
