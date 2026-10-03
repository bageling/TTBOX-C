"""timefmt.py — IPC wire 时间量到 ISO8601 的转换单点（2026-10-02 web 换写法 S1）。

从 ``plugins/web/bin/ttbox-web.py`` 拆出。行为逐字保留。
"""
from __future__ import annotations

from datetime import datetime, timezone


def _unix_to_iso(sec) -> str:
    """IPC wire 的 `expires_at` / `grace_until` 单位是 unix **秒**；本函数是全仓唯一的
    秒 → ISO8601(UTC, 'Z') 转换点（T1.07b 陷阱 4：不得在别处二次换算）。

    0 / 负 / 不可解析 ⇒ `''`（语义 = "无到期"，**不是** `1970-01-01T00:00:00Z`）。
    """
    try:
        sec = int(sec)
    except (TypeError, ValueError):
        return ''
    if sec <= 0:
        return ''
    try:
        return datetime.fromtimestamp(sec, timezone.utc).isoformat().replace('+00:00', 'Z')
    except (OverflowError, OSError, ValueError):
        return ''
