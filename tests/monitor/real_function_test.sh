#!/usr/bin/env bash
# TTBOX 真实功能验收测试（第二轮）— 不只看 API 返回值，验证真实副作用
# 用法: bash real_function_test.sh
# 退出码: 0=全PASS  1=有FAIL
set -u
BASE="http://127.0.0.1:8000"
CFG="/opt/ttbox/config/default.json"
PASS_N=0
FAIL_N=0
FAIL_LIST=""

note() { printf '%s\n' "$*"; }

ok() { PASS_N=$((PASS_N+1)); printf '  ✅ %s\n' "$*"; }

bad() { FAIL_N=$((FAIL_N+1)); FAIL_LIST="$FAIL_LIST\n  ❌ $*"; printf '  ❌ %s\n' "$*"; }

# http_json METHOD URL JSON_BODY -> stdout
http_json() {
    local method="$1" url="$2" body="${3:-}"
    if [ -n "$body" ]; then
        curl -s -X "$method" "$BASE$url" -H 'Content-Type: application/json' -d "$body"
    else
        curl -s -X "$method" "$BASE$url"
    fi
}

# 断言字段值: assert_eq LABEL ACTUAL EXPECTED
assert_eq() {
    local label="$1" actual="$2" expected="$3"
    if [ "$actual" = "$expected" ]; then
        ok "$label (= $actual)"
    else
        bad "$label (期望 $expected 实际 $actual)"
    fi
}

# 用 python 解析 json: jget JSON_FILE 'a.b.c' [default]
jget() {
    python3 -c "
import json,sys
d=json.load(open('$1'))
for k in '$2'.split('.'):
    if isinstance(d,dict) and k in d: d=d[k]
    else: print(''); sys.exit()
print(d)
"
}

# 文件字段: fget KEY_PATH JSON_FILE
fget() { jget "$2" "$1"; }

# err_has JSON_FILE CN_TEXT：解码 JSON error 字段判断包含中文（jsonify 输出 \uXXXX）
err_has() {
    python3 -c "
import json,sys
try:
    d=json.load(open('$1'))
except Exception:
    print('NO'); sys.exit()
e=d.get('error','') or ''
print('YES' if '$2' in e else 'NO')
"
}

CORE_STATE=$(systemctl is-active ttbox-core 2>/dev/null)

note ''
note '════════ A 组：配置真实链路（Core 不在时落盘 + 回读） ════════'

# A1: GET /api/config 不返回空结构（Core 不在时从文件兜底）
A1_LEN=$(http_json GET /api/config | python3 -c 'import json,sys; print(len(json.load(sys.stdin).get("data",{})))')
if [ "${A1_LEN:-0}" -ge 10 ]; then ok "GET /api/config 字段数=$A1_LEN (>=10, 非空)"; else bad "GET /api/config 字段数=$A1_LEN"; fi

# A2: PUT /api/config 真实落盘 runtime_profile 键 + 返回新值
TMP_OUT=/tmp/rft_cfg_put.json
http_json PUT /api/config '{"video_detection_confidence": 0.77}' > "$TMP_OUT"
A2_OK=$(jget "$TMP_OUT" ok)
A2_VDC=$(jget "$TMP_OUT" data.video_detection_confidence)
A2_FILE=$(python3 -c "
import json
c=json.load(open('$CFG'))
print(c.get('runtime_profile',{}).get('inference',{}).get('confidence',''))
")
A2_VDC_OK=$(python3 -c "print('OK' if abs(float('$A2_VDC')-0.77)<1e-4 else 'BAD')")
assert_eq "A2 PUT ok" "$A2_OK" "True"
if [ "$A2_VDC_OK" = "OK" ]; then ok "A2 PUT 返回 vdc≈0.77 ($A2_VDC)"; else bad "A2 PUT 返回 vdc ($A2_VDC)"; fi
A2_FILE_OK=$(python3 -c "print('OK' if abs(float('$A2_FILE')-0.77)<1e-4 else 'BAD')")
if [ "$A2_FILE_OK" = "OK" ]; then ok "A2 文件 confidence≈0.77 ($A2_FILE)"; else bad "A2 文件 confidence ($A2_FILE)"; fi

# A3: 重启持久化（PUT → 重启 web → GET 保持）
http_json PUT /api/config '{"video_detection_iou": 0.55}' > /dev/null
systemctl restart ttbox-web >/dev/null 2>&1
sleep 2
A3_VDC=$(http_json GET /api/config | python3 -c 'import json,sys; print(json.load(sys.stdin).get("data",{}).get("video_detection_confidence",""))')
A3_VDI=$(http_json GET /api/config | python3 -c 'import json,sys; print(json.load(sys.stdin).get("data",{}).get("video_detection_iou",""))')
A3_VDC_OK=$(python3 -c "print('OK' if abs(float('$A3_VDC')-0.77)<1e-4 else 'BAD')")
A3_VDI_OK=$(python3 -c "print('OK' if abs(float('$A3_VDI')-0.55)<1e-4 else 'BAD')")
if [ "$A3_VDC_OK" = "OK" ]; then ok "A3 重启后 vdc 保持 ($A3_VDC)"; else bad "A3 重启后 vdc ($A3_VDC)"; fi
if [ "$A3_VDI_OK" = "OK" ]; then ok "A3 重启后 vdi 保持 ($A3_VDI)"; else bad "A3 重启后 vdi ($A3_VDI)"; fi
http_json PUT /api/config '{"video_detection_confidence": 0.25, "video_detection_iou": 0.45}' > /dev/null

# A4: mouse PUT 真实落盘 runtime_profile.mouse.mode
TMP_MOUSE=/tmp/rft_mouse.json
http_json PUT /api/hardware/mouse/mode '{"mode": "kmbox"}' > "$TMP_MOUSE"
A4_MODE=$(python3 -c "
import json
c=json.load(open('$CFG'))
print(c.get('runtime_profile',{}).get('mouse',{}).get('mode',''))
")
A4_OFF=$(jget "$TMP_MOUSE" data._core_offline)
assert_eq "A4 文件 mouse.mode" "$A4_MODE" "kmbox"
# Core active 时热更新成功无 _core_offline（正确）；inactive 时落盘 + 标记
A4_OFF_EXPECT=$([ "$CORE_STATE" = "active" ] && echo "__ABSENT__" || echo "True")
if [ "$A4_OFF_EXPECT" = "__ABSENT__" ]; then
    if [ "$A4_OFF" = "__ABSENT__" ] || [ -z "$A4_OFF" ]; then ok "A4 Core 在线热更新（无 _core_offline）"; else bad "A4 不应有 _core_offline ($A4_OFF)"; fi
else
    assert_eq "A4 _core_offline 诚实标记" "$A4_OFF" "True"
fi
http_json PUT /api/hardware/mouse/mode '{"mode": "local_hid"}' > /dev/null

note ''
note '════════ B 组：防火墙真实副作用（iptables） ════════'

# B1: 空 ip 拒绝
http_json POST /api/system/lan-blocklist '{}' > /tmp/rft_b1.json
if [ "$(err_has /tmp/rft_b1.json '请选择或输入要拉黑的局域网 IP')" = "YES" ]; then ok "B1 空ip拒绝"; else bad "B1 空ip拒绝 ($(cat /tmp/rft_b1.json))"; fi

# B2: set 后 iptables 真实 DROP 规则
http_json POST /api/system/lan-blocklist '{"ip": "192.168.0.99"}' > /dev/null
B2_RULE=$(iptables -S TTBOX_BLOCKLIST 2>/dev/null | grep -c '192.168.0.99')
assert_eq "B2 iptables DROP 规则数" "$B2_RULE" "1"

# B3: status 从 iptables 回读
B3_IP=$(http_json GET /api/system/lan-blocklist | python3 -c 'import json,sys; d=json.load(sys.stdin).get("data",{}); print(",".join(d.get("blocked_ips",[])))')
assert_eq "B3 status blocked_ips" "$B3_IP" "192.168.0.99"

# B4: clear 后规则清空
http_json DELETE /api/system/lan-blocklist > /dev/null
B4_CNT=$(iptables -S TTBOX_BLOCKLIST 2>/dev/null | grep -c '192.168.0.99')
assert_eq "B4 clear 后规则数" "$B4_CNT" "0"

# B5: TTBOX 独立 chain 保持存在
B5_CHAIN=$(iptables -S TTBOX_BLOCKLIST 2>/dev/null | grep -c 'TTBOX_BLOCKLIST')
if [ "$B5_CHAIN" -ge 1 ]; then ok "B5 TTBOX_BLOCKLIST chain 存在"; else bad "B5 TTBOX_BLOCKLIST chain 异常"; fi

note ''
note '════════ C 组：诚实错误（拒绝假成功） ════════'

# C1: reactivate 对齐 TTBOX 契约（400 + 无需修复）
http_json POST /api/system/reactivate > /tmp/rft_c1.json
C1_CODE=$(curl -s -o /dev/null -w '%{http_code}' -X POST "$BASE/api/system/reactivate")
if [ "$(err_has /tmp/rft_c1.json '当前授权状态正常')" = "YES" ]; then ok "C1 reactivate 文案"; else bad "C1 reactivate 文案 ($(cat /tmp/rft_c1.json))"; fi
assert_eq "C1 reactivate HTTP" "$C1_CODE" "400"

# C2/C3/C4: cloud-encrypted 已随 S1 减法卸载（2026-09-18）—— 反向断言 404
C2_CODE=$(curl -s -o /dev/null -w '%{http_code}' -X POST "$BASE/api/models/cloud-encrypted" -H 'Content-Type: application/json' -d '{}')
assert_eq "C2 cloud-encrypted 已卸载" "$C2_CODE" "404"

# C5: aim-trace（Core 未运行→400 诚实拒绝；Core 运行中→200 真实记录）
C5_EXPECT=$([ "$CORE_STATE" = "active" ] && echo "200" || echo "400")
C5_CODE=$(curl -s -o /dev/null -w '%{http_code}' -X POST "$BASE/api/diagnostics/aim-trace" -H 'Content-Type: application/json' -d '{"duration_sec": 3}')
assert_eq "C5 aim-trace HTTP (core=$CORE_STATE)" "$C5_CODE" "$C5_EXPECT"

# C6: state core 段真实反映（Core active→loaded，inactive→not_running）
C6=$(http_json GET /api/state | python3 -c 'import json,sys; print(json.load(sys.stdin).get("data",{}).get("state",{}).get("core",{}).get("status",""))')
C6_EXPECT=$([ "$CORE_STATE" = "active" ] && echo "loaded" || echo "not_running")
assert_eq "C6 state.core.status (core=$CORE_STATE)" "$C6" "$C6_EXPECT"

# C7: license core 段真实反映（Core active→True，inactive→False）
C7_EXPECT=$([ "$CORE_STATE" = "active" ] && echo "True" || echo "False")
C7=$(http_json GET /api/license | python3 -c 'import json,sys; print(json.load(sys.stdin).get("data",{}).get("core",{}).get("loaded",""))')
assert_eq "C7 license.core.loaded (core=$CORE_STATE)" "$C7" "$C7_EXPECT"

# C8: calibration PUT 反映 Core 状态（active→应用成功 ready=True；inactive→ready=False）
TMP_CAL=/tmp/rft_cal.json
http_json PUT /api/control/calibration '{"gain_x_px_per_count": 0.6, "gain_y_px_per_count": 0.6, "response_delay_ms": 8.5}' > "$TMP_CAL"
C8_READY=$(jget "$TMP_CAL" data.runtime.ready)
C8_EXPECT=$([ "$CORE_STATE" = "active" ] && echo "True" || echo "False")
assert_eq "C8 calibration ready (core=$CORE_STATE)" "$C8_READY" "$C8_EXPECT"
rm -f /tmp/rft_cfg_put.json /tmp/rft_mouse.json /tmp/rft_cal.json /tmp/rft_b1.json /tmp/rft_c1.json /tmp/rft_c2.json /tmp/rft_c3.json

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
