#!/usr/bin/env bash
# ttbox_conventions_gate.sh — TTBOX「配置 · 常量 · 路径」口径门禁（Task #2 / T-E 交付）
#
# 目的：把 docs/handover/2026-09-17/配置常量路径口径-基线与整改方案-2026-09-17.md §1 的
#   口径规则落成**一条命令可判定**的门禁，防「同一事实多份字面量/多份默认值」的漂移回归
#   （即 §4「补丁式痕迹」专章的 9 类模式）。
#
# 断言（任一 FAIL 即退出非 0）：
#   ① 路径字面量单点化：/run/ttbox/core.sock、/run/ttbox-mouse-passthrough/{cmd,event}.sock、
#      /etc/ttbox/license.key 只允许出现在真源文件（Paths.hpp / paths.py / usb-proxy.cpp / systemd / deploy config）
#   ② 无未登记 RUNTIME/BUILD 环境变量（allowlist = docs/protocols/config-path-env-registry.md §二/§三）
#   ③ 无同义异名 env：TTBOX_CONFIG_PATH / TTBOX_MODEL_ROOT / TTBOX_DEFAULT_WEB_PORT / TTBOX_PORT / TTBOX_WEB_HOST
#   ④ V-03 共享键同值：config/default.json 与 deploy/config/default.json.prod ↔ deploy/config/00-factory.json
#   ⑤ 跨语言同值常量：socket / web 端口 / EDID attempts / 心跳 60·180 逐值相等（B-CONST-2）
#   ⑥ 版本：core/include/ttbox/core/version.hpp::kCoreVersion **去掉字母前缀** == core/CMakeLists.txt project VERSION
#      （2026-09-29 起产品版本带 V 前缀 "V1.0.01"，而 CMake 只解析数字 ⇒ 它降级为数字镜像）
#   ⑦ 无补丁残迹：hardware_display.json 单点（V-19）；systemd_units.py / runner.py / test_systemd_units.py 已删（V-15/16）
#   ⑧ V-07 无绝对路径注入：出货 Python 禁 `sys.path.insert(0, '/opt/…')`（散落字面量 + insert(0) 遮蔽 stdlib）
#   ⑨ TTBOX_PROJECT_ROOT 兜底清零：`#define TTBOX_PROJECT_ROOT` 出现次数必须为 0（强制由 CMake -D 注入）
#   ⑩ 无层级硬编码：禁 `parents[N]` / `"../.."` 相对跳目录（改用 paths.py::discover_root 根锚发现）
#   ⑪ 无开发机绝对路径：禁 `/mnt/g/WORKBUDDY…` / `/mnt/c/Users/<名字>/…` / `C:/Users/…` / `G:/WORKBUDDY…`
#      （这类路径在别人机器上必然不存在、且看起来像正常配置；改脚本自身位置派生或必填项报错）
#
# 用法：
#   bash scripts/ttbox_conventions_gate.sh              # 执行门禁
#   bash scripts/ttbox_conventions_gate.sh --selftest   # 门禁 + 负向控制（证明检测器真能捕获篡改）
# 退出码：0 = PASS；1 = 有 FAIL；2 = 环境错误（缺 python3 / 非仓库根）。

set -uo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

if ! command -v python3 >/dev/null 2>&1; then
    echo "[gate][ERROR] 需要 python3" >&2
    exit 2
fi
if [[ ! -f "${REPO_ROOT}/deploy/config/00-factory.json" ]]; then
    echo "[gate][ERROR] 未在仓库根运行（缺 deploy/config/00-factory.json）: ${REPO_ROOT}" >&2
    exit 2
fi

python3 - "$REPO_ROOT" "$@" <<'PY'
# -*- coding: utf-8 -*-
"""口径门禁核心（内联于 ttbox_conventions_gate.sh）。

只读检查：遍历源码树（排除 docs/third_party/build-*/legacy/tests），断言 7 组口径。
所有判定函数保持**纯函数**形态，便于 --selftest 用篡改输入做负向控制。
"""
import os
import re
import sys
import json
import glob

ROOT = sys.argv[1]
SELFTEST = "--selftest" in sys.argv[2:] or "--selftest" in sys.argv[1:]
try:
    os.chdir(ROOT)
except OSError:
    # 兼容 Windows Git Bash（/c/... → C:/...）下用 Windows Python 直接跑；
    # 板端/WSL 走 try 分支，不受影响。
    _m = re.match(r'^/([a-zA-Z])/(.*)$', ROOT)
    if not _m:
        raise
    os.chdir(_m.group(1).upper() + ":/" + _m.group(2))

# 统一以 cwd 为根，规避盘符/挂载路径差异（os.walk 用相对根，跨平台一致）。
ROOT = "."

_ok = []
_bad = []
_selftest_err = []


def ok(msg):
    _ok.append(msg)


def bad(msg):
    _bad.append(msg)


def st_bad(msg):
    _selftest_err.append(msg)


# ---------------------------------------------------------------------------
# 文件发现 / 注释剥离
# ---------------------------------------------------------------------------
INCLUDE_EXT = {".cpp", ".hpp", ".h", ".py", ".sh", ".js", ".service", ".json"}
# SKIP_TOP：不属「仓库版本化源码」的顶层目录，扫描它们只会产出与交付无关的噪声。
#   · docs/third_party/... = 非出货源码（历史既定）；
#   · .workbuddy = 本机 agent 状态（.gitignore:204，0 跟踪）；
#   · game-assist-lab = 他项目残留（游戏辅助的 3D 数学 + Windows 跨进程内存读写，与 TTBOX 无关）
#     ★ T1.49（2026-09-23）已 mv 入 .archive-2026-09-23/ ⇒ 不再登记（目录若再出现，本门禁应扫它）；
#   · image = ★ T1.49 起**已入库**（出厂镜像烘焙链，54 文件）⇒ 从豁免**移出**、纳入本门禁；
#   · dist = 本地打包产物（.gitignore，与 build-* 同为构建输出）。
SKIP_TOP = {"docs", "third_party", ".git", "__pycache__",
            ".archive-2026-09-17", ".archive-2026-09-18", ".archive-2026-09-23",
            "node_modules", ".mypy_cache", ".workbuddy", "dist"}
# 门禁脚本自身含字面量样例，必须排除，否则自检自恰失败。
SKIP_FILES = {"scripts/ttbox_conventions_gate.sh"}


def iter_files():
    for dp, dn, fn in os.walk(ROOT):
        dn[:] = [d for d in dn
                 if d not in SKIP_TOP and not d.startswith("build-")]
        for f in fn:
            if os.path.splitext(f)[1] not in INCLUDE_EXT:
                continue
            p = os.path.relpath(os.path.join(dp, f), ROOT).replace("\\", "/")
            if p in SKIP_FILES or p.startswith("scripts/legacy/"):
                continue
            yield p


def read(p):
    try:
        with open(p, encoding="utf-8", errors="ignore") as fh:
            return fh.read()
    except OSError:
        return ""


def strip_comment(line, ext):
    """剥去行内注释（python/shell/systemd 用 #，C/C++/JS 用 //）。json 无注释。"""
    if ext in (".py", ".sh", ".service"):
        i = line.find("#")
        return line[:i] if i >= 0 else line
    if ext in (".cpp", ".hpp", ".h", ".js"):
        i = line.find("//")
        return line[:i] if i >= 0 else line
    return line


# ---------------------------------------------------------------------------
# ① 路径字面量单点化（A-PATH-5）
# ---------------------------------------------------------------------------
LIT_ALLOW = {
    "/run/ttbox/core.sock": {
        "core/src/common/Paths.hpp",
        "plugins/web/lib/paths.py",
        "deploy/systemd/ttbox-core.service",
        "deploy/systemd/ttbox-preview.service",
        "deploy/systemd/ttbox-web.service",
    },
    "/run/ttbox-mouse-passthrough/cmd.sock": {
        "core/src/common/Paths.hpp",
        "plugins/web/lib/paths.py",
        "usbproxy/usb-proxy.cpp",
        "deploy/config/10-device.json",
        "deploy/config/default.json.prod",
        # 2026-09-23 补：image/factory/10-device.json 是**出厂镜像烘焙时真正灌进板子**的设备层
        # 配置（配置真源第四处，见 image/README.md），与 deploy/config 两份同类 —— 它此前
        # 一直在门禁 SKIP_TOP 豁免里，入库后首次纳管才暴露，属补登记而非新引入。
        "image/factory/10-device.json",
    },
    "/run/ttbox-mouse-passthrough/event.sock": {
        "core/src/common/Paths.hpp",
        "plugins/web/lib/paths.py",
        "usbproxy/usb-proxy.cpp",
        "deploy/config/10-device.json",
        "deploy/config/default.json.prod",
        "image/factory/10-device.json",
    },
    "/etc/ttbox/license.key": {
        "core/src/common/Paths.hpp",
    },
}
# 每个字面量必须在其 SSOT 文件里被真正定义（防「一刀切删除」把真源也删了）
LIT_SSOT = {
    "/run/ttbox/core.sock": ["core/src/common/Paths.hpp", "plugins/web/lib/paths.py"],
    "/run/ttbox-mouse-passthrough/cmd.sock": ["core/src/common/Paths.hpp", "plugins/web/lib/paths.py"],
    "/run/ttbox-mouse-passthrough/event.sock": ["core/src/common/Paths.hpp", "plugins/web/lib/paths.py"],
    "/etc/ttbox/license.key": ["core/src/common/Paths.hpp"],
}


def literal_in_code(text, lit, ext):
    """在剥离注释后的代码里判断字面量是否出现（纯函数，供 selftest 复用）。"""
    for line in text.splitlines():
        if lit in strip_comment(line, ext):
            return True
    return False


def check_literals():
    for lit, allowed in LIT_ALLOW.items():
        hits = {}
        for p in iter_files():
            ext = os.path.splitext(p)[1]
            for n, line in enumerate(read(p).splitlines(), 1):
                if lit in strip_comment(line, ext):
                    hits.setdefault(p, []).append(n)
        stray = {p: v for p, v in hits.items() if p not in allowed}
        for p, v in sorted(stray.items()):
            bad("① 散落路径字面量 %s 出现于非真源文件 %s（行 %s）"
                % (lit, p, ",".join(map(str, v[:5]))))
        for req in LIT_SSOT[lit]:
            if req not in hits:
                bad("① SSOT 文件 %s 未定义字面量 %s（真源丢失）" % (req, lit))
        if not stray and all(r in hits for r in LIT_SSOT[lit]):
            ok("① 路径字面量单点化：%s 仅出现于 %s"
               % (lit, sorted(p for p in hits)))


# ---------------------------------------------------------------------------
# ② 环境变量 allowlist（D-ENV-2）
# ---------------------------------------------------------------------------
ENV_ALLOW = {
    # ---- Core (C++) ----
    "TTBOX_CONFIG", "TTBOX_IPC_SOCKET", "TTBOX_MODELS_ROOT", "TTBOX_HID_ROOT",
    "TTBOX_LICENSE_SERVER", "TTBOX_APP_KEY", "TTBOX_CLIENT_SECRET",
    # 2026-09-23 登记：359f0f0（修 SO_RCVTIMEO 平台语义）引入的服务端诊断开关，
    # 当时漏了登记 —— 出货前跑门禁才暴露（第②项 FAIL）。登记表 §2.1 已同步。
    "TTBOX_IPC_DEBUG",
    # 2026-10-01 登记（批次 1.4 日志分级与分流）：core 日志目录覆盖。
    #   默认 /var/log/ttbox（Paths.hpp::kLogDirDefault，由 fhs_init 建）；消费方 =
    #   core Logger 的 FileSink（ttbox.log / ttbox-error.log / operation.log）与
    #   web 侧文件 sink（web.log）。不设即用默认，属纯增量、生产可不设。
    "TTBOX_LOG_DIR",
    # 2026-10-01 登记（批次 1.4）：日志级别（debug|info|warn|error|fatal|off）。
    #   §5.1「级别必须可在配置里调，不用重编译」—— core 侧对应 --log-level，
    #   web 侧（plugins/web/lib/logging_setup.py）用本变量，取值集合与 core 完全相同。
    "TTBOX_LOG_LEVEL",
    # ---- Web / preview (Python) ----
    "TTBOX_ROOT", "TTBOX_PREFIX", "TTBOX_SCRIPTS_DIR", "TTBOX_PRESETS_DIR",
    "TTBOX_HDMIRX_EDID", "TTBOX_MOTION_PROFILES_DIR", "TTBOX_CONFIG_DIR",
    "TTBOX_WEB_CREDENTIALS", "TTBOX_PLUGINS_ROOT", "TTBOX_PLUGIN_REPOSITORY_ROOT",
    "TTBOX_ENABLE_DESIGNER", "TTBOX_DISPLAY_CONFIG", "TTBOX_IPC_TCP",
    "TTBOX_UI_CUSTOM_CSS", "TTBOX_CONVERT_WORKDIR", "TTBOX_CONVERTER_SCRIPT",
    "TTBOX_CONVERTER_PYTHON", "TTBOX_CONVERT_CALIB_DIR", "TTBOX_ALLOW_ONNX",
    "TTBOX_CLOUD_SESSION", "TTBOX_WEB_PORT",
    "TTBOX_PREVIEW_HOST", "TTBOX_PREVIEW_PORT", "TTBOX_PREVIEW_URL",
    # ---- shell 运维 / 发布 ----
    "TTBOX_ETC", "TTBOX_RUN", "TTBOX_REPO", "TTBOX_KEEP_VERSIONS",
    "TTBOX_EDID_REHANDSHAKE", "TTBOX_EDID_REHANDSHAKE_ATTEMPTS",
    "TTBOX_EDID_HPD_SETTLE_SEC", "TTBOX_EDID_LOCK_TIMEOUT_SEC",
    "TTBOX_SYSTEMD", "TTBOX_UNIT_DIR", "TTBOX_RELEASE_VERSION",
    "TTBOX_CURRENT", "TTBOX_STATE", "TTBOX_OTA_PRIV_PASSWORD",
    "TTBOX_RELEASE_SELFTEST", "TTBOX_RESTART_UNITS", "TTBOX_HEALTH_TIMEOUT",
    "TTBOX_REAL_CORE_MAIN",
    # 2026-09-23 登记：出厂镜像烘焙链（image/**）此前整目录在 SKIP_TOP 豁免里，
    # 入库后首次纳管才暴露（不是新引入，是补登记）。4 个都是烘焙期由构建方注入的参数；
    # 其中凭据类两个（ROOT_PASS / HOSTKEY_DIR）明文只留项目外目录、不入镜像与文档，
    # 且 V5 起禁止回落固定弱口令 —— 见 image/steps/04_board_config.sh 开头注释。
    "TTBOX_STAGE_DIR", "TTBOX_VER", "TTBOX_HOSTKEY_DIR", "TTBOX_ROOT_PASS",
    # 2026-09-26 登记：90_finalize_host.sh 收尾门禁的可选「钉版本」开关（升版重烤用）
    "TTBOX_EXPECT_VER",
    # 2026-09-22 回流板端 dtb 修复脚本时登记（TEST 钩子：覆盖 DTB 期望哈希与报告路径）
    "TTBOX_DTB_SRC", "TTBOX_DTB_FIX_TEST", "TTBOX_DTB_REPORT",
    "TTBOX_DTB_GOOD_SHA", "TTBOX_DTB_BAD_SHA",
    # 2026-09-22 USB 透传模式运维脚本的测试钩子（放行非 root 写 drop-in）
    "TTBOX_USB_MODE_TEST",
    # 2026-10-01 登记：scripts/a9_ai_isolation.sh（板端手动 HW 并发隔离诊断）用的
    # HW 测试二进制目录。**刻意无默认值** —— 板端发布树 /opt/ttbox 只有 releases/current，
    # 不含构建产物，猜任何路径都是错的（不指定即人话报错）。
    # ★ 刻意不复用 TTBOX_BUILD_DIR：后者在登记表 §3.3 是 BUILD 域（交叉编译产物目录，
    #   ttbox_build_release.sh / fhs_init 消费），借用即同名异义。
    "TTBOX_HW_TEST_DIR",
    # 2026-10-01 登记：ttbox_phase1_gate.sh 的 Python 解释器显式指定。
    #   本机解释器位置属**本机事实**，不该硬编码进版本化脚本（原实现写死
    #   "C:/Users/Administrator/..." 当兜底，换机器即废，且违反本门禁⑪）。
    #   不设则按 python3 → python → $HOME/.workbuddy/.../envs/default/Scripts/python.exe
    #   顺序做**能力探测**（G6 要的是能 import pytest 的那个，不是"存在"的那个）。
    "TTBOX_PYTHON",
    # ---- BUILD（编译期/门禁；不得进运行期业务路径）----
    "TTBOX_BUILD_DIR", "TTBOX_PROJECT_ROOT", "TTBOX_GIT_SHA", "TTBOX_RKNNRT_SO",
    "TTBOX_USBPROXY_INCLUDE", "TTBOX_USBPROXY_LIBDIR",
    "TTBOX_RELEASE_VERIFY_REPRO", "TTBOX_RELEASE_VERIFY_USBPROXY_REBUILD",
    "TTBOX_DEVICE_LIBS", "TTBOX_SKIP_CONVENTIONS_GATE",
    # ---- WiFi 引导 ----
    "TTBOX_WIFI_DEFAULT_PASSWORD", "TTBOX_WIFI_DEFAULT_SSIDS", "TTBOX_WIFI_DEFAULT_SSID",
    "TTBOX_WIFI_DEFAULT_CONNECTION", "TTBOX_WIFI_DEFAULT_CONNECTION_PREFIX",
    "TTBOX_WIFI_USER_CONNECTION_PREFIX", "TTBOX_WIFI_AP_CONNECTION",
    "TTBOX_WIFI_BOOTSTRAP_SERVICE",
    # ---- usbproxy（历史 USB_PROXY_ 前缀例外，§四）----
    "USB_PROXY_DEVICE", "USB_PROXY_DRIVER",
    "USB_PROXY_SOCKET_DIR", "USB_PROXY_WAIT_SECONDS", "USB_PROXY_BIN",
    "USB_PROXY_EXTRA_ARGS", "USB_PROXY_GADGET_CONFIG_FILE",
    # 1.5.26 新增（自带库目录 + 两处死等超时；登记册 §2.3）
    "USB_PROXY_LIBDIR", "USB_PROXY_UDC_WAIT_SECONDS", "USB_PROXY_MOUSE_WAIT_SECONDS",
    # 1.5.62：USB_PROXY_MODE / 自动接管两项随「合成模式删除」一并移除
    # 1.5.22 起 ttbox_dtb_fix.sh 的 TEST 域钩子（源 DTB 路径覆盖 / 免 root 免重启开关；登记册 §2.5）
    "TTBOX_DTB_SRC", "TTBOX_DTB_FIX_TEST",
}
# 已删除 / 禁止复活的同义异名（D-ENV-3 / D-ENV-5）
ENV_BANNED = [
    "TTBOX_CONFIG_PATH", "TTBOX_MODEL_ROOT", "TTBOX_DEFAULT_WEB_PORT",
    "TTBOX_PORT", "TTBOX_WEB_HOST",
]
ENV_PATS = [
    re.compile(r'os\.environ(?:\.get\(|\[)\s*[\'"]([A-Z][A-Z0-9_]+)[\'"]'),
    re.compile(r'os\.getenv\(\s*[\'"]([A-Z][A-Z0-9_]+)[\'"]'),
    re.compile(r'getenv\(\s*"([A-Z][A-Z0-9_]+)"'),
    re.compile(r'\$\{([A-Z][A-Z0-9_]+)'),
    re.compile(r'\$([A-Z][A-Z0-9_]+)'),
    re.compile(r'^\s*Environment=([A-Z][A-Z0-9_]+)=', re.M),
    re.compile(r'\bexport\s+([A-Z][A-Z0-9_]+)='),
]


# D-ENV-1 TEST 域验收脚本（登记表 §2.5）：显式点名，不做通配豁免；新增须刻意加入。
TEST_DOMAIN_SCRIPTS = {
    "scripts/ttbox_m207_accept.py",
    "scripts/ttbox_m207_b21_expire.py",
    "scripts/ttbox_m2xx_console_accept.py",
}


def env_scan_excluded(p):
    """测试/自测/门禁脚本不在 RUNTIME 扫描域（D-ENV-1 TEST 域）。"""
    parts = p.split("/")
    if p.startswith("tests/") or any(s in ("tests",) for s in parts[:-1]):
        return True
    base = os.path.basename(p)
    if base.startswith("test_"):
        return True
    if p in TEST_DOMAIN_SCRIPTS:
        return True
    if base in ("ttbox_release_verify.sh", "ttbox_conventions_gate.sh"):
        return True
    if base.endswith("_selftest.sh") or base.endswith("_verify.sh"):
        return True
    return False


def scan_env_names(text, ext):
    names = set()
    for line in text.splitlines():
        code = strip_comment(line, ext)
        for pat in ENV_PATS:
            for m in pat.findall(code):
                if m.startswith(("TTBOX_", "USB_PROXY_")):
                    names.add(m)
    return names


def check_env():
    for p in iter_files():
        if env_scan_excluded(p):
            continue
        ext = os.path.splitext(p)[1]
        for name in sorted(scan_env_names(read(p), ext)):
            if name in ENV_BANNED:
                bad("③ 使用了已删除的同义异名 env %s @ %s（D-ENV-3/D-ENV-5）" % (name, p))
            elif name not in ENV_ALLOW:
                bad("② 未登记 env %s @ %s（需登记 docs/protocols/config-path-env-registry.md）"
                    % (name, p))
    # ③ 同义异名：**生产代码**（含注释剥离后）不得再出现。
    #   carve-out = env_scan_excluded（**仅 TEST 域**：tests/、test_*.py、TEST_DOMAIN_SCRIPTS 点名的验收脚本、
    #   *_verify.sh / *_selftest.sh）。唯一理由：回归/验收脚本必须**点名**同义异名，才能断言其
    #   "绝迹"（B27 断言 TTBOX_MODEL_ROOT 残留=False、B29 断言 TTBOX_WEB_HOST/PORT 无残留）。
    #   范围严格限定 TEST 域 —— 生产源码（core/src、core/include、plugins/web/bin、scripts/*.py
    #   非验收件等）**不在**豁免内，仍逐行扫。
    for p in iter_files():
        if env_scan_excluded(p):
            continue
        ext = os.path.splitext(p)[1]
        text = read(p)
        for name in ENV_BANNED:
            for n, line in enumerate(text.splitlines(), 1):
                if name in strip_comment(line, ext):
                    bad("③ 同义异名 env 残留 %s @ %s:%d" % (name, p, n))
    if not _bad:
        ok("② /③ 环境变量：RUNTIME/BUILD 全部已登记，无同义异名残留")


# ---------------------------------------------------------------------------
# ④ V-03 共享键同值
# ---------------------------------------------------------------------------
def load_json(p):
    with open(p, encoding="utf-8") as fh:
        return json.load(fh)


def shared_key_mismatches(cand, factory, skip=frozenset({"_comment"})):
    out = []
    for k in sorted(set(cand) & set(factory)):
        if k in skip:
            continue
        if cand[k] != factory[k]:
            out.append((k, cand[k], factory[k]))
    return out


def check_shared_keys(factory):
    for cand_path in ("config/default.json", "deploy/config/default.json.prod"):
        cand = load_json(cand_path)
        mm = shared_key_mismatches(cand, factory)
        for k, cv, fv in mm:
            bad("④ V-03 共享键异值：%s[%s]=%r != 00-factory %r" % (cand_path, k, cv, fv))
        if not mm:
            shared = len(set(cand) & set(factory)) - 1
            ok("④ V-03 共享键同值：%s ↔ 00-factory（%d 个共享键）" % (cand_path, shared))


# ---------------------------------------------------------------------------
# ⑤ 跨语言同值常量（B-CONST-2）
# ---------------------------------------------------------------------------
def hpp_const(text, name):
    m = re.search(r'\b' + re.escape(name) + r'\s*=\s*"([^"]*)"', text)
    return m.group(1) if m else None


def py_const(text, name):
    m = re.search(r'(?m)^' + re.escape(name) + r'\s*=\s*"([^"]*)"', text)
    return m.group(1) if m else None


def int_const(text, name):
    m = re.search(r'(?m)^' + re.escape(name) + r'\s*=\s*(\d+)', text)
    return int(m.group(1)) if m else None


def check_crosslang():
    hpp = read("core/src/common/Paths.hpp")
    py = read("plugins/web/lib/paths.py")
    usb = read("usbproxy/usb-proxy.cpp")
    for hn, pn in (("kIpcSocketDefault", "IPC_SOCKET_DEFAULT"),
                   ("kMouseCmdSocketDefault", "MOUSE_CMD_SOCK_DEFAULT"),
                   ("kMouseEventSocketDefault", "MOUSE_EVENT_SOCK_DEFAULT")):
        hv, pv = hpp_const(hpp, hn), py_const(py, pn)
        if hv is None or pv is None:
            bad("⑤ 跨语言常量缺定义：%s=%r / %s=%r" % (hn, hv, pn, pv))
        elif hv != pv:
            bad("⑤ 跨语言常量异值：%s=%r != %s=%r" % (hn, hv, pn, pv))
    # usb-proxy.cpp 与 Paths.hpp 逐字符相等
    m1 = re.search(r'std::string\s+mouse_cmd_socket\s*=\s*"([^"]*)"', usb)
    m2 = re.search(r'std::string\s+mouse_event_socket\s*=\s*"([^"]*)"', usb)
    if not m1 or m1.group(1) != hpp_const(hpp, "kMouseCmdSocketDefault"):
        bad("⑤ usb-proxy.cpp mouse_cmd_socket 与 Paths.hpp::kMouseCmdSocketDefault 不一致")
    if not m2 or m2.group(1) != hpp_const(hpp, "kMouseEventSocketDefault"):
        bad("⑤ usb-proxy.cpp mouse_event_socket 与 Paths.hpp::kMouseEventSocketDefault 不一致")

    # web 端口：paths.py / wifi_manager.py / release_install.sh 三镜像同值，且 LISTEN_PORT 派生自真源
    port_py = int_const(py, "WEB_PORT_DEFAULT")
    port_wifi = int_const(read("scripts/wifi_manager.py"), "WEB_PORT_DEFAULT")
    m = re.search(r'WEB_PORT="\$\{TTBOX_WEB_PORT:-(\d+)\}"', read("scripts/ttbox_release_install.sh"))
    port_inst = int(m.group(1)) if m else None
    web_py = read("plugins/web/bin/ttbox-web.py")
    # ★ 2026-10-03（S9/S10 后）：LISTEN_PORT 已搬进 lib/settings.py，入口只import。
    #   判据跟着搬 —— 原来只 grep 入口文件，重构后必然 FAIL（假红）。
    #   ★ 但不能只查 settings：还要确认**入口确实 import 了它**，
    #     否则「settings 里定义了、入口没用」同样是真源漂移（判据只查一半是半个守卫）。
    settings_py = read("plugins/web/lib/settings.py")
    derived = "LISTEN_PORT = ttbox_paths.WEB_PORT_DEFAULT" in settings_py
    imported = re.search(r'^\s*LISTEN_PORT,\s*$', web_py, re.M) is not None
    if not (derived and imported):
        bad("⑤ LISTEN_PORT 未派生自 paths.py::WEB_PORT_DEFAULT（派生=%s 入口import=%s）"
            % (derived, imported))
    if port_py is None or port_wifi != port_py or port_inst != port_py:
        bad("⑤ web 端口跨语言异值：paths.py=%r wifi_manager.py=%r release_install.sh=%r"
            % (port_py, port_wifi, port_inst))
    if port_py is not None and port_py == port_wifi == port_inst and derived and imported:
        ok("⑤ 跨语言同值：socket×3 / web 端口 %d / usb-proxy socket 全部一致" % port_py)

    # EDID 重协商 attempts：单一真源 = 2（★ 2026-09-28 由 12 下调；V-09 的"单一真源"约束不变，
    # 变的是那个真源的值）。为什么下调：每一轮重协商都要拉低/拉高一次 HPD，源端就重新枚举一次
    # （= 屏幕黑一次）；12 轮意味着最坏 12 次黑屏，客户体感"开机黑屏多次"。
    # 板端实测首轮 7~8s 即 lock ok，2 轮足够覆盖"源端一次没枚举完"的情形。
    # 配套：deploy/systemd/ttbox-edid.service 的 timeout 同步 20s→45s（否则跑不完一轮）。
    edid = read("scripts/edid/edid_apply.sh")
    if int_const(edid, "ATTEMPTS_DEFAULT") != 2:
        bad("⑤ EDID ATTEMPTS_DEFAULT 非 2（V-09 单一真源被破坏）")
    for n, line in enumerate(web_py.splitlines(), 1):
        if "TTBOX_EDID_REHANDSHAKE_ATTEMPTS" in strip_comment(line, ".py"):
            bad("⑤ ttbox-web.py:%d 仍覆写 TTBOX_EDID_REHANDSHAKE_ATTEMPTS（V-09 回归）" % n)

    # 心跳 60/180：唯一定义在 LicenseConstants.hpp，其余引用
    lc = read("core/src/auth/LicenseConstants.hpp")
    if not re.search(r'kHeartbeatIntervalSecDefault\s*=\s*60', lc):
        bad("⑤ 心跳间隔默认 60 未单点定义于 LicenseConstants.hpp")
    if not re.search(r'kHeartbeatTimeoutSecDefault\s*=\s*180', lc):
        bad("⑤ 心跳超时默认 180 未单点定义于 LicenseConstants.hpp")
    for f in ("core/src/auth/LicenseDaemon.cpp", "core/src/auth/LicenseGate.cpp",
              "core/src/auth/LicenseGate.hpp", "core/src/auth/LicenseStateMachine.hpp",
              "core/src/auth/TtboxLicenseClient.cpp"):
        if "kHeartbeatIntervalSecDefault" not in read(f):
            bad("⑤ %s 未引用心跳单点常量 kHeartbeatIntervalSecDefault" % f)


# ---------------------------------------------------------------------------
# ⑥ 版本同值（B-CONST-3）
# ---------------------------------------------------------------------------
def _ver_mirror(product_version):
    """产品版本 -> CMake 数字镜像：去掉前导字母前缀（V1.0.01 -> 1.0.01）。

    ★ 2026-09-29：产品版本改走「V 线」后，CMake 的 project VERSION 只解析数字
    ⇒ 它只能存镜像，判据改成「剥掉前导字母后相等」。
    """
    return re.sub(r'^[A-Za-z]+', '', str(product_version or ''))


def check_version():
    vh = read("core/include/ttbox/core/version.hpp")
    cm = read("core/CMakeLists.txt")
    m1 = re.search(r'kCoreVersion\s*=\s*"([^"]*)"', vh)
    m2 = re.search(r'project\(ttbox_core VERSION ([\d.]+)', cm)
    if not m1 or not m2:
        bad("⑥ 版本真源缺失：version.hpp::kCoreVersion 或 CMakeLists project VERSION 未找到")
    elif _ver_mirror(m1.group(1)) != m2.group(1):
        bad("⑥ 版本异值：kCoreVersion=%r 去字母前缀=%r != CMake project VERSION=%r"
            % (m1.group(1), _ver_mirror(m1.group(1)), m2.group(1)))
    else:
        ok("⑥ 版本同值：core %s（version.hpp 去前缀 %s == CMakeLists %s）"
           % (m1.group(1), _ver_mirror(m1.group(1)), m2.group(1)))


# ---------------------------------------------------------------------------
# ⑦ 无补丁残迹
# ---------------------------------------------------------------------------
def check_no_patch():
    hd = [p.replace("\\", "/") for p in glob.glob("**/hardware_display.json", recursive=True)
          if not p.replace("\\", "/").startswith(("docs/", "third_party/"))]
    if hd != ["deploy/config/hardware_display.json"]:
        bad("⑦ V-19 hardware_display.json 非单点：实际 %s（应仅 deploy/config/hardware_display.json）" % hd)
    else:
        ok("⑦ hardware_display.json 单点 = deploy/config/hardware_display.json")
    for p in ("ttbox_platform/supervisor/systemd_units.py", "ttbox_platform/supervisor/runner.py",
              "ttbox_platform/tests/test_systemd_units.py", "config/hardware_display.json"):
        if os.path.exists(p):
            bad("⑦ 补丁残迹未删除：%s（V-15/V-16/V-19）" % p)
    # config/default.json 不得再自带鼠标透传 socket（单点化）
    try:
        cand = load_json("config/default.json")
        for k in ("output_proxy_socket", "input_event_socket"):
            if k in cand:
                bad("⑦ config/default.json 仍自带 %s（应回落到 Paths.hpp 默认，A-PATH-5）" % k)
    except (OSError, ValueError) as exc:
        bad("⑦ config/default.json 读取失败：%s" % exc)


# ---------------------------------------------------------------------------
# ⑧ V-07：出货 Python 禁绝对路径 sys.path 注入
# ---------------------------------------------------------------------------
# 反模式：sys.path.insert(0, '/opt/ttbox/scripts') —— 既散落绝对路径字面量（A-PATH-3/5 违背），
# 又把目录顶到 sys.path 最前（insert(0)）有遮蔽 stdlib 的风险（见 ttbox-web.py 头部 platform 冲突）。
# 正解：经 lib.paths 相对派生 + append（受单点真源约束）。
_ABS_INSERT = re.compile(r'sys\.path\.insert\(\s*0\s*,\s*[\'"](/[^\'"]*)[\'"]')


def check_no_abs_insert():
    found = []
    for p in iter_files():
        if os.path.splitext(p)[1] != ".py":
            continue
        for n, line in enumerate(read(p).splitlines(), 1):
            m = _ABS_INSERT.search(strip_comment(line, ".py"))
            if m:
                found.append((p, n, m.group(1)))
    for p, n, lit in found:
        bad("⑧ 出货 Python 绝对路径注入 sys.path.insert(0, '%s') @ %s:%d"
            "（V-07：改走 lib.paths 相对派生 + append）" % (lit, p, n))
    if not found:
        ok("⑧ 无绝对路径 sys.path 注入（出货 Python 全绿）")


# ---------------------------------------------------------------------------
# ⑨ TTBOX_PROJECT_ROOT 兜底清零
# ---------------------------------------------------------------------------
# TTBOX_PROJECT_ROOT 必须由 CMake -D 注入（target_compile_definitions(ttbox_core PUBLIC ...)）。
# 任何 `#define TTBOX_PROJECT_ROOT` 兜底都会把"/"或"."静默顶替真实运行根 ⇒ 掩蔽注入掉线。
_ROOT_FALLBACK = re.compile(r'^\s*#\s*define\s+TTBOX_PROJECT_ROOT\b')


def check_project_root_injected():
    hits = []
    for p in iter_files():
        if os.path.splitext(p)[1] not in (".cpp", ".hpp", ".h"):
            continue
        for n, line in enumerate(read(p).splitlines(), 1):
            if _ROOT_FALLBACK.search(line):
                hits.append("%s:%d" % (p, n))
    for h in hits:
        bad("⑨ TTBOX_PROJECT_ROOT 静默兜底 #define 仍在（应改 #error，由 CMake -D 注入）：%s" % h)
    if not hits:
        ok("⑨ 无 TTBOX_PROJECT_ROOT 兜底 #define（强制由 CMake 注入）")


# ---------------------------------------------------------------------------
# ⑩ 层级硬编码清零（P6-2026-10-01）
# ---------------------------------------------------------------------------
# 反模式：`Path(__file__).resolve().parents[3]` / `os.path.join(dirname, "..", "..", "plugins")`
#   · 层级是硬编码常量：搬一次目录布局就静默指错根；错根**不报错**，只在下游表现为
#     "某个文件找不到"，排障成本极高；
#   · 把 parents[3] 改成 parents[4] 只是把错的深度换成另一个错的深度，不是修复。
# 正解：根锚发现 —— 向上找**最近一层**同时含 plugins/framework/scripts/deploy 的目录
#   （唯一实现 = plugins/web/lib/paths.py::discover_root）。
# 豁免：注释行；以及含反引号的说明行（文档式内联引用，如 “原 ``parents[1]``”）。
_PARENTS_HOP = re.compile(r'\.parents\[[0-9]+\]')
# 三种"相对跳目录"写法：① 整串就是 "../.." 或 "../../"；② `"..", ".."` 逗号分段；
# ③ sys.path 操作里出现 ".."。**不**匹配 '../../etc/passwd' 这类路径穿越测试样本。
_REL_HOP = re.compile(r'''['"]\.\./\.\./?['"]''')
_JOIN_HOP = re.compile(r''',\s*['"]\.\.['"]\s*,\s*['"]\.\.['"]''')
_SYSPATH_HOP = re.compile(r'sys\.path\.(?:append|insert|extend)\([^)]*\.\.')


def check_no_depth_hardcode():
    hits = []
    for p in iter_files():
        ext = os.path.splitext(p)[1]
        if ext not in (".py", ".sh"):
            continue
        for n, line in enumerate(read(p).splitlines(), 1):
            if "`" in line:
                continue                              # 反引号说明行
            body = strip_comment(line, ext)
            if (_PARENTS_HOP.search(body) or _REL_HOP.search(body)
                    or _JOIN_HOP.search(body) or _SYSPATH_HOP.search(body)):
                hits.append("%s:%d" % (p, n))
    for h in hits:
        bad("⑩ 层级硬编码（parents[N] / \"../..\"）@ %s —— 改用 paths.py::discover_root 根锚发现" % h)
    if not hits:
        ok("⑩ 无层级硬编码（parents[N] / \"../..\" 出货面全绿）")


# ---------------------------------------------------------------------------
# ⑪ 开发机绝对路径清零（P8 复查-2026-10-01）
# ---------------------------------------------------------------------------
# 反模式：把**开发机私有**目录写进版本化文件 ——
#   `/mnt/g/WORKBUDDY工作区/...`（本机源码树）、`/mnt/c/Users/<名字>/...`（本机下载/密钥目录）、
#   `C:/Users/<名字>/...`、`G:/WORKBUDDY...`。
#   · 这些路径在别人机器上**必然不存在**，脚本一跑就是 cd 失败 / 找不到文件；
#     更糟的是它们**看起来像正常配置**，排障时得先怀疑到它头上；
#   · 它们描述的是"某个人的机器"，不是项目事实 ⇒ 一律不准进版本化文件。
#   · 实测来源（2026-10-01 复查）：image/*.sh 的源镜像 / 密钥目录 / 成品落点；
#     ttbox_phase1_gate.sh 的 python 绝对路径兜底 —— 后者换台机器整个门禁直接废。
# 正解（按优先级）：
#   ① 脚本自身位置派生 —— shell：`$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)`；
#      Python：`Path(__file__).resolve()` + `lib.paths::discover_root` 根锚；
#   ② 必填项：`${VAR:-}` + 空则**人话报错**（讲清该传什么），不要编一个"看起来像"的默认值
#      —— 编了等于没修，只是把"确定错"换成"大概率也错"；
#   ③ 真要默认值就用板端运行根（/opt/ttbox、/var/lib/ttbox）—— 那才是项目事实。
# 豁免：注释行；含反引号的说明行；**检测规则自身的定义行**（含 DEV_PATH_PAT，
#   如 ttbox_pack_ota.sh 断言 2b 的正则 —— 它必须原样写出这些字样）。
_MACHINE_PATH = re.compile(r'/mnt/[a-z]/WORKBUDDY|/mnt/[a-z]/[Uu]sers/|C:/Users/|G:/WORKBUDDY')


def check_no_machine_paths():
    hits = []
    for p in iter_files():
        ext = os.path.splitext(p)[1]
        for n, line in enumerate(read(p).splitlines(), 1):
            if "`" in line or "DEV_PATH_PAT" in line:
                continue                          # 反引号说明行 / 检测规则定义行
            if _MACHINE_PATH.search(strip_comment(line, ext)):
                hits.append("%s:%d" % (p, n))
    for h in hits:
        bad("⑪ 开发机绝对路径 @ %s —— 改脚本自身位置派生，或必填项 `${VAR:-}` + 人话报错" % h)
    if not hits:
        ok("⑪ 无开发机绝对路径（源码面全绿）")


# ---------------------------------------------------------------------------
# --selftest 负向控制（证明检测器真能捕获篡改）
# ---------------------------------------------------------------------------
def run_selftest(factory):
    # 1) V-03：篡改 factory 的 conf ⇒ 必然报异值
    tampered = dict(factory)
    tampered["conf"] = 0.123456
    if not shared_key_mismatches(load_json("config/default.json"), tampered):
        st_bad("V-03 检测器失效：共享键 conf 篡改未被捕获")
    # 2) 字面量：散写一行 ⇒ 必被捕获
    if not literal_in_code('x = "/run/ttbox/core.sock"', "/run/ttbox/core.sock", ".py"):
        st_bad("字面量检测器失效：代码散写未被捕获")
    # 3) 字面量：注释行 ⇒ 不得误报
    if literal_in_code('# 参见 /run/ttbox/core.sock', "/run/ttbox/core.sock", ".py"):
        st_bad("字面量检测器误报：注释行被当成散落")
    # 4) env：禁用名残留 ⇒ 必被捕获
    if "TTBOX_MODEL_ROOT" not in scan_env_names("os.environ.get('TTBOX_MODEL_ROOT')", ".py"):
        st_bad("env 扫描器失效：禁用名未被捕获")
    # 5) 版本：异值必被捕获；★ 且 V 前缀必须被正确剥离（否则同源会被误报成异值）
    if _ver_mirror(hpp_const('inline constexpr const char* kCoreVersion = "9.9.9";', "kCoreVersion")) == \
            re.search(r'project\(ttbox_core VERSION ([\d.]+)', read("core/CMakeLists.txt")).group(1):
        st_bad("版本检测器失效：异值未被捕获")
    if _ver_mirror("V9.9.9") != "9.9.9" or _ver_mirror("9.9.9") != "9.9.9" or _ver_mirror("") != "":
        st_bad("版本归一器失效：V 前缀未被正确剥离")
    if _ver_mirror(re.search(r'kCoreVersion\s*=\s*"([^"]*)"',
                             read("core/include/ttbox/core/version.hpp")).group(1)) != \
            re.search(r'project\(ttbox_core VERSION ([\d.]+)', read("core/CMakeLists.txt")).group(1):
        st_bad("版本归一器误报：当前 V 线版本未与 CMake 数字镜像对上")
    # 6) V-07：绝对路径 insert(0, '/opt/…') ⇒ 必被捕获
    if not _ABS_INSERT.search('sys.path.insert(0, "/opt/ttbox/scripts")'):
        st_bad("V-07 检测器失效：绝对路径 sys.path 注入未被捕获")
    # 7) V-07：相对派生 append ⇒ 不得误报
    if _ABS_INSERT.search('sys.path.append(str(Path(__file__).resolve().parents[1]))'):
        st_bad("V-07 检测器误报：相对派生 append 被误判为绝对路径注入")
    # 8) TTBOX_PROJECT_ROOT 兜底 ⇒ 必被捕获
    if not _ROOT_FALLBACK.search('#define TTBOX_PROJECT_ROOT "."'):
        st_bad("TTBOX_PROJECT_ROOT 兜底检测器失效：兜底 #define 未被捕获")
    # 9) ⑩ 层级硬编码 ⇒ 必被捕获；注释/反引号说明 ⇒ 不得误报
    if not _PARENTS_HOP.search('ROOT = Path(__file__).resolve().parents[3]'):
        st_bad("层级硬编码检测器失效：parents[3] 未被捕获")
    if not _JOIN_HOP.search('os.path.join(d, "..", "..", "plugins")'):
        st_bad("层级硬编码检测器失效：\"..\", \"..\" 相对跳目录未被捕获")
    if not _SYSPATH_HOP.search("sys.path.append('../../plugins')"):
        st_bad("层级硬编码检测器失效：sys.path 里的 '../../plugins' 未被捕获")
    if not _REL_HOP.search('ROOT = HERE / "../.."'):
        st_bad("层级硬编码检测器失效：整串 \"../..\" 未被捕获")
    if _PARENTS_HOP.search(strip_comment('# 原写法 parents[3] 已删', ".py")):
        st_bad("层级硬编码检测器误报：注释行被当成代码")
    if _REL_HOP.search("'../../etc/passwd'"):
        st_bad("层级硬编码检测器误报：测试用例里的路径穿越样本字符串被误判")
    # 10) ⑪ 开发机绝对路径 ⇒ 必被捕获；正解写法 ⇒ 不得误报
    for bads in ('KEYS="/mnt/c/Users/Administrator/ttbox-image-keys"',
                 'REPO="/mnt/g/WORKBUDDY工作区/TTBOX-最终源码-2026-09-18"',
                 'PYEXE="C:/Users/Administrator/py/python.exe"',
                 'SRC="G:/WORKBUDDY工作区/x.img"'):
        if not _MACHINE_PATH.search(bads):
            st_bad("⑪ 开发机绝对路径检测器失效：%s 未被捕获" % bads)
    for clean in ('REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"',
                  'KEYS="${KEYS:-}"',
                  'IMG="${IMG_ROOT:-/mnt/img}"',
                  'M="/mnt/v5check"',
                  'STAGE="${TTBOX_STAGE_DIR:-/root/ttbox-image/_stage}"'):
        if _MACHINE_PATH.search(clean):
            st_bad("⑪ 开发机绝对路径检测器误报：正解写法被误判（%s）" % clean)
    if not _selftest_err:
        print("[gate][ OK ] --selftest 负向控制全部命中（篡改必被捕获、注释不误报）")


# ---------------------------------------------------------------------------
# 主流程
# ---------------------------------------------------------------------------
def main():
    check_literals()
    check_env()
    factory = load_json("deploy/config/00-factory.json")
    check_shared_keys(factory)
    check_crosslang()
    check_version()
    check_no_patch()
    check_no_abs_insert()
    check_project_root_injected()
    check_no_depth_hardcode()
    check_no_machine_paths()
    if SELFTEST:
        run_selftest(factory)

    for m in _ok:
        print("[gate][ OK ] " + m)
    for m in _bad:
        print("[gate][FAIL] " + m)
    for m in _selftest_err:
        print("[gate][FAIL][selftest] " + m)
    return 0 if (not _bad and not _selftest_err) else 1


if __name__ == "__main__":
    sys.exit(main())
PY
rc=$?

echo "------------------------------------------------------------"
if (( rc == 0 )); then
    echo "[gate] RESULT: PASS"
else
    echo "[gate] RESULT: FAIL"
fi
exit "$rc"
