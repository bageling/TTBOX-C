#!/usr/bin/env bash
# 步骤 05 · 镜像全量自检（阶段三）
#
# 设计原则：**尽量不自写判据**。能交给产品自带验证器的一律交给它，避免"我自己写的
#   断言自己通过"。自写的部分只覆盖自带工具不查的维度（镜像制品层面）。
#
#   权威判据来源：
#     A) /root/_bake/scripts/ttbox_release_verify.sh —— 发布树 7 项独立体检
#     B) /root/_bake/scripts/ttbox_doctor.sh        —— 现场诊断（部分项需 PID1，chroot 内标注为 N/A）
#     C) 本脚本 —— 镜像级：依赖闭包 / 权限 / enable 态 / 交付姿态 / 制品 sha256
set -uo pipefail
BAKE=/root/_bake
FAIL=0
chk() { if eval "$2"; then printf '  [✓] %s\n' "$1"; else printf '  [✗] %s\n' "$1"; FAIL=1; fi; }
SEC() { printf '\n=========== %s ===========\n' "$*"; }

SEC "A. 发布树独立体检（产品自带 ttbox_release_verify.sh）"
# 该脚本第 4、5 项正是关键：RUNPATH 是否自包含、设备专有库是否解析到本 release 的 lib/。
# 放到镜像内跑，判据完全来自发布树自身，不由本脚本再解释一遍。
bash "$BAKE/scripts/ttbox_release_verify.sh" 2>&1 | sed 's/^/  /'
RV_RC=${PIPESTATUS[0]}
if [ "$RV_RC" -eq 0 ]; then echo "  >>> release_verify 通过（rc=0）"; else echo "  >>> release_verify 未通过（rc=$RV_RC）"; FAIL=1; fi

SEC "B. 动态链接闭包（core 二进制在【本镜像内】能否被加载器解析）"
CORE=/opt/ttbox/current/bin/ttbox_core_main
echo "  -- readelf -d 的 NEEDED 清单 --"
readelf -d "$CORE" 2>/dev/null | sed -n 's/.*(NEEDED).*\[\(.*\)\]/    \1/p'
echo "  -- RUNPATH --"
readelf -d "$CORE" 2>/dev/null | sed -n 's/.*(RUNPATH).*\[\(.*\)\]/    \1/p'
echo "  -- ldd 实际解析（每条必须落到具体文件，'not found' 即失败）--"
LDD_OUT="$(ldd "$CORE" 2>&1)"
printf '%s\n' "$LDD_OUT" | sed 's/^/    /'
if printf '%s\n' "$LDD_OUT" | grep -q 'not found'; then
    echo "  [✗] 存在未解析的依赖"; FAIL=1
else
    echo "  [✓] 无 'not found'"
fi
# 逐条断言：NEEDED 里除 libc 家族外都要能解析
chk "librga.so.2 可解析"        "printf '%s' \"\$LDD_OUT\" | grep -q 'librga\\.so\\.2 => /'"
chk "libopencv_core 可解析"     "printf '%s' \"\$LDD_OUT\" | grep -q 'libopencv_core\\.so\\.4\\.5d => /'"
chk "libopencv_imgproc 可解析"  "printf '%s' \"\$LDD_OUT\" | grep -q 'libopencv_imgproc\\.so\\.4\\.5d => /'"
chk "libjpeg.so.8 可解析"       "printf '%s' \"\$LDD_OUT\" | grep -q 'libjpeg\\.so\\.8 => /'"
chk "librknnrt 解析到本 release 的 lib/" \
    "printf '%s' \"\$LDD_OUT\" | grep -q 'librknnrt\\.so => /opt/ttbox/current/bin/\\?\\.\\./lib/librknnrt\\.so'"
echo "  -- libc/loader 家族（镜像自带，逐条列出以便人工复核）--"
printf '%s\n' "$LDD_OUT" | grep -E 'ld-linux|libc\.so|libstdc|libm\.so|libgcc|libtbb|libz\.so|libpthread|libdl\.so' | sed 's/^/    /'

SEC "C. Python 依赖闭包（出货 .py 的第三方 import 是否都能导入）"
# 做法：扫出货树内全部 .py 的顶层 import，用 python3 importlib.util.find_spec 逐个解析。
# 只对"非本地模块"报错——本地模块（plugins.*/framework/ttbox_motion/edid/lib 等）由 sys.path 提供。
python3 - <<'PY'
import ast, importlib.util, os, sys
ROOT = "/opt/ttbox/current"
STDLIB = set(sys.stdlib_module_names)
# ★ V1.0.61：system_host / system_common 已随「去 Python」批次删除，故从 LOCAL 移除。
#   （该集合是"出货树内自带模块"的宽容名单，多留无害、少留更准。）
LOCAL = {"edid", "lib", "preview_contract", "ttbox_motion", "framework", "plugins",
         "fan", "wifi", "lan_blocklist", "module_registry"}
mods = set()
for dp, dn, fn in os.walk(ROOT):
    dn[:] = [d for d in dn if d != "__pycache__"]
    for f in fn:
        if not f.endswith(".py"):
            continue
        try:
            tree = ast.parse(open(os.path.join(dp, f), "rb").read())
        except SyntaxError as e:
            print("  [✗] 语法错误 %s: %s" % (os.path.join(dp, f), e)); continue
        for node in ast.walk(tree):
            if isinstance(node, ast.Import):
                for a in node.names:
                    mods.add(a.name.split(".")[0])
            elif isinstance(node, ast.ImportFrom):
                if node.level == 0 and node.module:
                    mods.add(node.module.split(".")[0])

# ★ 区分"第三方包"与"出货树内自带模块"：后者（如 plugins/model/model_service.py）
#   在发布树里以 .py / 包目录形式随货发出，由调用方自己的 sys.path 提供，**不是**漏装的依赖。
#   判据 = 名字能在 ROOT 下找到同名 .py 或包目录。若不做这层区分，会把包内模块误报成缺依赖。
shipped = {}
for dp, dn, fn in os.walk(ROOT):
    dn[:] = [d for d in dn if d != "__pycache__"]
    for f in fn:
        if f.endswith(".py"):
            shipped.setdefault(f[:-3], os.path.join(dp, f))
    for d in dn:
        shipped.setdefault(d, os.path.join(dp, d))

rest = sorted(m for m in mods if m not in STDLIB and m not in LOCAL and not m.startswith("_"))
local_shipped = [m for m in rest if m in shipped]
third = [m for m in rest if m not in shipped]
if local_shipped:
    print("  出货树内自带模块（非第三方依赖）:")
    for m in local_shipped:
        print("    · %s  <- %s" % (m, shipped[m].replace(ROOT, "current")))
print("  stdlib 之外的**第三方**顶层模块: %s" % (third or "(无)"))
missing = []
for m in third:
    if importlib.util.find_spec(m) is None:
        missing.append(m)
if missing:
    print("  [✗] 无法导入: %s" % missing)
    sys.exit(1)
print("  [✓] 全部第三方模块在镜像内可导入 (%d 个: %s)" % (len(third), ", ".join(third) or "-"))
PY
[ $? -eq 0 ] || FAIL=1
echo "  -- 关键第三方版本 --"
python3 -c "import flask, waitress, cryptography; print('    flask %s / waitress %s / cryptography %s' % (flask.__version__, waitress.__version__ if hasattr(waitress,'__version__') else 'n/a', cryptography.__version__))" 2>&1 | sed 's/^/  /'

SEC "D. 全部出货 .py 语法编译（含 plugins/scripts/framework）"
# ★ 坑：/opt/ttbox/current 是软链，`find <软链>` 默认**不下降**（只 stat 自身、当场输出一条）⇒
#   不带尾斜杠的 find 会得到 0 个文件，py_compile 一个都没跑，却打印"全部通过"（实测踩到）。
#   修法：路径带尾斜杠强制解析软链；再拿 RELEASE_MANIFEST 的 .py 条数做**等式校验**，
#   多一个少一个都判失败 —— 不用"至少 N 个"这种会随版本漂移的魔法阈值。
PY_EXP="$(python3 -c "import json;m=json.load(open('/opt/ttbox/current/RELEASE_MANIFEST.json'));print(sum(1 for k in m['files_sha256'] if k.endswith('.py')))" 2>/dev/null || echo -1)"
PY_LIST="$(find /opt/ttbox/current/ -name '*.py' -not -path '*__pycache__*' 2>/dev/null)"
PY_N="$(printf '%s\n' "$PY_LIST" | grep -c . || true)"
echo "  磁盘 .py 数量=$PY_N   清单(RELEASE_MANIFEST) .py 数量=$PY_EXP"
PYFAIL=0
if [ "$PY_N" -ne "$PY_EXP" ] || [ "$PY_N" -le 0 ]; then
    echo "  [✗] 数量不等（或为 0）——find 未下降 / 树不全 / 清单漂移，拒绝给通过"
    FAIL=1
else
    while read -r f; do
        [ -n "$f" ] || continue
        python3 -m py_compile "$f" 2>/dev/null || { echo "  [✗] $f"; PYFAIL=1; }
    done <<< "$PY_LIST"
    [ "$PYFAIL" -eq 0 ] && echo "  [✓] 全部 .py 编译通过（$PY_N 个）" || FAIL=1
fi
find /opt/ttbox/current/ -name '__pycache__' -type d -exec rm -rf {} + 2>/dev/null || true

SEC "E. 制品完整性（对照 RELEASE_MANIFEST.json 全量 sha256）"
python3 - <<'PY'
import hashlib, json, os, sys
root = "/opt/ttbox/current"
m = json.load(open(os.path.join(root, "RELEASE_MANIFEST.json"), encoding="utf-8"))
bad, n = [], 0
for rel, want in sorted(m["files_sha256"].items()):
    p = os.path.join(root, rel)
    n += 1
    if not os.path.isfile(p):
        bad.append((rel, "缺失")); continue
    got = hashlib.sha256(open(p, "rb").read()).hexdigest()
    if got != want:
        bad.append((rel, "sha256 不符"))
print("  版本=%s  git_sha=%s" % (m["version"], m.get("git_sha", "")))
print("  逐文件比对 %d 项" % n)
if bad:
    for rel, why in bad: print("  [✗] %s: %s" % (rel, why))
    sys.exit(1)
print("  [✓] %d 项全部一致" % n)
PY
[ $? -eq 0 ] || FAIL=1

SEC "F. 用户/组/权限（独立重读，不复用浇筑时的判断）"
echo "  -- ttbox 身份 --"
id ttbox 2>&1 | sed 's/^/    /'
echo "  -- 目录属主与权限 --"
printf '    %-46s %-14s %s\n' 路径 属主 权限
while read -r p; do
    if [ -e "$p" ]; then printf '    %-46s %-14s %s\n' "$p" "$(stat -c %U:%G "$p")" "$(stat -c %a "$p")"
    else printf '    %-46s %s\n' "$p" "缺失"; FAIL=1; fi
done <<'EOF'
/etc/ttbox
/etc/ttbox/config.d
/etc/ttbox/config.d/00-factory.json
/etc/ttbox/config.d/10-device.json
/var/lib/ttbox
/var/lib/ttbox/models
/var/lib/ttbox/models/installed
/var/lib/ttbox/hid
/var/lib/ttbox/activation
/var/lib/ttbox/update
/var/lib/ttbox/ota/jobs
/var/lib/ttbox/motion-profiles
/var/lib/ttbox/presets
/var/log/ttbox
/opt/ttbox/config
/opt/ttbox/config/hardware_display.json
/opt/ttbox/config/default.json
/opt/ttbox/config/motion-profiles
/opt/ttbox/presets
/var/lib/ttbox/license
/var/lib/ttbox/ota/processed
/opt/ttbox/current
/opt/ttbox/plugins
/opt/ttbox/scripts
EOF
chk "ttbox 不在 sudo 组"          '! getent group sudo | grep -qw ttbox'
chk "ttbox shell 为 nologin"      '[ "$(getent passwd ttbox | cut -d: -f7)" = /usr/sbin/nologin ]'
chk "ttbox 在 video 组"           'id -nG ttbox | tr " " "\n" | grep -qx video'
# world-writable 断言必须排除软链：Linux 软链自身的 mode 恒为 0777（chmod 对软链是 no-op），
# 拿它当"可写风险"是显示假象；真正决定安全的是**目标**的权限，已由上面的目录属主表覆盖。
# 不排软链会把 /opt/ttbox/{current,plugins,scripts} 三条 755 目标的过渡软链误判成失败。
chk "整棵 /opt/ttbox 无 world-writable（实体文件/目录）" \
    'test -z "$(find /opt/ttbox \( -type f -o -type d \) -perm -o+w -print -quit)"'

SEC "F2. 开箱可用性（激活凭据 / 可写目录）——独立重读，与 04b 的写入不复用"
# ★ 这一节是补上来的：第一轮自检全绿却让镜像带着"客户无法激活"的致命缺陷出门，
#   因为当时只查了"产品树 + 服务 + 姿态"，没查"客户第一脚要走的路"。
#   判据来自产品源码而非我的推断：
#     · web_credentials_file() = /opt/ttbox/config/default.json（只取 cloud.* 段）
#     · cloud_client.py 明写 cloud.license_base_url / app_secret **无编译期缺省**
#     缺 ⇒ card_login fail-closed ⇒ 激活页输任何卡密都失败。
CRED=/opt/ttbox/config/default.json
if [ -f "$CRED" ]; then
    python3 - "$CRED" <<'PY'
import json, sys
d = json.load(open(sys.argv[1], encoding='utf-8'))
c = d.get('cloud') or {}
need = ('license_base_url', 'app_key', 'app_secret')
miss = [k for k in need if not c.get(k)]
print('  cloud 段: %s' % (', '.join('%s=%s' % (k, (c[k] if k != 'app_secret' else '%s…%s' % (c[k][:4], c[k][-4:]))) for k in need if c.get(k)) or '(空)'))
sys.exit(1 if miss or not str(c.get('license_base_url','')).startswith('https://') else 0)
PY
    [ $? -eq 0 ] || { echo "  [✗] 云端凭据缺失/非法 ⇒ 客户激活必失败"; FAIL=1; }
else
    echo "  [✗] $CRED 不存在 ⇒ 客户激活必失败（card_login 无编译期缺省）"; FAIL=1
fi
chk "云端凭据权限 = root:ttbox 0640"  '[ "$(stat -c %U:%G:%a /opt/ttbox/config/default.json 2>/dev/null)" = "root:ttbox:640" ]'
chk "云端凭据世界不可读（secret 不外泄）" 'test -z "$(find /opt/ttbox/config/default.json -perm -o+r -print 2>/dev/null)"'
# 真以 ttbox 身份写一次：只 stat 权限位是猜，实际写成功才是证据
chk "ttbox 身份可读云端凭据（web 要读）" \
    'su -s /bin/sh ttbox -c "test -r /opt/ttbox/config/default.json"'
chk "ttbox 身份可写 /opt/ttbox/presets（面板保存预设）" \
    'su -s /bin/sh ttbox -c "touch /opt/ttbox/presets/.wt && rm -f /opt/ttbox/presets/.wt"'
chk "ttbox 身份可写 /opt/ttbox/config/motion-profiles" \
    'su -s /bin/sh ttbox -c "touch /opt/ttbox/config/motion-profiles/.wt && rm -f /opt/ttbox/config/motion-profiles/.wt"'
chk "/opt/ttbox 未被放开成 world-writable" '[ "$(stat -c %a /opt/ttbox)" != "777" ]'
echo "  -- 与开发板口径的差异（有意收紧，非缺陷）--"
echo "     开发板 /opt/ttbox = 777（开发检出遗留）；出厂 = $(stat -c %a /opt/ttbox) root:root"

SEC "G. systemd 单元与 enable 态"
for u in ttbox-core.service ttbox-web.service ttbox-preview.service ttbox-usbproxy.service \
         ttbox-edid.service ttbox-ota.path ttbox-ota.service ttbox-ensure.service ttbox-ensure.timer; do
    if [ -f "/etc/systemd/system/$u" ]; then printf '  [✓] %s 已安装\n' "$u"; else printf '  [✗] %s 缺失\n' "$u"; FAIL=1; fi
done
echo "  -- enable 落链 --"
ls -l /etc/systemd/system/multi-user.target.wants/ /etc/systemd/system/timers.target.wants/ 2>/dev/null \
    | grep -E 'ttbox|:$' | sed 's/^/    /'
for u in ttbox-core.service ttbox-web.service ttbox-preview.service ttbox-usbproxy.service \
         ttbox-edid.service ttbox-ota.path; do
    chk "$u 已 enable 且可解析" "test -e /etc/systemd/system/multi-user.target.wants/$u"
done
chk "ttbox-ensure.timer 已 enable 且可解析" 'test -e /etc/systemd/system/timers.target.wants/ttbox-ensure.timer'

SEC "H. 交付姿态（SSH 已启用【V5 起】/ 出厂固化 / 可维护）"
# ★ 与 04_board_config.sh §7 同一判据：看"实际生效值"，不看"文件在不在"。
chk "ssh.service 已 enable（开机落链在位）" 'test -L /etc/systemd/system/multi-user.target.wants/ssh.service'
chk "ssh.service 未被 mask"      'test ! -e /etc/systemd/system/ssh.service || test "$(readlink /etc/systemd/system/ssh.service)" != /dev/null'
chk "ssh.socket 未被 mask"       'test ! -e /etc/systemd/system/ssh.socket || test "$(readlink /etc/systemd/system/ssh.socket)" != /dev/null'
chk "cloud-init 已禁用"          'test -f /etc/cloud/cloud-init.disabled'
chk "netplan 已固化"             'test -f /etc/netplan/01-ttbox.yaml'
chk "root 可控制台/远程登录"      'awk -F: "\$1==\"root\" && \$2 ~ /^\\\$/" /etc/shadow | grep -q .'
chk "machine-id 已清空"          'test ! -s /etc/machine-id'
chk "SSH host key 在位（ed25519+rsa）" 'test -s /etc/ssh/ssh_host_ed25519_key && test -s /etc/ssh/ssh_host_rsa_key'
chk "SSH host key 权限 600"      'test "$(stat -c %a /etc/ssh/ssh_host_ed25519_key)" = 600'
# 注意：本步在 /root/_bake 内执行，_bake 此刻必然存在 ⇒ 这里**不做断言**（旧写法
# 'test ! -e /root/_bake || echo ...' 恒为 0，是个永远不会失败的空断言）。真正的
# "交付前必须无暂存" 门禁放在宿主侧 90_finalize_host.sh（那里才查得准）。
echo "  构建暂存 /root/_bake: $([ -e /root/_bake ] && echo '在场（由 90_finalize_host.sh 交付前清除并复验）' || echo '不在场')"
chk "journal 持久化已开"         'test -d /var/log/journal'
chk "extlinux.conf 未被改动（与出厂同 sha256）" \
    '[ "$(sha256sum /boot/extlinux/extlinux.conf | cut -d" " -f1)" = 3fab043163523909f5093ab505a1e4ef0106a0e9f6647aac6d9759417d58defc ]'
chk "镜像内无 TTBOX 私钥"        'test -z "$(find / -xdev \( -name "*.priv.pem" -o -name ".testkeys" \) 2>/dev/null)"'

SEC "H2. SSH 专项（生效值判定 · V5 新增）"
# 判据分三层：S1–S3 系统层（落链/未 mask）；S4–S7 看 sshd 自己解析出的【生效值】；
# S8 host key 在位；S9 root 口令哈希为强哈希。口诀：文件在 ≠ 生效。
# 教训实例：旧 99-ttbox-service.conf 就是"文件在、但被 60-cloudimg-settings.conf 抢先"的假绿。
export PATH="/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin:$PATH"
mkdir -p /run/sshd
SSHD_T="$(sshd -T 2>/dev/null || true)"
sshd_eff() { printf '%s\n' "$SSHD_T" | awk -v k="$1" '$1==k{print $2; exit}'; }
chk "S1 ssh.service 未指向 /dev/null" \
    'test "$(readlink /etc/systemd/system/ssh.service 2>/dev/null)" != /dev/null'
chk "S2 SSH 开机自启（enable 落链在位）" 'test -L /etc/systemd/system/multi-user.target.wants/ssh.service'
# 注：chroot 内 `systemctl is-enabled` 会打印 "Running in chroot, ignoring request" 且返回码不可靠
#     （本项目实测过 systemctl 在 chroot 假成功）⇒ S2 用落链判定，与 enable 语义等价且确定性强。
chk "S3 落链目标可解析（非断链）" '[ -e "$(readlink -f /etc/systemd/system/multi-user.target.wants/ssh.service)" ]'
chk "S4 sshd 配置语法合法（sshd -t）" 'sshd -t >/dev/null 2>&1'
chk "S5 生效 permitrootlogin = yes"        '[ "$(sshd_eff permitrootlogin)" = yes ]'
chk "S6 生效 passwordauthentication = yes" '[ "$(sshd_eff passwordauthentication)" = yes ]'
chk "S7 生效 port = 22"                    '[ "$(sshd_eff port)" = 22 ]'
chk "S8 host key 非空（ed25519+rsa）"      'test -s /etc/ssh/ssh_host_ed25519_key && test -s /etc/ssh/ssh_host_rsa_key'
PWHASH="$(awk -F: '$1=="root"{print $2}' /etc/shadow)"
case "$PWHASH" in
    \$y\$*|\$6\$*|\$5\$*) printf '  [✓] S9 root 口令为强 crypt 哈希（算法域 %s）\n' "$(printf '%s' "$PWHASH" | cut -d'$' -f2)" ;;
    *) printf '  [✗] S9 root 口令哈希异常：%s\n' "${PWHASH:-<空>}"; FAIL=1 ;;
esac
echo "  -- 补充：ufw 应 inactive（若启用必须放行 22，否则 S7 是假绿）--"
ufw status 2>/dev/null || echo "  (未安装 ufw ⇒ 无防火墙拦截)"
echo "  -- 以上 9 条全部基于【生效值】/落链，不是文件在不在 --"

SEC "I. 硬件前置（静态可判部分）"
echo "  内核 HDMI RX     : $(grep -h 'CONFIG_VIDEO_ROCKCHIP_HDMIRX=y' /boot/config-* | head -1)"
echo "  内核 USB HID 设备 : $(grep -h 'CONFIG_USB_CONFIGFS_F_HID=y' /boot/config-* | head -1)"
# modinfo 在 chroot 内拿不到（模块不会加载，uname -r 又是宿主内核）⇒ 直接找 .ko 落盘证据
RAWKO="$(ls /lib/modules/*/kernel/drivers/usb/gadget/legacy/raw_gadget.ko* 2>/dev/null | head -1)"
echo "  raw_gadget 模块   : ${RAWKO:-未找到（modinfo: $(modinfo -n raw_gadget 2>/dev/null || echo n/a)）}"
echo "  hdmirx overlay    : $(ls /usr/lib/firmware/*/device-tree/rockchip/overlay/rk3588-hdmirx.dtbo 2>/dev/null || echo '未找到')"
echo "  （/dev/video0、NPU、USB gadget 属真实硬件，chroot 内不可判 —— 须上板验）"

# ★ 2026-09-20 现场事故补的断言：出厂 DTB 里 hdmirx-controller@fdee0000 是
#   status="disabled"，而 extlinux.conf 根本没有 fdtoverlays 指令 ⇒ dtbo 永远不会被应用
#   ⇒ HDMI-RX 不 probe ⇒ /sys/class/hdmirx 与 /dev/video0 都不存在 ⇒
#   ①EDID 应用报「未找到可写 HDMI-RX HPD 节点」②HDMI 采集整体不可用。
#   旧自检只断言了"dtbo 文件在不在""boot 配置含 overlay 字样"——**文件在 ≠ 生效**，
#   这就是那次假绿的来源。故改为直接断言设备树里 hdmirx 是否被启用。
DTB="$(ls /lib/firmware/*/device-tree/rockchip/rk3588-orangepi-5-plus.dtb 2>/dev/null | head -1)"
if [ -z "$DTB" ]; then
    echo "  [✗] 找不到 rk3588-orangepi-5-plus.dtb，无法判 hdmirx 启用状态"; FAIL=1
else
    DTB_SHA="$(sha256sum "$DTB" | cut -d' ' -f1)"
    DTB_DIS="$(grep -ao disabled "$DTB" | wc -l)"
    echo "  DTB              : $DTB"
    echo "    sha256=$DTB_SHA  disabled 计数=$DTB_DIS"
    # 已验证可用（开发板同型号同内核）的基线：sha256 277d9de8… / disabled 191
    # 出厂原版是 7b8cc892… / disabled 192（hdmirx 那一处为 disabled）
    chk "设备树已启用 HDMI-RX（hdmirx status=okay）" \
        'test "$DTB_SHA" = "277d9de87876a4e6160ae7ac048d4adadec73bbaa7706f39e2b5a5fe42379980" || test "$DTB_DIS" = "191"'
    echo "    对照：出厂未改 = 7b8cc892… / 192；改过 = 277d9de8… / 191（差的那 1 处就是 hdmirx）"
fi

SEC "J. 现场诊断器 ttbox_doctor.sh（部分项需 PID1，chroot 内 N/A）"
# ★ 坑：chroot 内 systemctl **不会失败、而是假成功** —— 实测 `is-active` 打印
#   "Running in chroot, ignoring command 'is-active'" 且 **返回码 0**，连不存在的单元也报 PASS。
#   所以 doctor 的 5 个服务项 + ensure.timer 项在本环境一律是假绿（不是假红）。如实标注。
TTBOX_CURRENT=/opt/ttbox/current bash "$BAKE/scripts/ttbox_doctor.sh" 2>&1 \
  | sed 's/^/  /' \
  | sed -E 's/\[doctor\]\[PASS\] ([a-zA-Z0-9_.@-]+\.(service|timer)) active.*/[doctor][N\/A ] \1 —— chroot 内 systemctl 假成功，真实存活只能上板验（静态 enable 态见 G 段）/' \
  | sed -E 's/\[doctor\]\[FAIL\] \/dev\/video0 不存在.*/[doctor][N\/A ] \/dev\/video0 —— 需真实硬件（chroot 无设备节点）/' \
  | sed -E 's/\[doctor\]\[PASS\] boot 配置含 overlay 配置.*/[doctor][ OK ] boot 配置含 overlay 配置（静态可判）—— 运行时是否生效须上板看 \/dev\/video0/'
echo "  >>> 被标 [N/A ] 的项 = chroot 环境判不了（不是失败，也不是真通过）"
echo "  >>> doctor 退出码在 chroot 内会被上面的假绿污染，故本段不参与 FAIL 汇总"
echo "  >>> 真正的服务存活/HDMI RX/NPU 验证 = 烧录后上板（见交付文档）"

SEC "结论"
if [ "$FAIL" = "0" ]; then
    echo "== 阶段三：镜像级自检全部通过 =="
else
    echo "== 阶段三：存在失败项（见上方 [✗]）=="
fi
exit $FAIL
