#!/usr/bin/env bash
# TTBOX Overview 验收 — 逐个 API 实测
set -u
TT="http://127.0.0.1:8000"
PASS_N=0
FAIL_N=0
FAIL_LIST=""

ok() { PASS_N=$((PASS_N+1)); printf '  ✅ %s\n' "$*"; }
bad() { FAIL_N=$((FAIL_N+1)); FAIL_LIST="$FAIL_LIST\n  ❌ $*"; printf '  ❌ %s\n' "$*"; }

note() { printf '%s\n' "$*"; }

# Python 取 JSON 字段
jget() {
    python3 -c "import json,sys; d=json.load(sys.stdin);
for k in '$1'.split('.'):
    if isinstance(d,dict): d=d.get(k,'')
    else: d=''
print(d)" 2>/dev/null
}

note ''
note '════════ TTBOX Overview 验收 ════════'

# OV-001: HTTP 200
note '--- OV-001 /api/state 200 ---'
TT_CODE=$(curl -s -o /dev/null -w '%{http_code}' --max-time 8 "$TT/api/state")
[ "$TT_CODE" = "200" ] && ok "OV-001 /api/state 200 (TT=$TT_CODE)" || bad "OV-001 HTTP TT=$TT_CODE"

# 抓取
curl -s "$TT/api/state" 2>/dev/null > /tmp/tt_state.json

# OV-002: version 存在
note '--- OV-002 version ---'
TT_VER=$(cat /tmp/tt_state.json | jget data.version)
[ -n "$TT_VER" ] && ok "OV-002 version (TT=$TT_VER)" || bad "OV-002 version empty TT=[$TT_VER]"

# OV-003: app_version
note '--- OV-003 app_version ---'
TT_AV=$(cat /tmp/tt_state.json | jget data.app_version)
[ -n "$TT_AV" ] && ok "OV-003 app_version (TT=$TT_AV)" || bad "OV-003 app_version empty"

# OV-004: capture data exists
note '--- OV-004 capture 结构 ---'
TT_CAP=$(cat /tmp/tt_state.json | python3 -c "import json,sys; d=json.load(sys.stdin).get('data',{}).get('state',{}).get('capture',{}); print(len(d))")
[ "$TT_CAP" -ge 5 ] && ok "OV-004 capture 字段数 (TT=$TT_CAP)" || bad "OV-004 capture 字段少 TT=$TT_CAP"

# OV-005: core.status
note '--- OV-005 core.status ---'
TT_CS=$(cat /tmp/tt_state.json | python3 -c "import json,sys; d=json.load(sys.stdin).get('data',{}).get('state',{}).get('core',{}); print(d.get('status',''))")
[ "$TT_CS" = "loaded" ] && ok "OV-005 core.status loaded" || bad "OV-005 core.status TT=$TT_CS"

# OV-006: fan_control 字段
note '--- OV-006 fan_control 字段 ---'
TT_FAN=$(cat /tmp/tt_state.json | python3 -c "import json,sys; d=json.load(sys.stdin).get('data',{}).get('state',{}).get('fan_control',{}); print(sorted(d.keys()))")
[ -n "$TT_FAN" ] && ok "OV-006 fan_control 字段 ($TT_FAN)" || bad "OV-006 fan_control 空"

# OV-007: /api/system/version 已随 S1 减法卸载（2026-09-18）—— 反向断言 404
note '--- OV-007 /api/system/version（已卸载） ---'
TT_SV=$(curl -s -o /dev/null -w '%{http_code}' "$TT/api/system/version")
[ "$TT_SV" = "404" ] && ok "OV-007 system/version 已卸载 (404)" || bad "OV-007 system/version TT=$TT_SV（应 404）"

# OV-008: /api/system/storage 真实
note '--- OV-008 /api/system/storage ---'
TT_ST=$(curl -s "$TT/api/system/storage" 2>/dev/null | python3 -c "import json,sys; d=json.load(sys.stdin).get('data',{}).get('rootfs',{}); print(d.get('free',''))" 2>/dev/null || echo "")
[ -n "$TT_ST" ] && [ "$TT_ST" != "0" ] && ok "OV-008 storage 真实 (TT=$TT_ST)" || bad "OV-008 storage empty TT=[$TT_ST]"

# OV-009: /api/license
note '--- OV-009 /api/license ---'
TT_LI=$(curl -s "$TT/api/license" | python3 -c "import json,sys; d=json.load(sys.stdin).get('data',{}); print('license' in d)")
[ "$TT_LI" = "True" ] && ok "OV-009 /api/license 结构完整" || bad "OV-009 /api/license TT=$TT_LI"

# OV-010: preview.jpg 已随 S1 减法卸载（2026-09-18，MJPEG 预览走 /api/preview.mjpg）—— 反向断言 404
note '--- OV-010 preview.jpg（已卸载） ---'
TT_PV=$(curl -s -o /dev/null -w '%{http_code}' --max-time 8 "$TT/api/preview.jpg")
[ "$TT_PV" = "404" ] && ok "OV-010 preview.jpg 已卸载 (404)" || bad "OV-010 preview.jpg TT=$TT_PV（应 404）"

# OV-011: Web 根路径
note '--- OV-011 Web 根路径 ---'
TT_OK=$(curl -s -o /dev/null -w '%{http_code}' --max-time 5 "$TT/")
[ "$TT_OK" = "200" ] && ok "OV-011 Web 根路径 200 (TT=$TT_OK)" || bad "OV-011 Web 根路径 TT=$TT_OK"

note ''
note '════════ 结果 ════════'
note "PASS: $PASS_N"
note "FAIL: $FAIL_N"
if [ "$FAIL_N" -gt 0 ]; then
    printf '失败项:%b\n' "$FAIL_LIST"
    echo 'FINAL: FAIL'
    exit 1
fi
echo 'FINAL: PASS'
exit 0
