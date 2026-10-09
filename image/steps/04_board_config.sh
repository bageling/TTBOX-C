#!/usr/bin/env bash
# 步骤 04 · 板级配置与客户交付姿态
#
# 业主决定（2026-09-21 · V5）：镜像内【开启】SSH，供售后远程排障（V4 及以前是关闭的）。
# TTBOX 的提权模型（已核）不依赖 sudo：core/ota/usbproxy 三个单元以 root 运行，
# web/preview 以 ttbox 运行靠组 ttbox 连 core.sock。故"最高权限"由 systemd 单元承载，
# 不需要给 ttbox 任何 shell 或 sudo 面。
set -uo pipefail
umask 022
FAIL=0
chk() { if eval "$2"; then printf '  [✓] %s\n' "$1"; else printf '  [✗] %s\n' "$1"; FAIL=1; fi; }

echo "########## 0. 环境事实（先核，再改）##########"
echo "  extlinux.conf sha256 : $(sha256sum /boot/extlinux/extlinux.conf | cut -d' ' -f1)"
echo "  HDMI RX 能力         : $(grep -c 'CONFIG_VIDEO_ROCKCHIP_HDMIRX=y' /boot/config-* 2>/dev/null) 处 =y"
echo "  hdmirx dtbo 在位     : $([ -f /usr/lib/firmware/5.10.0-1012-rockchip/device-tree/rockchip/overlay/rk3588-hdmirx.dtbo ] && echo yes || echo NO)"
echo "  → 厂商 DTB 已启用 hdmirx（板端同 extlinux.conf 下 /dev/video0 的 ID_V4L_PRODUCT=rk_hdmirx）"
echo "  → 结论：**不改 extlinux.conf**（少一处无必要的引导改动）"

echo
echo "########## 1. 启用 SSH 登录面（V5 起；V4 及以前是关闭的）##########"
# 业主决定（2026-09-21）：V5 起镜像内开 SSH，由售后远程排障使用。
#   · 去掉 V4 遗留的 mask 占位（/dev/null 软链），改为正常 enable ssh.service
#   · 只 enable ssh.service —— 同时 enable ssh.socket 会与之争抢 :22
#     （Ubuntu 官方默认也只 enable ssh.service；socket 保持未 enable、未 mask）
#   · root 口令登录要生效，需 PermitRootLogin/PasswordAuthentication 都为 yes，
#     ★ 且必须放在【排序早于 60-cloudimg-settings.conf】的文件里：
#       sshd 对同一参数取【首个出现的值】，而 60-cloudimg-settings.conf 写的是
#       PasswordAuthentication no ⇒ 若放 99- 会被它抢先，"看着开了"其实根本登不进来。
#       实测（chroot 内 sshd -T）：文件叫 99- 时 passwordauthentication=no；叫 00- 时 =yes。
mkdir -p /etc/systemd/system /etc/ssh/sshd_config.d
rm -f /etc/systemd/system/ssh.service /etc/systemd/system/ssh.socket    # 去 mask 占位
rm -f /etc/ssh/sshd_config.d/99-ttbox-service.conf                      # 旧名（排序在后，实测无效）
cat > /etc/ssh/sshd_config.d/00-ttbox-service.conf <<'EOF'
# TTBOX 出厂镜像（V5+）：SSH 供售后远程排障，出厂即生效。
# ★ 文件名必须以 00- 开头。sshd 对同一参数取「首个出现的值」，
#   若排在 60-cloudimg-settings.conf（PasswordAuthentication no）之后则不生效。
PermitRootLogin yes
PasswordAuthentication yes
EOF
echo "  [+] /etc/ssh/sshd_config.d/00-ttbox-service.conf（出厂即生效）"
mkdir -p /etc/systemd/system/multi-user.target.wants
ln -sfn /lib/systemd/system/ssh.service /etc/systemd/system/multi-user.target.wants/ssh.service
echo "  [enable] multi-user.target.wants/ssh.service -> /lib/systemd/system/ssh.service"
chk "ssh.service 已 enable（开机落链在位）" 'test -L /etc/systemd/system/multi-user.target.wants/ssh.service'
chk "ssh.service 未被 mask"  'test ! -e /etc/systemd/system/ssh.service || test "$(readlink /etc/systemd/system/ssh.service)" != /dev/null'
chk "ssh.socket 未被 mask"   'test ! -e /etc/systemd/system/ssh.socket || test "$(readlink /etc/systemd/system/ssh.socket)" != /dev/null'

echo
echo "########## 2. 关闭 cloud-init（改出厂固化，去首启不确定性）##########"
# 理由：cloud-init 首启会建 ubuntu/ubuntu 账号并把 ssh_pwauth 打开（引入一个多余的
#       ubuntu 账号，且首启行为不可复现）。改为把网络与账号全部固化进镜像。
touch /etc/cloud/cloud-init.disabled
echo "  [+] /etc/cloud/cloud-init.disabled"
chk "cloud-init.disabled 已就位" 'test -f /etc/cloud/cloud-init.disabled'

echo
echo "########## 3. 固化 netplan（cloud-init 不再生成，必须自带）##########"
# 内容逐字复刻板端由 cloud-init 生成并已实网验证的 /etc/netplan/50-cloud-init.yaml。
# 不指定 renderer：与板端一致（该镜像未装 NetworkManager，netplan 默认走 systemd-networkd）。
cat > /etc/netplan/01-ttbox.yaml <<'EOF'
# TTBOX 出厂镜像固化网络配置（不再由 cloud-init 生成）。
# 复刻自开发板 /etc/netplan/50-cloud-init.yaml（该配置已在真机验证可拿到 DHCP）。
# 匹配 en*/eth* 全部有线网口，dhcp4；optional: true 令无网线时也不阻塞启动。
network:
    ethernets:
        zz-all-en:
            dhcp4: true
            match:
                name: en*
            optional: true
        zz-all-eth:
            dhcp4: true
            match:
                name: eth*
            optional: true
    version: 2
EOF
chmod 0600 /etc/netplan/01-ttbox.yaml
echo "  [+] /etc/netplan/01-ttbox.yaml (0600)"
echo "  -- netplan generate 校验 --"
# 注意：不能用 `if netplan generate | sed ...; then` —— 管道退出码取的是 sed 的，
# 恒为 0，会把配置错误吃掉。必须先取 rc 再落屏。
NP_OUT="$(netplan generate 2>&1)"; NP_RC=$?
if [ -n "$NP_OUT" ]; then printf '%s\n' "$NP_OUT" | sed 's/^/     /'; fi
if [ "$NP_RC" -eq 0 ]; then echo "     [✓] netplan 配置可解析（rc=0）"; else echo "     [✗] netplan 配置有误（rc=$NP_RC）"; FAIL=1; fi
# 清理 cloud-init 遗留的空 netplan 目录项（本镜像 /etc/netplan 原本为空，无遗留）
chk "netplan 配置存在" 'test -f /etc/netplan/01-ttbox.yaml'

echo
echo "########## 4. 持久化 journal（现场排障必需）##########"
# 镜像默认 Storage=auto：仅当 /var/log/journal 存在才落盘。板端就是持久化的。
install -d -o root -g systemd-journal -m 2755 /var/log/journal
echo "  [+] /var/log/journal（2755 root:systemd-journal）"
chk "journal 目录就位" 'test -d /var/log/journal'
chk "journal 目录 mode 2755" 'test "$(stat -c %a /var/log/journal)" = 2755'

echo
echo "########## 5. 维护口令（root · 控制台与远程登录共用）##########"
# 镜像原状：root 无口令（连 shadow 哈希都没有）= 串口/显示器也登不进去。
# 关掉 cloud-init 后若不设口令，出问题时将【完全没有入口】，故必须设一个已知口令。
# V5 起 SSH 开启 ⇒ 该口令即远程登录凭据，**不再用 V4 的固定弱口令 'root'**。
# 口令由构建方经环境变量 TTBOX_ROOT_PASS 注入；明文只留项目外目录，不入镜像/文档。
ROOT_PASS="${TTBOX_ROOT_PASS:-}"
if [ -z "$ROOT_PASS" ]; then
    echo "  [✗] 未提供 TTBOX_ROOT_PASS —— V5 起禁止回落到固定弱口令，构建中止" >&2
    FAIL=1
else
    # 用 SHA512 而非发行版默认 yescrypt：**为了能在本机离线复算验证**
    # （chpasswd 支持的算法：NONE/DES/MD5/SHA256/SHA512；yescrypt 在宿主上无法复算）。
    # 20 位随机口令下 KDF 强度差异无实际影响，可验证性更重要。
    printf 'root:%s\n' "$ROOT_PASS" | chpasswd -c SHA512
    echo "  [+] root 口令已设置（来源 TTBOX_ROOT_PASS，长度 ${#ROOT_PASS}，SHA512）"
fi
chk "root 已有口令哈希" 'awk -F: "\$1==\"root\" && \$2 ~ /^\\\$/" /etc/shadow | grep -q .'
echo "  root 登录 shell: $(getent passwd root | cut -d: -f7)"
echo "  root 是否在 sudo 组体系内: 本就是 uid 0，无需 sudo"

echo
echo "########## 6. 出厂化清理（不把构建痕迹留在镜像里）##########"
# 6a. 构建暂存目录 /root/_bake —— **不在这里删**：本脚本此刻正从该目录执行，
#     chroot 内删除会让流式读取脚本的 bash 报 "cannot read"。
#     由宿主侧 image/90_finalize_host.sh 统一清除。
echo "  [i] /root/_bake 交由宿主侧 90_finalize_host.sh 清除（避免删掉正在执行的脚本自身）"
# 6b. apt 痕迹
apt-get clean >/dev/null 2>&1 || true
rm -rf /var/lib/apt/lists/* /var/cache/apt/archives/*.deb 2>/dev/null || true
echo "  [+] 已清 apt 缓存与索引"
# 6c. 构建期 shell 历史
rm -f /root/.bash_history /home/*/.bash_history 2>/dev/null || true
# 6d. machine-id 保持清空 —— systemd 首启生成唯一值（多台设备不得同 id）
: > /etc/machine-id
echo "  [+] /etc/machine-id 置空（首启由 systemd 生成唯一值）"
# 6e. 安装【固定】host key（V5 起 SSH 开启，出厂即带 key）
#     V4 及以前是"不留 host key"（SSH 被 mask，且避免多机同 key）；
#     V5 改为构建时固定一套，由 TTBOX_HOSTKEY_DIR 注入。
#     ★ 风险已知并接受：这一套 key 泄露即可冒充全部盒子（业主 2026-09-21 拍板）。
HK_DIR="${TTBOX_HOSTKEY_DIR:-}"
if [ -n "$HK_DIR" ] && [ -f "$HK_DIR/ssh_host_ed25519_key" ]; then
    install -d -m 0755 /etc/ssh
    rm -f /etc/ssh/ssh_host_*
    for k in ssh_host_ed25519_key ssh_host_rsa_key; do
        [ -f "$HK_DIR/$k" ] || continue
        install -m 0600 "$HK_DIR/$k" "/etc/ssh/$k"
        [ -f "$HK_DIR/$k.pub" ] && install -m 0644 "$HK_DIR/$k.pub" "/etc/ssh/$k.pub"
    done
    echo "  [+] 已安装固定 host key（源 $HK_DIR）"
else
    echo "  [✗] 未提供 TTBOX_HOSTKEY_DIR 或其中无 ssh_host_ed25519_key" >&2
    FAIL=1
fi
HK_ED_DST="$(sha256sum /etc/ssh/ssh_host_ed25519_key 2>/dev/null | cut -d' ' -f1)"
HK_ED_SRC="$(sha256sum "$HK_DIR/ssh_host_ed25519_key" 2>/dev/null | cut -d' ' -f1)"
echo "  [+] host key ed25519 sha256 = ${HK_ED_DST:-<无>}"
# 6f. 清日志（把本次构建产生的 journal/日志清掉，出厂从干净开始）
rm -rf /var/log/journal/* /var/log/ttbox/* 2>/dev/null || true
find /var/log -maxdepth 1 -type f -name '*.log' -delete 2>/dev/null || true
find /var/log -maxdepth 1 -type f -name '*.gz' -delete 2>/dev/null || true
echo "  [+] 已清日志"

echo
echo "########## 7. 交付姿态断言 ##########"
chk "SSH 已启用（ssh.service 开机落链在位）" 'test -L /etc/systemd/system/multi-user.target.wants/ssh.service'
chk "ssh.service 未被 mask"  'test ! -e /etc/systemd/system/ssh.service || test "$(readlink /etc/systemd/system/ssh.service)" != /dev/null'
chk "ssh.socket 未被 mask"   'test ! -e /etc/systemd/system/ssh.socket || test "$(readlink /etc/systemd/system/ssh.socket)" != /dev/null'
chk "cloud-init 已禁用"               'test -f /etc/cloud/cloud-init.disabled'
chk "netplan 已固化"                  'test -f /etc/netplan/01-ttbox.yaml'
chk "journal 持久化"                  'test -d /var/log/journal'
chk "root 可登录（有口令哈希）"        'awk -F: "\$1==\"root\" && \$2 ~ /^\\\$/" /etc/shadow | grep -q .'
chk "machine-id 已清空"               'test ! -s /etc/machine-id'
chk "SSH host key 在位（ed25519）"     'test -s /etc/ssh/ssh_host_ed25519_key'
chk "SSH host key 权限 600"            'test "$(stat -c %a /etc/ssh/ssh_host_ed25519_key)" = 600'
chk "host key 与注入源逐字节一致"       '[ -n "$HK_ED_DST" ] && [ "$HK_ED_DST" = "$HK_ED_SRC" ]'
# 注：/root/_bake 的存在性断言放在宿主侧 90_finalize_host.sh —— 本脚本执行时它必然还在。
# ★ SSH 生效值判定：不看"文件在不在"，看 sshd 的**实际生效值**（本项目级教训）
#   sshd -T 需要 /run/sshd 与 host key；chroot 内 /run 是 bind 的 tmpfs，缺目录则先建。
mkdir -p /run/sshd
export PATH="/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin:$PATH"
ssh_chk() {
    local want="$1" key="$2" got
    got="$(sshd -T 2>/dev/null | awk -v k="$key" '$1==k{print $2; exit}')"
    if [ "$got" = "$want" ]; then printf '  [✓] SSH 生效值 %s = %s\n' "$key" "$got"
    else printf '  [✗] SSH 生效值 %s = %s（期望 %s）\n' "$key" "${got:-<空>}" "$want"; FAIL=1; fi
}
if command -v sshd >/dev/null 2>&1; then
    ssh_chk yes permitrootlogin
    ssh_chk yes passwordauthentication
    ssh_chk 22  port
else
    echo "  [✗] 找不到 sshd，无法判 SSH 生效值"; FAIL=1
fi
chk "TTBOX 服务仍全部在 enable 清单"  'test -L /etc/systemd/system/multi-user.target.wants/ttbox-core.service'
chk "ttbox-ensure.timer 仍在 enable"  'test -L /etc/systemd/system/timers.target.wants/ttbox-ensure.timer'

echo
echo "########## 8. 磁盘余量 ##########"
df -h /

echo
if [ "$FAIL" = "0" ]; then echo "== 步骤 04 全部断言通过 =="; else echo "== 步骤 04 有断言失败 =="; fi
exit $FAIL
