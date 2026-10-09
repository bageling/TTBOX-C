#!/usr/bin/env bash
# Phase 2: Hotkey & Profiles 验收（仅 TTBOX）
set -u
TT="http://127.0.0.1:8000"
PASS_N=0
FAIL_N=0
FAIL_LIST=""

ok() { PASS_N=$((PASS_N+1)); printf '  ✅ %s\n' "$*"; }
bad() { FAIL_N=$((FAIL_N+1)); FAIL_LIST="$FAIL_LIST\n  ❌ $*"; printf '  ❌ %s\n' "$*"; }

note() { printf '%s\n' "$*"; }

note '════════ Phase 2: Hotkey & Profiles ════════'

# --- 获取 TTBOX 配置 ---
TT_STATE=$(curl -s "$TT/api/state" 2>/dev/null)
TT_CFG=$(echo "$TT_STATE" | python3 -c "import json,sys; d=json.load(sys.stdin).get('data',{}).get('config',{}); print(json.dumps(d))" 2>/dev/null)

# HP-001: auto-start 端点已随 S1 减法卸载（2026-09-18）—— 反向断言 404
note '--- HP-001 auto-start（已卸载） ---'
TT_AS_CODE=$(curl -s -o /dev/null -w '%{http_code}' "$TT/api/settings/auto-start")
[ "$TT_AS_CODE" = "404" ] && ok "HP-001 auto-start 已卸载 (404)" || bad "HP-001 auto-start TT=$TT_AS_CODE（应 404）"

# HP-002: controller 字段存在（从 state.config 获取）
note '--- HP-002 controller 字段 ---'
TT_CTRL=$(echo "$TT_CFG" | python3 -c "import json,sys; d=json.load(sys.stdin).get('ai',{}).get('controller',{}); print('kp_x' in d and 'kp_y' in d)" 2>/dev/null)
[ "$TT_CTRL" = "True" ] && ok "HP-002 controller 字段存在" || bad "HP-002 controller TT=$TT_CTRL"

# HP-003: aim_profiles 字段存在
note '--- HP-003 aim_profiles ---'
TT_AP=$(echo "$TT_CFG" | python3 -c "import json,sys; d=json.load(sys.stdin).get('aim_profiles',[]); print(len(d))" 2>/dev/null)
[ "$TT_AP" -gt 0 ] 2>/dev/null && ok "HP-003 aim_profiles 存在 (TT=$TT_AP)" || bad "HP-003 aim_profiles TT=$TT_AP"

# HP-004: hotkey 字段
note '--- HP-004 hotkey 字段 ---'
TT_HK=$(echo "$TT_CFG" | python3 -c "import json,sys; d=json.load(sys.stdin).get('aim_profiles',[{}])[0].get('hotkey',''); print(d)" 2>/dev/null)
[ -n "$TT_HK" ] && ok "HP-004 hotkey 存在 (TT=$TT_HK)" || bad "HP-004 hotkey TT=[$TT_HK]"

# HP-005: PUT /api/config 修改生效
note '--- HP-005 config PUT 修改 ---'
TT_PUT=$(curl -s -X PUT "$TT/api/config" -H "Content-Type: application/json" -d '{"video_detection_confidence": 0.26}' 2>/dev/null)
TT_PUT_OK=$(echo "$TT_PUT" | python3 -c "import json,sys; d=json.load(sys.stdin); print(d.get('ok'))" 2>/dev/null)
# 读回
TT_VAL=$(curl -s "$TT/api/state" | python3 -c "import json,sys; d=json.load(sys.stdin).get('data',{}).get('config',{}).get('video_detection_confidence',''); print(d)" 2>/dev/null)
TT_VAL_OK=$(python3 -c "print('OK' if abs(float('$TT_VAL')-0.26)<1e-4 else 'BAD')" 2>/dev/null)
[ "$TT_PUT_OK" = "True" ] && [ "$TT_VAL_OK" = "OK" ] && ok "HP-005 config PUT 修改生效 (TT=$TT_VAL)" || bad "HP-005 config PUT TT_OK=$TT_PUT_OK TT_VAL_OK=$TT_VAL_OK"

# 恢复原值
curl -s -X PUT "$TT/api/config" -H "Content-Type: application/json" -d '{"video_detection_confidence": 0.25}' >/dev/null 2>&1

# HP-006: motion-profiles 存在
note '--- HP-006 motion-profiles ---'
TT_MP=$(curl -s "$TT/api/motion-profiles" | python3 -c "import json,sys; d=json.load(sys.stdin).get('data',{}); print(type(d).__name__)" 2>/dev/null)
[ -n "$TT_MP" ] && ok "HP-006 motion-profiles 存在 (TT=$TT_MP)" || bad "HP-006 motion-profiles TT=$TT_MP"

# HP-007: motion-profiles 拒绝创建
note '--- HP-007 motion-profiles 拒绝创建 ---'
TT_MPC=$(curl -s -X POST "$TT/api/motion-profiles" -H "Content-Type: application/json" -d '{"name":"test"}' | python3 -c "import json,sys; d=json.load(sys.stdin); print(d.get('ok'))" 2>/dev/null)
[ "$TT_MPC" = "False" ] && ok "HP-007 motion-profiles 拒绝创建 (TT=$TT_MPC)" || bad "HP-007 motion-profiles TT=$TT_MPC"

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
