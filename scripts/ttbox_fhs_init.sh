#!/usr/bin/env bash
# ttbox_fhs_init.sh — TTBOX FHS 目录结构初始化 + 运行树装货（全新部署，幂等可重跑）
#
# 背景（T02）：原部署把程序/配置/模型/日志全部堆在 /opt/ttbox，升级程序时客户模型
# 与调好的参数可能被一并冲掉。本脚本按 FHS 拆分五个"抽屉"：
#   /opt/ttbox        程序本体（只读，升级整体替换；releases/<ver> + current 软链）
#   /etc/ttbox        配置（升级保留；config.d 分层：00-factory ← 10-device）
#   /var/lib/ttbox    客户数据：模型/HID 包（升级绝不触碰）
#   /run/ttbox        运行时 socket（tmpfs，重启清空；systemd RuntimeDirectory 托管）
#   /var/log/ttbox    日志
#
# 说明：存量设备 = 0（仅一台开发板），因此不需要"10 步幂等迁移状态机"，
# 本脚本只做全新部署的目录初始化。所有步骤幂等，重跑无副作用。
#
# 用法（板端 root）：
#   cd <仓库>/scripts && ./ttbox_fhs_init.sh [<version>]
#   <version> 省略时默认 1.0.0（重复发布同版本幂等）。
#   可通过 TTBOX_BUILD_DIR 指定交叉编译产物目录（默认自动探测 build-aarch64 / build）。
set -euo pipefail
# 2026-09-17 板端实测订正：非登录 SSH（umask 077）下 mkdir/install -d 产出 0700 root 目录，
# User=ttbox 的服务 200/CHDIR。发布树必须全局可遍历——强制 022，不信任调用方 umask。
umask 022

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

# payload 临时目录（sync_tree 组装用）——用 EXIT trap 统一清理，避免函数 RETURN trap 误删。
PAYLOAD_DIR=""
cleanup_payload() {
    if [ -n "$PAYLOAD_DIR" ] && [ -d "$PAYLOAD_DIR" ]; then
        rm -rf -- "$PAYLOAD_DIR"
    fi
}
trap cleanup_payload EXIT

echo "== TTBOX FHS 初始化（幂等）=="
echo "仓库根: ${REPO_ROOT}"

# ---- 1. 系统用户/组（约定即本节；原记录于 ttbox_platform/supervisor/README.md，
#        该目录已随 V1.0.52「去 Python」批次 1 移除）----
if ! getent group ttbox >/dev/null; then
    groupadd --system ttbox
    echo "  [+] 创建系统组 ttbox"
else
    echo "  [=] 组 ttbox 已存在"
fi
if ! getent passwd ttbox >/dev/null; then
    useradd --system --gid ttbox --no-create-home --shell /usr/sbin/nologin ttbox
    echo "  [+] 创建系统用户 ttbox"
else
    echo "  [=] 用户 ttbox 已存在"
fi
# V-EDID-1（板端实测 2026-09-19）：web（User=ttbox）在「显示器与鼠标」页读 EDID 现状
# 走 hdmirx_edid.py --status → v4l2-ctl --get-edid；/dev/video0 是 root:video 0660，
# ttbox 不在 video 组 ⇒ open Permission denied ⇒ 面板显示「EDID 状态读取失败」。
# 幂等：已在组内则跳过。注意：组变更只影响**新进程**——存量设备跑完本脚本后需
# restart ttbox-web（fhs_init 开机自举时天然满足：web 尚未启动）。
if id -nG ttbox 2>/dev/null | tr ' ' '\n' | grep -qx video; then
    echo "  [=] 用户 ttbox 已在 video 组"
elif getent group video >/dev/null; then
    usermod -aG video ttbox
    echo "  [+] 用户 ttbox 加入 video 组（/dev/video0 root:video 0660 可读写）"
else
    echo "  [=] 系统无 video 组（无 V4L2 视频设备机型），跳过"
fi

# ---- 2. /etc/ttbox：配置（升级保留）----
# F9：/etc/ttbox 本体必须 root:ttbox 0775（组可写）——web（User=ttbox）的
# /setup（first-setup）用 _save_credentials 的原子写：在目录内新建
# web_credentials.json.tmp.<pid> 再 rename；0755 root:root 的话 ttbox 用户在
# **目录**上 EACCES ⇒ 全新板第一次 /setup 必 500。与下方 config.d 的 B2 是同一个坑。
install -d -o root -g ttbox -m 0775 /etc/ttbox
# 幂等补刀：install -d 对**已存在**目录不改属组/权限（本缺陷漏网正因如此）⇒
# 存量目录（旧版建的 root:root 0755）升级重跑 fhs_init 后也必须被纠正。
chgrp ttbox /etc/ttbox 2>/dev/null || true
chmod 0775 /etc/ttbox
# B2：config.d 必须 0775（组可写）——Core 以 ttbox 组运行，persist 的原子写
# 要在目录内新建 10-device.json.tmp 再 rename；0755 的话 ttbox 用户 EACCES，
# 10-device.json 本身 0664（组 rw）救不了 tmp+rename 模式。
install -d -o root -g ttbox -m 0775 /etc/ttbox/config.d
# 出厂基线：root:root 0644，随发布包替换（已存在则覆盖为新基线）
install -o root -g root -m 0644 \
    "${REPO_ROOT}/deploy/config/00-factory.json" \
    /etc/ttbox/config.d/00-factory.json
echo "  [+] /etc/ttbox/config.d/00-factory.json（出厂基线，只读）"
# 设备层：root:ttbox 0664，升级绝不覆盖（存在则跳过）
if [ ! -f /etc/ttbox/config.d/10-device.json ]; then
    install -o root -g ttbox -m 0664 \
        "${REPO_ROOT}/deploy/config/10-device.json" \
        /etc/ttbox/config.d/10-device.json
    echo "  [+] /etc/ttbox/config.d/10-device.json（设备层，客户可改）"
else
    echo "  [=] 10-device.json 已存在，保留（升级绝不覆盖客户配置）"
fi

# ---- 3. /var/lib/ttbox：客户数据（升级绝不触碰）----
# 模型仓库根（ModelRegistry 会自行建 registry/installed/staging/... 子目录）
install -d -o ttbox -g ttbox -m 0775 /var/lib/ttbox/models
# V-04 幂等补刀：子目录若由旧布局/ROOT 创建，可能是 root:root 755，导致 web(ttbox)
# 写入 _incoming 报 EACCES。这里显式创建并修正属主/权限，确保升级设备也生效。
install -d -o ttbox -g ttbox -m 0775 /var/lib/ttbox/models/installed \
    /var/lib/ttbox/models/staging /var/lib/ttbox/models/registry \
    /var/lib/ttbox/models/cache /var/lib/ttbox/models/quarantine \
    /var/lib/ttbox/models/_incoming 2>/dev/null || true
find /var/lib/ttbox/models -maxdepth 3 -type d -exec chown ttbox:ttbox {} + 2>/dev/null || true
find /var/lib/ttbox/models -maxdepth 3 -type d -exec chmod 0775 {} + 2>/dev/null || true
# HID 包仓库根（HidPackageRegistry 同上）
install -d -o ttbox -g ttbox -m 0775 /var/lib/ttbox/hid
# 授权凭据区（T04 使用，0700）
install -d -o ttbox -g ttbox -m 0700 /var/lib/ttbox/activation
# 动作曲线 / 预设（现状在 /opt/ttbox 下，按设计迁至 /var/lib）
install -d -o ttbox -g ttbox -m 0775 /var/lib/ttbox/motion-profiles
install -d -o ttbox -g ttbox -m 0775 /var/lib/ttbox/presets
# OTA 工作区（T03 使用，root:ttbox 0750）
install -d -o root -g ttbox -m 0750 /var/lib/ttbox/update
echo "  [+] /var/lib/ttbox/{models,hid,activation,motion-profiles,presets,update}"

# ---- 4. /run/ttbox：运行时 socket（tmpfs）----
# systemd RuntimeDirectory=ttbox 会在服务启动时自动创建；这里保证手工运行 Core
# 排障时目录也存在。属主 ttbox:ttbox，socket 权限 0660（仅同组可连）。
install -d -o ttbox -g ttbox -m 0750 /run/ttbox 2>/dev/null || \
    echo "  [!] /run/ttbox 创建失败（tmpfs 重启即清空，可忽略，由 systemd 托管）"
echo "  [+] /run/ttbox（core.sock 所在，0660 同组可连）"

# ---- 5. /var/log/ttbox：日志 ----
install -d -o ttbox -g ttbox -m 0755 /var/log/ttbox
echo "  [+] /var/log/ttbox"

# ===========================================================================
# 以下为 DEP-03（T1.02）/ DEP-01（T1.03）新增：装货进 release 树 + 幂等自愈
# ===========================================================================

TTBOX_PREFIX="${TTBOX_PREFIX:-/opt/ttbox}"
RELEASE_INSTALL="${REPO_ROOT}/scripts/ttbox_release_install.sh"
ENSURE_SCRIPT="${REPO_ROOT}/scripts/ttbox_ensure_services.sh"

# ---- 6. hardware_display.json 模板（"不存在才放"，绝不覆盖客户配置）----
# 消费者硬编码读取 /opt/ttbox/config/hardware_display.json：
#   scripts/edid/edid_apply.sh:9、scripts/edid/hdmirx_edid.py:124、scripts/edid/mode_builder.py:161、
#   plugins/web/bin/ttbox-web.py:3202/3335。
# 现状 fhs_init 从不创建 → EDID 注入一上来就报 "hardware_display.json 不存在"（静默降级）。
# 首次部署补齐模板；已存在（客户改过）则原样保留。
# D-A 修复（M2.07）：/opt/ttbox/config 本体必须 root:ttbox 0775（组可写）——web（User=ttbox）
# 的云端会话走原子写 plugins/web/lib/cloud_session.py：在该**目录**内新建
# cloud_session.json.tmp.<pid> 再 rename；若为 0755 root:root，ttbox 用户在目录上 EACCES
# ⇒ 云端验证成功后“会话落盘失败” ⇒ /api/license/activate 返回 500，core 的 ACTIVATE_CLOUD
# 根本不会被调用（B17 正是死在这里）。与 §2 的 F9（/etc/ttbox）同族缺陷。
install -d -o root -g ttbox -m 0775 "${TTBOX_PREFIX}/config"
# 幂等补刀：install -d 对**已存在**目录不改属组/权限（升级上来的机器目录早已存在，光改
# install -d 参数救不了存量设备）⇒ 显式纠正，照抄 §2 给 /etc/ttbox 的 F9 写法。
chgrp ttbox "${TTBOX_PREFIX}/config" 2>/dev/null || true
chmod 0775 "${TTBOX_PREFIX}/config"
# F10（M2.07.1 根治）：default.json 含 cloud.app_secret（敏感凭据），必须收敛为
# **root:ttbox 0640**（世界不可读，收敛泄露面；组可读 ⇒ User=ttbox 的 web 读得到）。
# 此前收紧曾回退，根因**不是权限而是写侧丢属组**：任何以 root 重写该文件的原子写
# （如验收脚本 B22 改 cloud.license_base_url）会把文件重建为 root:root ⇒ 0640 下 web
# EACCES ⇒ 云端凭据丢失（激活 502「云端凭据未配置」）。现写侧已保留属主/属组
# （ttbox_m207_accept.py::atomic_write_json 的 os.chown + 保 mode）⇒ 可安全收紧。
# 幂等补刀：install/install -d 对**已存在**文件/目录不改属主/权限 ⇒ 存量设备重跑
# fhs_init 也必须被纠正（照抄上方 §2 给 /etc/ttbox 的 F9 写法）。
if [ -f "${TTBOX_PREFIX}/config/default.json" ]; then
    chgrp ttbox "${TTBOX_PREFIX}/config/default.json" 2>/dev/null || true
    chmod 0640 "${TTBOX_PREFIX}/config/default.json"
    echo "  [+] ${TTBOX_PREFIX}/config/default.json 收敛为 root:ttbox 0640（幂等补刀）"
else
    echo "  [=] ${TTBOX_PREFIX}/config/default.json 尚未生成（运行时创建），跳过权限收敛"
fi
if [ ! -f "${TTBOX_PREFIX}/config/hardware_display.json" ]; then
    # 属主/权限 = root:ttbox 0664（与同层的 10-device.json 一致），**不是** root:root 0644。
    # 依据：web（User=ttbox / Group=ttbox）在 PUT /api/hardware/display 里是**原地重写**
    # （plugins/web/bin/ttbox-web.py:「json.dump(cur, open(cpath, 'w'))」），需要的是**文件**
    # 写权限；0644 root:root 下 ttbox 直接 EACCES ⇒ 显示器配置保存 500（板端实测 2026-09-19 P1-1）。
    install -o root -g ttbox -m 0664 \
        "${REPO_ROOT}/deploy/config/hardware_display.json" \
        "${TTBOX_PREFIX}/config/hardware_display.json"
    echo "  [+] ${TTBOX_PREFIX}/config/hardware_display.json（显示器身份模板，首次放置，root:ttbox 0664）"
else
    # 幂等补刀：install/install -d 对**已存在**文件不改属主/权限（本缺陷漏网正因如此）⇒
    # 旧版装成 root:root 0644 的存量设备重跑 fhs_init 也必须被纠正，否则升级后 500 依旧。
    # 只纠属组/权限，**内容一字不动**（绝不覆盖客户配置）。照抄上方给 default.json 的写法。
    chgrp ttbox "${TTBOX_PREFIX}/config/hardware_display.json" 2>/dev/null || true
    chmod 0664 "${TTBOX_PREFIX}/config/hardware_display.json"
    echo "  [=] hardware_display.json 已存在，内容保留；属组/权限收敛为 root:ttbox 0664（幂等补刀）"
fi

# V-EDID-2（板端实测 2026-09-19）：web「保存并应用」以 ttbox 身份直跑 edid_apply.sh——
# 应用后的 HPD rehandshake 要**写** /sys/class/hdmirx/hdmirx/status，builtin 组切换要
# **写**同目录 edid 节点；两节点内核默认 root:root 0644 ⇒ ttbox 写不进 ⇒ EDID 应用失败
# ⇒ 上位机源端永不重新枚举（用户实测「保存后没有重新枚举」）。sysfs 节点每次开机由
# 内核重建（权限归零），故 fhs_init 每次开机幂等收敛为 root:ttbox 0660。
# debugfs 的 /sys/kernel/debug/hdmirx/status 仅是显示增强（Actual RX），读不到已有
# 容错降级，**不放开**（debugfs 放权面太大）。
for _hx in /sys/class/hdmirx/hdmirx \
           /sys/devices/platform/fdee0000.hdmirx-controller/hdmirx/hdmirx; do
    if [ -e "$_hx/status" ] || [ -e "$_hx/edid" ]; then
        for _n in status edid; do
            if [ -e "$_hx/$_n" ]; then
                chown root:ttbox "$_hx/$_n" 2>/dev/null || true
                chmod 0660 "$_hx/$_n" 2>/dev/null || true
            fi
        done
        echo "  [+] $_hx/{status,edid} 收敛为 root:ttbox 0660（EDID 应用与 HPD 重握手）"
        break
    fi
done

# ---- 7. sync_tree：把运行树装进 releases/<ver>/ 并激活（DEP-03 / T1.02）----
# 精确定位交叉编译产物目录（显式指定优先，否则自动探测）。
# 读出二进制动态段的 RUNPATH 值（无段则空串）。返回 0。
# 说明：管道内 `|| true` 防 readelf 读到非 ELF 时非零；**不用 head**（避免 SIGPIPE 触发 set -o pipefail）。
read_runpath() {
    local bin="$1"
    { readelf -d "$bin" 2>/dev/null || true; } \
        | sed -n 's/.*(RUNPATH)[^[]*\[\(.*\)\].*/\1/p'
}

# 精确定位交叉编译产物目录。★ 判据 = **实测 RUNPATH 必须恰为形 A**（自包含，仅 `$ORIGIN/../lib`
# 一段），而非"目录名先到先得"——两个 build 目录都可能链到同一个 2.3.2、门禁照样全绿，但只有形 A
# 的产物能就近加载 payload 内 librknnrt.so：形 C（/opt/ttbox/lib）会去共享目录找、**静默绕过 T1.16**；
# 形 B（$ORIGIN/../lib:/opt/ttbox/lib）是"尾段跨版本共享目录"的假回滚。详见 T1.16 与
# ttbox_release_verify.sh 的逐段判据。候选顺序：显式 TTBOX_BUILD_DIR 优先；其次是固定名
# build-aarch64 / build-aarch64-t114；再兜住任意 build-aarch64-t*（按 mtime 新→旧，避免把
# 某个轮次编号写死——旧写法只认 t114，而该目录每轮构建都会被 rm -rf 重建、事后又被清理
# ⇒ 探针会静默退回 build/ 或直接找不到产物）。**stdout 只回显选中路径**；人类日志走 stderr。
detect_build_dir() {
    local d bin actual summary=""
    for d in "${TTBOX_BUILD_DIR:-}" build-aarch64 build-aarch64-t114 $(ls -1dt "${REPO_ROOT}"/build-aarch64-t* 2>/dev/null) build; do
        [ -n "$d" ] || continue
        case "$d" in
            /*) ;;
            *) d="${REPO_ROOT}/${d}" ;;
        esac
        bin="${d}/ttbox_core_main"
        if [ ! -x "$bin" ]; then
            # 显式指定但二进制缺失 = 验证失败（显式指定不许被静默跳过）
            if [ -n "${TTBOX_BUILD_DIR:-}" ]; then
                echo "  [✗] TTBOX_BUILD_DIR 指定的目录无 ttbox_core_main: ${bin}" >&2
                return 1
            fi
            continue
        fi
        if ! command -v readelf >/dev/null 2>&1; then
            echo "  [✗] 环境错误：找不到 readelf，无法实测 RUNPATH —— 拒绝按目录名猜测（请先装 binutils）。" >&2
            return 1
        fi
        actual="$(read_runpath "$bin")"
        actual="${actual%%$'\n'*}"
        if [ "$actual" = '$ORIGIN/../lib' ]; then
            echo "  [i] 编译产物目录: ${d}（RUNPATH=形 A 已验证）" >&2
            printf '%s' "$d"
            return 0
        fi
        echo "  [!] 候选 ${d} 的 RUNPATH 非形 A（实际: ${actual:-<无>}），不采纳" >&2
        summary="${summary}    ${d} → RUNPATH=${actual:-<无>}"$'\n'
        if [ -n "${TTBOX_BUILD_DIR:-}" ]; then
            echo "  [✗] TTBOX_BUILD_DIR 显式指定 ${d}，但其 RUNPATH 非形 A（实际: ${actual:-<无>}）——显式指定必须被验证，拒绝静默回退" >&2
            return 1
        fi
    done
    if [ -n "$summary" ]; then
        echo "  [✗] 未找到 RUNPATH = 形 A（\$ORIGIN/../lib）的交叉编译产物；候选实测：" >&2
        printf '%s' "$summary" >&2
    fi
    return 1
}

# tar 管道拷贝（不依赖 rsync；保留可执行位）。排除 __pycache__ / *.pyc / .git。
tcopy() {
    local src="$1" dst="$2"
    if [ ! -e "$src" ]; then
        echo "  [!] 源不存在，跳过: ${src}" >&2
        return 0
    fi
    mkdir -p -- "$dst"
    # E02（2026-09-18）：tests/ 不进出货包 —— 这是 payload 侧唯一口径，
    # install 侧 TAR_EXCLUDES 与此镜像（改口径两处一起改）。
    # P8（2026-10-01）补 .registry.json：插件注册表是**运行期状态**，由 PluginRegistry
    # 在 <plugins_root>/.registry.json 现场生成、内含绝对路径（实测泄漏过一条 dev 树
    # 路径进 payload）。剔除集三处同一口径：本文件 / ttbox_pack_ota.sh /
    # ttbox_release_install.sh::TAR_EXCLUDES。
    tar -C "$src" --exclude=__pycache__ --exclude='*.pyc' --exclude=.git --exclude=tests \
        --exclude=.registry.json -cf - . \
        | tar -C "$dst" -xf -
}

# ── 出货清单解析（双列 <源路径> -> <目标路径>）────────────────────────────────
# 语法与规则见 deploy/pack_manifest.txt 头注释。
# ★ 本套语义必须与 scripts/ttbox_pack_ota.sh 里的 parse_entry / map_target 完全一致
#   （装机树与 OTA 包读同一份清单，口径不一致 = 两者出货树不一样）。改一处必须改两处。
manifest_fail() { echo "  [✗] $*" >&2; exit 1; }

manifest_parse_entry() {   # $1 = 清单原始行；解析成全局 SRC / DST；返回 1 = 空行或注释
    local entry="$1" left right
    case "$entry" in ''|'#'*) return 1 ;; esac
    left="${entry%% -> *}"; right="${entry#* -> }"
    [ "$right" != "$entry" ] || manifest_fail "清单行缺 ' -> ' 分隔符：${entry}"
    case "$right" in *' -> '*) manifest_fail "清单行含多处 ' -> '：${entry}" ;; esac
    SRC="${left%"${left##*[![:space:]]}"}";  SRC="${SRC#"${SRC%%[![:space:]]*}"}"
    DST="${right%"${right##*[![:space:]]}"}"; DST="${DST#"${DST%%[![:space:]]*}"}"
    case "$SRC" in ''|*[[:space:]]*|/*|*..*) manifest_fail "清单源路径非法（空/含空白/绝对路径/含 ..）：${entry}" ;; esac
    case "$DST" in ''|*[[:space:]]*|/*|*..*) manifest_fail "清单目标路径非法（空/含空白/绝对路径/含 ..）：${entry}" ;; esac
    case "$SRC" in
        *'*'*) case "$DST" in */) ;; *) manifest_fail "源含通配符时目标必须是目录前缀（以 / 结尾）：${entry}" ;; esac ;;
        *)     case "$DST" in */) manifest_fail "精确源的落点不得以 / 结尾：${entry}" ;; esac ;;
    esac
    return 0
}

manifest_map_target() {   # $1 = 源的仓库根相对路径（已 glob 展开）；回显映射后的落点
    case "$DST" in */) printf '%s%s' "$DST" "$(basename "$1")" ;; *) printf '%s' "$DST" ;; esac
}

# 生成 RELEASE_MANIFEST.json（全量 sha256；T1.01 的 install 依赖它做完整性校验）。
gen_manifest() {
    local root="$1" ver="$2" gitsha
    # ★ F11②（2026-09-17）：git_sha 解析分层。板端源码副本常**无 .git**（由部署驱动 pscp
    #   推送，非 git clone），旧实现直接 `git rev-parse` ⇒ 恒回退成空串（QA 发现），
    #   使 RELEASE_MANIFEST.git_sha 失去"可复现锚第二半"的作用。现按序回退：
    #     ① 显式 env TTBOX_GIT_SHA —— 部署驱动从**构建机** `git rev-parse HEAD` 注入的唯一权威值；
    #     ② 仓库根 @ .git —— 本机构建/测试场景；
    #     ③ 仓库根 @ .git_sha 文件 —— 部署驱动随源码推送落盘的小文件（无 .git 时的兜底）。
    gitsha="${TTBOX_GIT_SHA:-}"
    if [ -z "${gitsha}" ]; then
        gitsha="$(git -C "${REPO_ROOT}" rev-parse HEAD 2>/dev/null || echo '')"
    fi
    if [ -z "${gitsha}" ] && [ -f "${REPO_ROOT}/.git_sha" ]; then
        gitsha="$(tr -d '[:space:]' < "${REPO_ROOT}/.git_sha" 2>/dev/null || echo '')"
    fi
    if [ -z "${gitsha}" ]; then
        echo "  [!] 警告：无法解析 git_sha（无 TTBOX_GIT_SHA / 无 .git / 无 .git_sha）—— manifest.git_sha 将为空" >&2
    fi
    python3 - "$root" "$ver" "$gitsha" <<'PY'
import datetime, hashlib, json, os, sys
root, ver, gitsha = sys.argv[1], sys.argv[2], sys.argv[3]
files = {}
for dp, dn, fn in os.walk(root):
    for f in fn:
        full = os.path.join(dp, f)
        rel = os.path.relpath(full, root)
        if rel == "RELEASE_MANIFEST.json":
            continue
        with open(full, "rb") as fh:
            files[rel] = hashlib.sha256(fh.read()).hexdigest()
doc = {
    "version": ver,
    "built_at": datetime.datetime.now(datetime.timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ"),
    "git_sha": gitsha,
    "files_sha256": files,
}
with open(os.path.join(root, "RELEASE_MANIFEST.json"), "w", encoding="utf-8") as fh:
    json.dump(doc, fh, indent=2, sort_keys=True)
print(f"[manifest] {len(files)} 个文件已登记 sha256")
PY
}

# ---- F1：librknnrt.so 出货源解析 + 同源硬门禁（lib-scope-ruling.md §6）----
# 铁律：payload 的 librknnrt.so 必须与【链接期所用那份】逐字节同源，否则拒绝出货。
# 作用域二分（§0/§2/§3）：
#   release 作用域     = librknnrt.so        → 随版本打进 releases/<ver>/lib/（本块负责）
#   基础镜像作用域     = librga/libjpeg/libopencv*/系统库 → 由板端 BSP 提供，一律不拷
RKNNRT_SHA_EXPECT="d31fc19c85b85f6091b2bd0f6af9d962d5264a4e410bfb536402ec92bac738e8"  # 2.3.2（链接期/镜像规格 PINNED）
RKNNRT_SHA_BANNED="9f53d7b1941338242e227fb097496261049c3aa1d8f2f343b27e439c325f6990"  # 1.5.2（仓库过期残留，禁止出货）
RKNNRT_VER_EXPECT="2.3.2"

# 解析出货用的 librknnrt.so（链接期同源）。成功回显路径、返回 0；否则返回 1。
# 来源优先级（唯一·乙案，甲案"统一到仓库副本"已驳回）：
#   ① TTBOX_RKNNRT_SO 显式覆盖（排障/指定构建机）
#   ② 构建目录 CMakeCache.txt 的 RKNNRT_LIBRARY（= 链接期真值，最权威）
#   ③ 失败即返回 1 —— **绝不回退仓库 lib/**（1.5.2 过期残留）
resolve_rknnrt_so() {
    local build_dir="$1"
    # ① 显式覆盖
    if [ -n "${TTBOX_RKNNRT_SO:-}" ]; then
        if [ -f "$TTBOX_RKNNRT_SO" ]; then
            printf '%s' "$TTBOX_RKNNRT_SO"
            return 0
        fi
        echo "  [!] TTBOX_RKNNRT_SO 指向的文件不存在: ${TTBOX_RKNNRT_SO}" >&2
        return 1
    fi
    # ② 链接期真值（build cache）
    local cache="${build_dir}/CMakeCache.txt" lib
    if [ -f "$cache" ]; then
        lib="$(sed -n 's/^RKNNRT_LIBRARY:[^=]*=//p' "$cache" | head -1)"
        if [ -n "$lib" ] && [ -f "$lib" ]; then
            printf '%s' "$lib"
            return 0
        fi
    fi
    # ③ 不回退仓库副本
    return 1
}

# librknnrt.so 同源硬门禁（§6 改动点 4）：任一不符 ⇒ 返回 1（调用方据此非零退出、绝不静默装货）：
#   - 命中 1.5.2 过期残留（9f53d7b1…）⇒ FAIL
#   - sha256 != d31fc19c…（2.3.2）    ⇒ FAIL
#   - 版本串不含 2.3.2               ⇒ FAIL
rknnrt_gate() {
    local f="$1" got ver
    if [ ! -f "$f" ]; then
        echo "  [✗] 门禁 FAIL：librknnrt.so 不存在（${f}）" >&2
        return 1
    fi
    got="$(sha256sum -- "$f" | awk '{print $1}')"
    if [ "$got" = "$RKNNRT_SHA_BANNED" ]; then
        echo "  [✗] 门禁 FAIL：librknnrt.so = 仓库 1.5.2 过期残留（sha256 ${got}）——禁止出货" >&2
        return 1
    fi
    if [ "$got" != "$RKNNRT_SHA_EXPECT" ]; then
        echo "  [✗] 门禁 FAIL：librknnrt.so sha256 不符。期望 ${RKNNRT_SHA_EXPECT}（2.3.2），实得 ${got}" >&2
        return 1
    fi
    ver="$(strings -a -- "$f" 2>/dev/null | grep -i 'librknnrt version' | head -1)"
    case "$ver" in
        *"$RKNNRT_VER_EXPECT"*) : ;;
        *)
            echo "  [✗] 门禁 FAIL：librknnrt.so 版本串不含 ${RKNNRT_VER_EXPECT}（实得: ${ver:-<空>}）" >&2
            return 1 ;;
    esac
    echo "  [✓] librknnrt.so 同源门禁通过：2.3.2 / ${got:0:12}… / ${ver}"
    return 0
}

# 组装 payload（design §B2.1 的白名单闭集，表外一律不进 release 树）。
sync_tree() {
    local ver="$1"
    local build_dir payload
    if ! build_dir="$(detect_build_dir)"; then
        echo "  [!] 未找到 RUNPATH=形 A 的交叉编译产物（ttbox_core_main）；请先交叉编译或设 TTBOX_BUILD_DIR" >&2
        echo "      （发布已跳过：没有合格 core 二进制就不该浇筑 current）" >&2
        return 1
    fi
    # 编译产物目录 + "RUNPATH=形 A 已验证" 由 detect_build_dir 打到 stderr（该函数 stdout 专供返回路径）。

    payload="$(mktemp -d "${TMPDIR:-/tmp}/ttbox-payload-XXXXXX")"
    PAYLOAD_DIR="$payload"   # 交由 EXIT trap 清理
    chmod 0755 -- "$payload"  # 2026-09-17 订正：mktemp 是 0700，tar 会把 "." 模式带给 release 根 ⇒ 全局可遍历

    install -d "${payload}/bin" "${payload}/lib" "${payload}/plugins" \
               "${payload}/scripts" "${payload}/deploy/systemd" "${payload}/deploy/config"

    # bin/：M1 payload bin/ 为【白名单闭集】= { ttbox_core_main, ttbox_web, ttbox_edid, ttbox_ota }
    #   （禁通配，防 .bak-* 混入）。V1.0.47 加 ttbox_web；V1.0.53 加 ttbox_edid
    #   （EDID 应用入口由 scripts/edid/edid_apply.sh 换成 C++ 二进制）。
    install -m 0755 "${build_dir}/ttbox_core_main" "${payload}/bin/ttbox_core_main"
    for _b in ttbox_web ttbox_edid ttbox_ota; do
        if [ -f "${build_dir}/${_b}" ]; then
            install -m 0755 "${build_dir}/${_b}" "${payload}/bin/${_b}"
        else
            echo "  [✗] 缺交叉编译产物 ${_b}（V1.0.53 起必须随包）——中止。" >&2
            exit 1
        fi
    done

    # lib/：仅 release 作用域库 librknnrt.so（F1 / lib-scope-ruling.md §6）
    #   ★ 铁律：必须与【链接期所用那份】逐字节同源 —— 来源 = TTBOX_RKNNRT_SO（可覆盖）
    #     > 构建缓存 RKNNRT_LIBRARY；**禁止**拷仓库 lib/（那份是 1.5.2 过期残留）。
    #   ★ librga / libjpeg / libopencv* 属基础镜像作用域，由板端 BSP 提供，一律不拷。
    local rknnrt_so
    if ! rknnrt_so="$(resolve_rknnrt_so "$build_dir")"; then
        echo "  [✗] 无法解析链接期 librknnrt.so（${build_dir}/CMakeCache.txt 无 RKNNRT_LIBRARY，" >&2
        echo "      且未设 TTBOX_RKNNRT_SO）。拒绝出货仓库 lib/ 副本（1.5.2 过期残留）——中止。" >&2
        exit 1
    fi
    echo "  [i] librknnrt 出货源（链接期同源）: ${rknnrt_so}"
    install -m 0644 "$rknnrt_so" "${payload}/lib/librknnrt.so"
    rknnrt_gate "${payload}/lib/librknnrt.so" || {
        echo "  [✗] librknnrt.so 同源门禁未通过 —— 中止浇筑（绝不静默装货）。" >&2
        exit 1
    }

    # ---- 白名单单一真源：deploy/pack_manifest.txt（双列：源路径 → 目标路径）----
    # sync_tree（首次装机）与 scripts/ttbox_pack_ota.sh（OTA 打包）读同一份清单、
    # 用同一套语义（本文件 manifest_parse_entry / manifest_map_target ↔ 那边
    # parse_entry / map_target）—— 改清单一处、装机树与出货包一起变。
    # 历史：此前装机走本函数硬编码清单、而 OTA 包是手工整树 tar，两套口径无机制保证
    # 一致 ⇒ 1.4.7 出货树 368 文件混入 docs(123)/tools(8)/platform(25)/modules(10)。
    # 条目语义见清单头注释：源=目录 → tcopy 整树（允许改名落点）；
    #   源=文件/glob → install（.sh/.py=0755，余 0644），glob 落到「目标目录前缀 + 文件名」。
    # bin/ 与 lib/ 属构建产物，不进清单（上方已按闭集规则安装）。
    local manifest="${REPO_ROOT}/deploy/pack_manifest.txt"
    if [ ! -f "$manifest" ]; then
        echo "  [✗] 缺打包白名单 ${manifest} —— 中止" >&2
        exit 1
    fi
    local entry src rel tgt mode matched
    while IFS= read -r entry <&3; do
        manifest_parse_entry "$entry" || continue
        matched=0
        # ★ $SRC 必须裸奔（不加引号）glob 才会展开；REPO_ROOT 引号保留防分词
        for src in "${REPO_ROOT}"/$SRC; do
            [ -e "$src" ] || break   # glob 未匹配 → 原样字面量 → 视为条目失效
            matched=1
            rel="${src#${REPO_ROOT}/}"
            tgt="$(manifest_map_target "$rel")"
            if [ -d "$src" ]; then
                tcopy "$src" "${payload}/${tgt}"
            else
                mode=0644
                case "$src" in *.sh|*.py) mode=0755 ;; esac
                install -D -m "$mode" "$src" "${payload}/${tgt}"
            fi
        done
        [ "$matched" = 1 ] || manifest_fail "清单源路径不存在（或 glob 展开为空）：${entry}"
    done 3< "$manifest"

    # ---- 清单后【只做权限修正】，不做任何增删文件（P1-2026-10-01：取消交付减法结构）----
    # 原先这里剪掉 plugins/web/{api_v1.py,framework_api.py,static/legacy} 三条路径，
    # 那属于「出货减法」结构（S1-2026-09-18 / A0-3c·A0-3d）—— 该结构已取消：
    # 出货树 = 清单映射结果 + bin/lib 两个构建产物，脚本不再对 payload 做减法。
    # 要把某个文件挡在包外，唯一办法是让它不出现在任何清单条目的源里。
    # ★ 「整包拷 plugins」这条仍然成立（2026-09-17 板端实测订正）：web 侧有
    #   `from plugins.system_host import SystemPluginHost`（以及 system_common / fan /
    #   wifi 等顶层模块与子包）——只拷 web/preview 两个子目录会让 release 树里
    #   `plugins` 包残缺，web 启动即 ModuleNotFoundError。bin/ 闭集（A7）不受影响。

    # usbproxy/：预编译 ELF（DEP-04④）
    # ★ 2026-09-17 板端实测订正：预编译 ELF `usb-proxy` 无 shebang ⇒ MSYS/Git-Bash 判定其
    #   非可执行（源侧 0644）⇒ tar 原样把 0644 带进 payload ⇒ release 树里 usb-proxy 不可执行
    #   ⇒ run-ttbox-usb-proxy.sh 预检直接 exit 1（ttbox-usbproxy.service 反复重启后 failed）。
    #   为何能穿过发布门禁：step5b 的 unit 断言只查 ExecStart 首 token
    #   （= usbproxy/board/run-ttbox-usb-proxy.sh，has shebang ⇒ 0755），**不查**它内部
    #   `exec "$PROJECT_DIR/usb-proxy"` 的那个二进制 ⇒ 该缺陷在门禁下静默通过。
    chmod 0755 -- "${payload}/usbproxy/usb-proxy"

    gen_manifest "$payload" "$ver"

    echo "  [i] payload 就绪: $(find "$payload" -type f | wc -l) 个文件"

    # 激活（T1.01）。首次部署时必须跳过 install 的 restart+健康检查：
    # 那时 /etc/systemd/system 里尚无 ttbox unit，按名 restart 必然失败 → 健康检查必然失败
    # → install 会判"首次部署无版本可回切"而失败。故首次部署只让 install 做
    # "发布 + 原子换链 + unit 断言 + 过渡软链"（TTBOX_SYSTEMD=0），随后由 ensure 负责
    # 安装 unit 并 enable --now（服务真正起来）。已部署环境（unit 已在 /etc）走完整 activate。
    local rc=0
    if [ -f /etc/systemd/system/ttbox-core.service ]; then
        echo "  [i] 已部署环境：走完整 activate（含 restart + 30s 健康检查 + 失败自动回切）"
        bash "$RELEASE_INSTALL" "$ver" "$payload" --activate || rc=$?
    else
        echo "  [i] 首次部署：install 仅切指针（跳过 restart/健康检查），服务由 ensure 拉起"
        TTBOX_SYSTEMD=0 bash "$RELEASE_INSTALL" "$ver" "$payload" --activate || rc=$?
    fi
    rm -rf -- "$payload"; PAYLOAD_DIR=""
    return $rc
}

# ---- 8. 首次部署：把既有模型【复制】（不移动）到 /var/lib/ttbox/models ----
# 旧布局模型在 /opt/ttbox/models；FHS 后客户数据归 /var/lib/ttbox/models。
# 只做 cp -n（no-clobber）：绝不覆盖已有文件、绝不删除源、绝不移动——重跑零副作用。
seed_models_from_legacy() {
    local legacy="/opt/ttbox/models" target="/var/lib/ttbox/models"
    if [ ! -d "$legacy" ]; then
        return 0
    fi
    if [ -z "$(ls -A -- "$legacy" 2>/dev/null)" ]; then
        return 0
    fi
    echo "  [i] 发现旧布局模型目录 ${legacy}，复制（不移动、不覆盖）到 ${target}"
    cp -a -n -- "$legacy"/. "$target"/ 2>/dev/null || \
        echo "  [!] 模型复制有告警（可能是权限/已存在，已按 no-clobber 跳过）"
}

# ---- 9. systemd 单元：自举 ensure 自身 + 调用 ensure 安装其余 unit（DEP-01 / T1.03）----
# unit 安装逻辑【收敛到 ensure 一处】——本函数只负责自举 ensure 的 service/timer
# （ensure 自身不安装自己，避免自举循环），随后由 ensure 从
# /opt/ttbox/current/deploy/systemd/ 安装 5 个运行 unit。
install_ensure_units() {
    local u
    for u in ttbox-ensure.service ttbox-ensure.timer; do
        install -o root -g root -m 0644 \
            "${REPO_ROOT}/deploy/systemd/${u}" "/etc/systemd/system/${u}"
        echo "  [+] /etc/systemd/system/${u}"
    done
    systemctl daemon-reload 2>/dev/null || true
    systemctl enable --now ttbox-ensure.timer >/dev/null 2>&1 && \
        echo "  [+] ttbox-ensure.timer 已启用（每 10 分钟巡检）" || \
        echo "  [!] ttbox-ensure.timer 启用失败（稍后 ensure 巡检也会兜底）"
}

run_ensure_and_units() {
    if [ ! -x "$ENSURE_SCRIPT" ] && [ ! -f "$ENSURE_SCRIPT" ]; then
        echo "  [!] 找不到 ${ENSURE_SCRIPT}，跳过自愈" >&2
        return 0
    fi
    install_ensure_units
    echo "  [i] 调用 ensure 安装/修复运行 unit（unit 源 = ${TTBOX_PREFIX}/current/deploy/systemd/）"
    bash "$ENSURE_SCRIPT" || echo "  [!] ensure 返回非 0（已忽略；巡检 timer 会再兜底）"
}

# ===========================================================================
# 主流程（DEP-03 / DEP-01）
# ===========================================================================
VER="${1:-${TTBOX_RELEASE_VERSION:-1.0.0}}"
echo ""
echo "== 装货进 release 树（sync_tree，版本 ${VER}）=="
# D02（2026-09-18 更新功能定案 A-3）：sync_tree 失败必须 fail-closed——
# 残缺 payload 一旦被激活就是半套服务，比装不上更糟。
sync_tree "$VER" || { echo "  [✗] sync_tree 未完成（见上）—— 中止，绝不产出残缺 payload" >&2; exit 1; }

echo ""
echo "== 首次部署：复制既有模型到 /var/lib/ttbox/models =="
seed_models_from_legacy

echo ""
echo "== 幂等自愈：安装 unit 并启用巡检（ensure / DEP-01）=="
run_ensure_and_units

echo ""
echo "== 完成。后续步骤 =="
echo "  1. 发布/切换走 bootstrap 后，日常发布与回滚统一用发布脚本："
echo "       scripts/ttbox_release_install.sh <ver> <payload_dir> --activate   # 发布+切换"
echo "       scripts/ttbox_release_install.sh --rollback                       # 换指针回上一版"
echo "       scripts/ttbox_release_verify.sh [<ver>]                           # 独立体检"
echo "  2. 手工自愈（unit 被误删/误改时）："
echo "       sudo scripts/ttbox_ensure_services.sh"
echo "       systemctl start ttbox-ensure        # 或等 timer 10 分钟内自动跑"
echo "  3. 健康检查："
echo "     systemctl is-active ttbox-core ttbox-web ttbox-edid"
echo "     ls -l /run/ttbox/                     # core.sock 应在此（srw-rw---- ttbox ttbox）"
echo "     ipc_ping --type PING                   # 未选模型时应能 PING 通（T02 死锁修复）"
echo "     ls /etc/ttbox/config.d/                # 00-factory.json + 10-device.json"
