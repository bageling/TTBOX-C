#!/usr/bin/env bash
# 步骤 03 · 浇筑发布树 + 安装并启用 systemd 单元
#
# 关键决定：**不手写浇筑逻辑**，直接调用仓库真脚本
#   scripts/ttbox_release_install.sh <ver> <payload> --activate
# 理由：它是"校验 payload → staging → 全量 sha256 → 原子发布 → 原子换链 → unit 断言 →
#       过渡软链"的唯一权威实现；照它重写一遍就是第二套真源，必然漂移。
#       首次部署语义（TTBOX_SYSTEMD=0 跳过 restart/健康检查）与 ttbox_fhs_init.sh 完全一致
#       —— 因为 chroot 里没有 PID1，本来就起不了服务。
#
# 单元安装同样不手写：调用 scripts/ttbox_ensure_services.sh（unit 安装逻辑的唯一收敛点）。
set -uo pipefail
umask 022
BAKE=/root/_bake
VER="${1:-1.5.21}"
FAIL=0
chk() { if eval "$2"; then printf '  [✓] %s\n' "$1"; else printf '  [✗] %s\n' "$1"; FAIL=1; fi; }

echo "########## 1. payload 预检 ##########"
test -f "$BAKE/payload/RELEASE_MANIFEST.json" || { echo "  [✗] 缺 RELEASE_MANIFEST.json"; exit 1; }
if ! python3 - "$BAKE/payload/RELEASE_MANIFEST.json" <<'PY'
import json, sys, os
m = json.load(open(sys.argv[1], encoding="utf-8"))
root = os.path.dirname(sys.argv[1])
files = m["files_sha256"]
missing = [p for p in files if not os.path.isfile(os.path.join(root, p))]
print("  版本=%s  声明文件=%d  实际缺失=%d" % (m["version"], len(files), len(missing)))
print("  git_sha=%s" % m.get("git_sha", ""))
if missing:
    print("  缺失清单: %s" % missing[:10]); sys.exit(1)
PY
then
    echo "  [✗] payload 不完整，中止"; exit 1
fi

echo
echo "########## 2. payload 可执行位预检 ##########"
for f in bin/ttbox_core_main bin/ttbox_web bin/ttbox_edid \
         usbproxy/usb-proxy usbproxy/board/run-ttbox-usb-proxy.sh; do
    printf '  %-46s %s\n' "$f" "$(stat -c %a "$BAKE/payload/$f" 2>/dev/null || echo 缺失)"
done

echo
echo "########## 3. 浇筑（调用仓库真脚本）##########"
TTBOX_SYSTEMD=0 bash "$BAKE/scripts/ttbox_release_install.sh" "$VER" "$BAKE/payload" --activate
RC=$?
echo "  release_install 退出码=$RC"
test "$RC" -eq 0 || { echo "  [✗] 浇筑失败，中止"; exit 1; }

echo
echo "########## 4. 浇筑结果断言 ##########"
REL="/opt/ttbox/releases/$VER"
chk "current 是软链"                 'test -L /opt/ttbox/current'
chk "current -> releases/$VER"       "test \"\$(readlink /opt/ttbox/current)\" = \"releases/$VER\""
chk "releases/$VER 是实体目录"        "test -d $REL"
chk "release 根可遍历 (0755)"        "test \"\$(stat -c %a $REL)\" = 755"
chk "过渡软链 plugins -> current/plugins" 'test "$(readlink /opt/ttbox/plugins)" = current/plugins'
chk "过渡软链 scripts -> current/scripts" 'test "$(readlink /opt/ttbox/scripts)" = current/scripts'
chk "core 二进制可执行"               'test -x /opt/ttbox/current/bin/ttbox_core_main'
chk "web 后端可执行"                  'test -x /opt/ttbox/current/bin/ttbox_web'
chk "EDID 入口可执行"                 'test -x /opt/ttbox/current/bin/ttbox_edid'
chk "usbproxy ELF 可执行"             'test -x /opt/ttbox/current/usbproxy/usb-proxy'
chk "usbproxy 启动脚本可执行"         'test -x /opt/ttbox/current/usbproxy/board/run-ttbox-usb-proxy.sh'
chk "librknnrt.so 随版本就地交付"     'test -f /opt/ttbox/current/lib/librknnrt.so'
chk "RELEASE_MANIFEST 随树交付"       'test -f /opt/ttbox/current/RELEASE_MANIFEST.json'
chk "OTA 验签公钥随树交付"            'test -f /opt/ttbox/current/deploy/keys/ttbox-ota-2026b.pub'
echo "  release 树文件数: $(find "$REL" -type f | wc -l)"
echo "  release 树大小  : $(du -sh "$REL" | cut -f1)"
echo "  releases/ 内容  : $(ls /opt/ttbox/releases/ | tr '\n' ' ')"

echo
echo "########## 5. 安装 systemd 单元（调用 ensure 真脚本）##########"
TTBOX_SYSTEMD=0 bash /opt/ttbox/current/scripts/ttbox_ensure_services.sh
echo "  ensure 退出码=$?"

UNITS="ttbox-core.service ttbox-web.service ttbox-usbproxy.service ttbox-edid.service ttbox-ota.path ttbox-ota.service"
for u in $UNITS; do
    chk "/etc/systemd/system/$u 已安装" "test -f /etc/systemd/system/$u"
done
chk "core unit 与发布树逐字节一致"  'cmp -s /etc/systemd/system/ttbox-core.service /opt/ttbox/current/deploy/systemd/ttbox-core.service'
chk "web unit 与发布树逐字节一致"   'cmp -s /etc/systemd/system/ttbox-web.service /opt/ttbox/current/deploy/systemd/ttbox-web.service'

echo
echo "########## 5b. 安装 ensure 自举单元 ##########"
# ttbox_ensure_services.sh 的受管清单【不含】ensure 自身（避免自举循环），
# 故这两个单元必须由装机流程显式放入 —— 与 ttbox_fhs_init.sh::install_ensure_units 同款。
# 漏掉的后果：timers.target.wants/ttbox-ensure.timer 指向不存在的单元 = 悬空软链，
# 开机后自愈机制静默失效（systemd 只在 journal 留一句 not found）。
for u in ttbox-ensure.service ttbox-ensure.timer; do
    install -o root -g root -m 0644 "/opt/ttbox/current/deploy/systemd/$u" "/etc/systemd/system/$u"
    echo "  [+] /etc/systemd/system/$u"
done
chk "ensure.service 已安装"  'test -f /etc/systemd/system/ttbox-ensure.service'
chk "ensure.timer 已安装"    'test -f /etc/systemd/system/ttbox-ensure.timer'

echo
echo "########## 5c. 发布树权限复核（world-writable 是提权面，必须为零）##########"
printf '   %-46s %s\n' "bin/ttbox_core_main" "$(stat -c %a /opt/ttbox/current/bin/ttbox_core_main)"
printf '   %-46s %s\n' "plugins/web/bin/ttbox-web" "$(stat -c %a /opt/ttbox/current/plugins/web/bin/ttbox-web)"
printf '   %-46s %s\n' "usbproxy/usb-proxy" "$(stat -c %a /opt/ttbox/current/usbproxy/usb-proxy)"
printf '   %-46s %s\n' "lib/librknnrt.so" "$(stat -c %a /opt/ttbox/current/lib/librknnrt.so)"
chk "core 二进制不是 world-writable"   'test -z "$(find /opt/ttbox/current/bin \( -type f -o -type d \) -perm -o+w -print -quit)"'
chk "整棵发布树无 world-writable"      'test -z "$(find /opt/ttbox/current/ \( -type f -o -type d \) -perm -o+w -print -quit)"'
chk "core 二进制恰为 755"              'test "$(stat -c %a /opt/ttbox/current/bin/ttbox_core_main)" = 755'
chk "librknnrt.so 恰为 644"            'test "$(stat -c %a /opt/ttbox/current/lib/librknnrt.so)" = 644'

echo
echo "########## 6. enable 单元（chroot 内无 PID1，逐条核对落链）##########"
# 板端实测的 enable 集合：multi-user.target.wants 6 个 + timers.target.wants 1 个。
# ttbox-ota.service 【无 [Install] 段】⇒ 本就不该被 enable（由 .path 触发）；
# ttbox-ensure.service 同理不由自己 enable（由 timer 拉起），故均不在清单内。
MU="ttbox-core.service ttbox-web.service ttbox-usbproxy.service ttbox-edid.service ttbox-ota.path"
enable_unit() {
    local u="$1" tgt="$2"
    if systemctl enable "$u" >/dev/null 2>&1 && [ -L "/etc/systemd/system/${tgt}.wants/$u" ]; then
        echo "  [enable] systemctl enable $u"
    else
        # chroot 兜底：手动浇筑与 systemctl enable 等价的软链
        mkdir -p "/etc/systemd/system/${tgt}.wants"
        ln -sfn "/etc/systemd/system/$u" "/etc/systemd/system/${tgt}.wants/$u"
        echo "  [enable] 手动落链 $u -> ${tgt}.wants/"
    fi
}
for u in $MU; do enable_unit "$u" multi-user.target; done
enable_unit ttbox-ensure.timer timers.target

echo "  -- multi-user.target.wants 下的 ttbox 链 --"
ls /etc/systemd/system/multi-user.target.wants/ 2>/dev/null | grep '^ttbox' | sed 's/^/     /'
echo "  -- timers.target.wants 下的 ttbox 链 --"
ls /etc/systemd/system/timers.target.wants/ 2>/dev/null | grep '^ttbox' | sed 's/^/     /'

echo
echo "########## 7. enable 状态断言（与板端黄金态逐条对齐）##########"
for u in $MU; do chk "$u 已落链" "test -L /etc/systemd/system/multi-user.target.wants/$u"; done
chk "ttbox-ensure.timer 已落链" "test -L /etc/systemd/system/timers.target.wants/ttbox-ensure.timer"
chk "ttbox-ota.service 未被 enable（设计如此）" 'test ! -e /etc/systemd/system/multi-user.target.wants/ttbox-ota.service'
echo "  -- 软链可解析（-e 会跟随软链，悬空则失败）--"
for u in $MU; do chk "$u 软链可解析" "test -e /etc/systemd/system/multi-user.target.wants/$u"; done
chk "ttbox-ensure.timer 软链可解析" "test -e /etc/systemd/system/timers.target.wants/ttbox-ensure.timer"

echo
echo "########## 8. 单元语法/段位预检（无 PID1 时的替代验证）##########"
if command -v systemd-analyze >/dev/null 2>&1; then
    for u in $UNITS ttbox-ensure.service ttbox-ensure.timer; do
        out="$(systemd-analyze verify "/etc/systemd/system/$u" 2>&1)"
        if [ -z "$out" ]; then
            printf '  [✓] %s 校验通过\n' "$u"
        else
            printf '  [!] %s 有输出:\n' "$u"
            printf '%s\n' "$out" | head -5 | sed 's/^/        /'
        fi
    done
else
    echo "  (无 systemd-analyze，跳过)"
fi

echo
if [ "$FAIL" = "0" ]; then echo "== 步骤 03 全部断言通过 =="; else echo "== 步骤 03 有断言失败 =="; fi
exit $FAIL
