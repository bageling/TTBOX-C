#!/usr/bin/env bash
# ttbox_doctor.sh — TTBOX 一键诊断（D10/D15，2026-09-18 定案 O14/I-30/I-32）
#
# 检查项（全绿才算健康，任何一项 FAIL 都以非零退出）：
#   1. current 软链与发布树
#   2. 4 个服务 unit active（V1.0.52 起 ttbox-preview 已下线，不再含它）
#   3. 硬件前置：/dev/video0 存在（HDMI 采集）；overlay 是否生效
#   4. 软件依赖：python3-cryptography（OTA 更新器硬依赖，缺则 ImportError）
#   5. OTA 配置：更新器/公钥/任务目录存在且权限正确；ttbox.sh 的服务器地址非占位
#      （V1.0.52：原「ttbox.sh vs ttbox-web.py 两处同值」检查随 Python 后端移除而改；
#        现对照物为 C++ 二进制里的字面量，提取不到时只提示不判 FAIL）
#   6. ensure 自愈链：ttbox-ensure.timer active
set -euo pipefail

FAILS=0
log()    { printf '[doctor] %s\n' "$*"; }
pass()   { printf '[doctor][PASS] %s\n' "$*"; }
fail()   { printf '[doctor][FAIL] %s\n' "$*" >&2; FAILS=$((FAILS + 1)); }
SCRIPTS="${TTBOX_CURRENT:-/opt/ttbox/current}/scripts"
# ★ V1.0.52：web.py 已随 Python 清理移除，改用 C++ 二进制作为地址对照物
WEB_BIN="${TTBOX_CURRENT:-/opt/ttbox/current}/bin/ttbox_web"

# 1. current 断链（D15 告警源的复检）
if [ -L /opt/ttbox/current ] && [ -d /opt/ttbox/current/ ]; then
    pass "current -> $(readlink -f /opt/ttbox/current)（版本 $(basename "$(readlink -f /opt/ttbox/current)")）"
else
    fail "current 断链——发布树不可用（这正是 ensure 告警的根因场景）"
fi

# 2. 服务 active
for u in ttbox-core ttbox-web ttbox-usbproxy ttbox-edid; do
    if systemctl is-active --quiet "${u}.service" 2>/dev/null; then
        pass "${u}.service active"
    else
        fail "${u}.service 非 active（$(systemctl is-active "${u}.service" 2>/dev/null || echo unknown)）"
    fi
done

# 3. 硬件前置
if [ -e /dev/video0 ]; then pass "/dev/video0 存在（HDMI 采集节点）"; else fail "/dev/video0 不存在"; fi
if grep -qs 'dtoverlay\|rockchip' /boot/config.txt /boot/firmware/config.txt /boot/extlinux/extlinux.conf 2>/dev/null; then
    pass "boot 配置含 overlay 配置（人工确认 hdmirx overlay 已生效）"
else
    fail "未找到 boot overlay 配置（/dev/video0 若缺失优先查这里）"
fi

# 4. 软件依赖（升级器硬依赖；缺失即 ImportError）
if python3 -c 'import cryptography' 2>/dev/null; then
    pass "python3-cryptography 可用"
else
    fail "python3-cryptography 缺失——OTA 更新器无法运行（apt-get install -y python3-cryptography）"
fi

# 5. OTA 配置
if [ -f "/opt/ttbox/current/deploy/keys/ttbox-ota-2026b.pub" ]; then
    pass "OTA 公钥在位（ttbox-ota-2026b）"
else
    fail "OTA 公钥缺失（deploy/keys/ttbox-ota-2026b.pub）"
fi
# ★ V1.0.56：更新器入口由 python3 脚本换成 C++ 二进制（bin/ttbox_ota）
OTA_BIN="${TTBOX_CURRENT:-/opt/ttbox/current}/bin/ttbox_ota"
if [ -x "${OTA_BIN}" ]; then
    pass "OTA 更新器在位"
else
    fail "OTA 更新器缺失（${OTA_BIN}）"
fi
if [ -d /var/lib/ttbox/ota/jobs ] && [ "$(stat -c %a /var/lib/ttbox/ota/jobs 2>/dev/null || echo 000)" = "770" ]; then
    pass "OTA 任务目录在位（root:ttbox 0770）"
else
    fail "OTA 任务目录缺失或权限不对（应 root:ttbox 0770）"
fi
# 服务器地址漂移（V1.0.52：真源 = scripts/ttbox.sh；对照物 = C++ 二进制字面量）
#   原检查对照 plugins/web/bin/ttbox-web.py，该文件已随 Python 后端移除。
#   板端无源码，故从 ttbox_web 二进制里提取 OTA 地址做对照；提取不到时只提示、不判 FAIL
#   （避免 strings 行为差异造成误报）。
OTA_URL="$(grep -m1 -oP '^OTA_SERVER_URL="\K[^"]+' "${SCRIPTS}/ttbox.sh" 2>/dev/null || true)"
BIN_URL="$(strings "$WEB_BIN" 2>/dev/null | grep -m1 -oE 'https://[0-9a-zA-Z.:_-]+/ota' || true)"
if [ -n "$OTA_URL" ]; then
    if [ -n "$BIN_URL" ]; then
        if [ "$OTA_URL" = "$BIN_URL" ]; then
            pass "OTA 服务器地址 ttbox.sh 与 C++ 二进制一致（${OTA_URL}）"
        else
            fail "OTA 服务器地址漂移：ttbox.sh='${OTA_URL}' ttbox_web='${BIN_URL}'（必须同值）"
        fi
    else
        log "提示：未能从 ${WEB_BIN} 提取 OTA 地址（strings 不可用或二进制不同），跳过一致性对照"
    fi
    case "$OTA_URL" in
        *example.com*) fail "OTA 服务器地址仍是占位值——交付前必须替换为正式服务器" ;;
        *) pass "OTA 服务器地址非占位值" ;;
    esac
else
    fail "无法读取 ttbox.sh 的 OTA_SERVER_URL（${SCRIPTS}/ttbox.sh）"
fi

# 6. ensure 自愈链
if systemctl is-active --quiet ttbox-ensure.timer 2>/dev/null; then
    pass "ttbox-ensure.timer active（自愈巡检在跑）"
else
    fail "ttbox-ensure.timer 未启用——current 断链将无人告警"
fi

echo ""
if [ "$FAILS" -eq 0 ]; then
    echo "[doctor] 全部通过 ✓"
    exit 0
else
    echo "[doctor] ${FAILS} 项未通过（见上方 FAIL 行）" >&2
    exit 1
fi
