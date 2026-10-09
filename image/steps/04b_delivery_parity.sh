#!/usr/bin/env bash
# 步骤 04b · 交付一致性补齐（拿开发板实况当基准，把"镜像缺、板上有"的东西补齐并断言）
#
# 为什么单开一步：04 管的是"客户姿态"（关 SSH / 固网 / 出厂清理），本步管的是
#   "出厂即可用" —— 缺了不会报错，但客户第一脚就踩空（激活失败、面板保存预设报错）。
#
# 依据（都是实测/源码级证据，不是推测）：
#   · plugins/web/lib/cloud_client.py:41 明写「板端 default.json **出厂即含**
#     license_base_url/app_secret，出货开箱可用性不受影响」⇒ 出厂镜像必须带它。
#   · plugins/web/lib/ttbox-web.py:307 WEB_CREDENTIALS_FILE = <config_dir>/default.json，
#     而 config_dir() = /opt/ttbox/config ⇒ 凭据落点是 /opt/ttbox/config/default.json。
#   · 该文件只被 _load_cloud_credentials() 取 cloud.* 段（运行期配置真源是 core 的
#     --config /etc/ttbox/config.d，两码事）⇒ 只写 cloud 段即可，不复制整份运行配置。
set -uo pipefail
umask 022
BAKE=/root/_bake
FAIL=0
chk() { if eval "$2"; then printf '  [✓] %s\n' "$1"; else printf '  [✗] %s\n' "$1"; FAIL=1; fi; }

echo "########## 0. 路径口径自证（先确认我写的落点 == 产品代码算出来的落点）##########"
EXP_PRESETS=/opt/ttbox/presets
EXP_MOTION=/opt/ttbox/config/motion-profiles
EXP_CRED=/opt/ttbox/config/default.json
GOT="$(cd /opt/ttbox/current/plugins/web && python3 -c '
import sys; sys.path.insert(0, ".")
from lib import paths as p
print(p.presets_dir()); print(p.motion_profiles_dir()); print(p.web_credentials_file())')"
GOT_PRESETS="$(printf '%s\n' "$GOT" | sed -n 1p)"
GOT_MOTION="$(printf '%s\n'  "$GOT" | sed -n 2p)"
GOT_CRED="$(printf '%s\n'    "$GOT" | sed -n 3p)"
printf '  代码算出 presets_dir()          = %s\n' "$GOT_PRESETS"
printf '  代码算出 motion_profiles_dir() = %s\n' "$GOT_MOTION"
printf '  代码算出 web_credentials_file()= %s\n' "$GOT_CRED"
chk "presets 落点与代码一致"          '[ "$GOT_PRESETS" = "'"$EXP_PRESETS"'" ]'
chk "motion-profiles 落点与代码一致"  '[ "$GOT_MOTION"  = "'"$EXP_MOTION"'" ]'
chk "云端凭据落点与代码一致"          '[ "$GOT_CRED"    = "'"$EXP_CRED"'" ]'

echo
echo "########## 1. 云端激活凭据 /opt/ttbox/config/default.json ##########"
# 缺了会怎样：card_login fail-closed ⇒ 客户在激活页输任何卡密都失败，且错误文案是
# "云端凭据未配置"（看着像服务器挂了，其实是本地没配）。这是交付阻断项。
install -o root -g ttbox -m 0640 "$BAKE/factory/cloud.default.json" "$EXP_CRED"
python3 - <<'PY'
import json, sys
p = '/opt/ttbox/config/default.json'
d = json.load(open(p, encoding='utf-8'))
c = d.get('cloud') or {}
miss = [k for k in ('license_base_url', 'app_key', 'app_secret') if not c.get(k)]
if miss:
    print('  [✗] cloud 段缺字段:', miss); sys.exit(1)
if not str(c['license_base_url']).startswith('https://'):
    print('  [✗] license_base_url 非 https:', c['license_base_url']); sys.exit(1)
if str(c['app_secret']).startswith('<'):
    print('  [✗] app_secret 还是占位符'); sys.exit(1)
print('  [+] cloud.license_base_url =', c['license_base_url'])
print('  [+] cloud.app_key          =', c['app_key'])
print('  [+] cloud.app_secret       = %s…%s（%d 字符）' % (c['app_secret'][:4], c['app_secret'][-4:], len(c['app_secret'])))
print('  [+] 顶层键 =', list(d.keys()), '（只放凭据，不放运行配置）')
PY
[ $? -eq 0 ] || FAIL=1
chk "default.json 权限 = root:ttbox 0640" '[ "$(stat -c %U:%G:%a "$EXP_CRED")" = "root:ttbox:640" ]'
# 世界不可读：app_secret 不能给任何人看；组可读：User=ttbox 的 web 要读得到
chk "default.json 世界不可读（secret 不外泄）" 'test -z "$(find "$EXP_CRED" -perm -o+r -print)"'
chk "ttbox 身份能读到 default.json（web 以 ttbox 跑）" \
    'su -s /bin/sh ttbox -c "test -r $EXP_CRED"'

echo
echo "########## 2. 运行时目录补齐（对照板端实况，出厂就建好）##########"
# 这些目录产品自己的 fhs_init 不建（它只管 /var/lib/ttbox/*），是运行时按需建的。
# 板上能建出来，是因为开发板的 /opt/ttbox 是 777 的开发检出；出厂镜像我刻意保持
# /opt/ttbox=0755 root:root（收紧是对的），于是 web（User=ttbox）**建不出 /opt/ttbox/presets**：
#   ttbox-web.py:2662/3132/3145/3235 都是 d.mkdir(parents=True, exist_ok=True)，
#   父目录 0755 root:root ⇒ PermissionError ⇒ 面板"保存/导入预设"直接报错。
#   所以出厂必须预置，不能指望运行时自建。
install -d -o ttbox -g ttbox -m 0775 "$EXP_PRESETS"
install -d -o ttbox -g ttbox -m 0775 "$EXP_MOTION"
# license 状态目录：core 以 root 写 license.json；预置后 core 首次落盘不用自己 mkdir
install -d -o root -g root -m 0700 /var/lib/ttbox/license
# OTA 已处理归档目录（板端实况；updater 会用到）
install -d -o root -g root -m 0755 /var/lib/ttbox/ota/processed
echo "  [+] 已建：$EXP_PRESETS / $EXP_MOTION / /var/lib/ttbox/license / /var/lib/ttbox/ota/processed"

echo
echo "########## 3. 以 ttbox 身份【真跑一次写入】，不是猜权限 ##########"
# 只 stat 权限位是猜；真以 ttbox 身份创建/删除文件才是证据（本仓库踩过"用回显做断言"的坑）。
W1='touch '"$EXP_PRESETS"'/.ttbox-wtest && rm -f '"$EXP_PRESETS"'/.ttbox-wtest'
W2='touch '"$EXP_MOTION"'/.ttbox-wtest && rm -f '"$EXP_MOTION"'/.ttbox-wtest'
if su -s /bin/sh ttbox -c "$W1" 2>/dev/null; then echo "  [✓] ttbox 可写 $EXP_PRESETS"; else echo "  [✗] ttbox 不可写 $EXP_PRESETS（面板保存预设会失败）"; FAIL=1; fi
if su -s /bin/sh ttbox -c "$W2" 2>/dev/null; then echo "  [✓] ttbox 可写 $EXP_MOTION"; else echo "  [✗] ttbox 不可写 $EXP_MOTION（运动档案存不下）"; FAIL=1; fi

echo
echo "########## 4. 出厂基线对照（/opt/ttbox 本体的收紧是有意的）##########"
echo "  /opt/ttbox           = $(stat -c %U:%G:%a /opt/ttbox)   ← 开发板是 777；出厂收紧为 0755 root:root（写面交给下面的子目录）"
echo "  /opt/ttbox/config    = $(stat -c %U:%G:%a /opt/ttbox/config)"
echo "  /opt/ttbox/presets   = $(stat -c %U:%G:%a /opt/ttbox/presets)"
chk "/opt/ttbox 未被放开成 world-writable" '[ "$(stat -c %a /opt/ttbox)" != "777" ]'

echo
if [ "$FAIL" = "0" ]; then echo "== 步骤 04b 全部断言通过 =="; else echo "== 步骤 04b 有断言失败 =="; fi
exit $FAIL
