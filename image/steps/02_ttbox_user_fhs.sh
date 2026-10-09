#!/usr/bin/env bash
# 步骤 02 · 建立 ttbox 用户/组 + FHS 目录与权限
#
# 口径来源：scripts/ttbox_fhs_init.sh §1–§6（仓库单一真源），逐条对齐，
#           不另行发明第二套权限。差异仅在"无仓库树可读"⇒ 物料取自 /root/_bake/。
#
# 提权模型（与板端一致，非 sudo）：
#   ttbox-core.service      User=root  Group=ttbox   —— 控风扇/调频 sysfs 需 root
#   ttbox-ota.service       User=root  Group=root    —— 写 releases/ 切软链需 root
#   ttbox-usbproxy.service  User=root  Group=ttbox   —— raw-gadget 需 root
#   ttbox-web/preview       User=ttbox Group=ttbox   —— 最小权限，靠组 ttbox 连 core.sock
#   全仓无 sudoers/polkit（已核），故不给 ttbox 任何 sudo 面。
set -uo pipefail
umask 022
BAKE=/root/_bake
FAIL=0
chk() { if eval "$2"; then printf '  [✓] %s\n' "$1"; else printf '  [✗] %s\n' "$1"; FAIL=1; fi; }

echo "########## 1. 系统用户与组 ##########"
if ! getent group ttbox >/dev/null; then
    groupadd --system ttbox && echo "  [+] 创建系统组 ttbox"
else
    echo "  [=] 组 ttbox 已存在"
fi
if ! getent passwd ttbox >/dev/null; then
    useradd --system --gid ttbox --no-create-home --shell /usr/sbin/nologin ttbox \
        && echo "  [+] 创建系统用户 ttbox"
else
    echo "  [=] 用户 ttbox 已存在"
fi
# 板端实测需要：/dev/video0 是 root:video 0660，不在 video 组则 EDID 读回 EACCES
if id -nG ttbox 2>/dev/null | tr ' ' '\n' | grep -qx video; then
    echo "  [=] ttbox 已在 video 组"
else
    usermod -aG video ttbox && echo "  [+] ttbox 加入 video 组"
fi
echo "  uid/gid/组: $(id ttbox)"

echo
echo "########## 2. /etc/ttbox（配置，升级保留）##########"
# F9：/etc/ttbox 必须 root:ttbox 0775 —— web 的 /setup 在同目录内
# 新建 web_credentials.json.tmp.<pid> 再 rename，目录不可写则 EACCES ⇒ 首配 500
install -d -o root -g ttbox -m 0775 /etc/ttbox
chgrp ttbox /etc/ttbox 2>/dev/null || true
chmod 0775 /etc/ttbox
# B2：config.d 同理必须 0775 —— Core 以 ttbox 组身份原子写 10-device.json
install -d -o root -g ttbox -m 0775 /etc/ttbox/config.d
chgrp ttbox /etc/ttbox/config.d 2>/dev/null || true
chmod 0775 /etc/ttbox/config.d

# 出厂基线（只读，随发布包整体替换）
install -o root -g root -m 0644 "$BAKE/deploy/config/00-factory.json" \
        /etc/ttbox/config.d/00-factory.json
echo "  [+] 00-factory.json (root:root 0644)"
# 设备层：出厂固化。
#   ⚠ 与运行时"绝不覆盖"规则的区别：那条约束的是【线上机器的升级】（fhs_init 的
#   "存在则跳过"），防止冲掉客户调参。这里是【出厂浇筑】——我们本来就在决定
#   出厂值是什么，故 factory 覆盖存在时以覆盖为准（可重复重跑，语义幂等）。
if [ -f "$BAKE/factory/10-device.json" ]; then
    install -o root -g ttbox -m 0664 "$BAKE/factory/10-device.json" \
            /etc/ttbox/config.d/10-device.json
    echo "  [+] 10-device.json <- factory 覆盖（业主指定：开发机调参版，$(wc -c < "$BAKE/factory/10-device.json") 字节）"
else
    install -o root -g ttbox -m 0664 "$BAKE/deploy/config/10-device.json" \
            /etc/ttbox/config.d/10-device.json
    echo "  [+] 10-device.json <- 仓库 seed（$(wc -c < "$BAKE/deploy/config/10-device.json") 字节）"
fi

echo
echo "########## 3. /var/lib/ttbox（客户数据，升级绝不触碰）##########"
install -d -o ttbox -g ttbox -m 0775 /var/lib/ttbox
install -d -o ttbox -g ttbox -m 0775 /var/lib/ttbox/models
install -d -o ttbox -g ttbox -m 0775 \
    /var/lib/ttbox/models/installed /var/lib/ttbox/models/staging \
    /var/lib/ttbox/models/registry /var/lib/ttbox/models/cache \
    /var/lib/ttbox/models/quarantine /var/lib/ttbox/models/_incoming
install -d -o ttbox -g ttbox -m 0775 /var/lib/ttbox/hid
install -d -o ttbox -g ttbox -m 0700 /var/lib/ttbox/activation
install -d -o ttbox -g ttbox -m 0775 /var/lib/ttbox/motion-profiles
install -d -o ttbox -g ttbox -m 0775 /var/lib/ttbox/presets
install -d -o root  -g ttbox -m 0750 /var/lib/ttbox/update
# OTA 特权通道（ttbox_release_install.sh 首装时创建的同款）：
#   jobs = web(ttbox) 投递任务、root 更新器消费的唯一权限面 ⇒ root:ttbox 0770
install -d -o root -g ttbox -m 0770 /var/lib/ttbox/ota/jobs
install -d -o root -g ttbox -m 0755 /var/lib/ttbox/ota/processed
install -d -o root -g ttbox -m 0755 /var/lib/ttbox/ota/failed
echo "  [+] models / hid / activation / motion-profiles / presets / update / ota"

echo
echo "########## 4. /run/ttbox（tmpfs，重启清空）##########"
install -d -o ttbox -g ttbox -m 0750 /run/ttbox 2>/dev/null && \
    echo "  [+] /run/ttbox (ttbox:ttbox 0750)" || \
    echo "  [!] /run/ttbox 创建失败（tmpfs，systemd RuntimeDirectory 会兜）"

echo
echo "########## 5. /var/log/ttbox ##########"
install -d -o ttbox -g ttbox -m 0755 /var/log/ttbox
echo "  [+] /var/log/ttbox"

echo
echo "########## 6. /opt/ttbox 边角（非 release 树，升级不动）##########"
# D-A 修复（M2.07）：/opt/ttbox/config 必须 root:ttbox 0775 —— web 的云端会话
# 走 cloud_session.py 在同目录内 tmp+rename 原子写，目录不可写则激活 500
install -d -o root -g ttbox -m 0775 /opt/ttbox/config
chgrp ttbox /opt/ttbox/config 2>/dev/null || true
chmod 0775 /opt/ttbox/config
if [ ! -f /opt/ttbox/config/hardware_display.json ]; then
    # P1-1：root:ttbox 0664 —— web 是【原地重写】该文件（非 tmp+rename），需文件级写权限
    install -o root -g ttbox -m 0664 "$BAKE/deploy/config/hardware_display.json" \
            /opt/ttbox/config/hardware_display.json
    echo "  [+] /opt/ttbox/config/hardware_display.json (root:ttbox 0664)"
else
    echo "  [=] hardware_display.json 已存在"
fi
# /opt/ttbox/state：core 启停意愿 runtime_intent.json 落此（1.5.15+）
install -d -o root -g root -m 0755 /opt/ttbox/state
# /opt/ttbox/runtime/edid：开机 ttbox-edid.service 会 mkdir+chgrp；这里预置避免首启竞态
install -d -o root -g ttbox -m 0775 /opt/ttbox/runtime /opt/ttbox/runtime/edid
echo "  [+] /opt/ttbox/{config,state,runtime/edid}"

echo
echo "########## 7. 反向断言（用真实 stat 复核，不信 install 的返回值）##########"
printf '%-46s %-18s %s\n' 路径 属主 权限
while read -r p; do
    [ -e "$p" ] || { printf '%-46s %s\n' "$p" "缺失"; FAIL=1; continue; }
    printf '%-46s %-18s %s\n' "$p" "$(stat -c '%U:%G' "$p")" "$(stat -c '%a' "$p")"
done <<'EOF'
/etc/ttbox
/etc/ttbox/config.d
/etc/ttbox/config.d/00-factory.json
/etc/ttbox/config.d/10-device.json
/var/lib/ttbox
/var/lib/ttbox/models/installed
/var/lib/ttbox/activation
/var/lib/ttbox/update
/var/lib/ttbox/ota/jobs
/var/log/ttbox
/opt/ttbox/config
/opt/ttbox/config/hardware_display.json
/opt/ttbox/state
/opt/ttbox/runtime/edid
EOF

echo
echo "########## 8. 关键词断言 ##########"
chk "/etc/ttbox = root:ttbox 0775" '[ "$(stat -c %U:%G:%a /etc/ttbox)" = "root:ttbox:775" ]'
chk "config.d = root:ttbox 0775"    '[ "$(stat -c %U:%G:%a /etc/ttbox/config.d)" = "root:ttbox:775" ]'
chk "00-factory = root:root 0644"   '[ "$(stat -c %U:%G:%a /etc/ttbox/config.d/00-factory.json)" = "root:root:644" ]'
chk "10-device = root:ttbox 0664"   '[ "$(stat -c %U:%G:%a /etc/ttbox/config.d/10-device.json)" = "root:ttbox:664" ]'
chk "ota/jobs = root:ttbox 0770"    '[ "$(stat -c %U:%G:%a /var/lib/ttbox/ota/jobs)" = "root:ttbox:770" ]'
chk "hardware_display = root:ttbox 0664" '[ "$(stat -c %U:%G:%a /opt/ttbox/config/hardware_display.json)" = "root:ttbox:664" ]'
chk "ttbox 用户 shell = nologin"    "[ \"\$(getent passwd ttbox | cut -d: -f7)\" = '/usr/sbin/nologin' ]"
chk "ttbox 在 video 组"             "id -nG ttbox | tr ' ' '\n' | grep -qx video"
chk "ttbox 无 sudo 权限"            '! getent group sudo | grep -q "\bttbox\b"'

echo
if [ "$FAIL" = "0" ]; then echo "== 步骤 02 全部断言通过 =="; else echo "== 步骤 02 有断言失败 =="; fi
exit $FAIL
