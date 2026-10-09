#!/usr/bin/env bash
# 抓取匿名映射明细（Rss>0 的 [anon] 段），用于前后对比定位泄漏
set -u
CPID="${1:?need pid}"
OUT="${2:-/tmp/ttbox_anon_maps.txt}"
python3 - "$CPID" "$OUT" << 'PYEOF'
import re, sys
pid, out = sys.argv[1], sys.argv[2]
mappings = []
cur = None
rss = 0
with open(f"/proc/{pid}/smaps") as f:
    for line in f:
        m = re.match(r"([0-9a-f]+)-([0-9a-f]+) \S+ \S+ \S+ \S+ +\S+\s*(.*)", line)
        if m:
            if cur is not None:
                mappings.append((cur, rss))
            cur = m.group(3).strip() if m.group(3).strip() else "[anon]"
            rss = 0
        elif line.startswith("Rss:"):
            rss += int(line.split()[1])
    if cur is not None:
        mappings.append((cur, rss))
anon = [(p, r) for p, r in mappings if p == "[anon]" and r > 0]
anon.sort(key=lambda x: -x[1])
with open(out, "w") as f:
    for p, r in anon:
        f.write(f"{r}\n")
print(f"匿名映射(>0): {len(anon)} 个, 总Rss={sum(r for _,r in anon)//1024}KB")
PYEOF
