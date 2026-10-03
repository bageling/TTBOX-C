"""paths.py — TTBOX Web 运行期路径单点真源（A-PATH-5 / B-CONST-2）。

与 C++ core 侧 ``core/src/common/Paths.hpp`` **同值**；跨语言无法 include ⇒ 以
docs/protocols/config-path-env-registry.md 登记 + scripts/ttbox_conventions_gate.sh
同值断言防漂移。

取值链（A-PATH-2 / A-PATH-4）：
  · 运行根前缀统一经 ``TTBOX_PREFIX``（默认 ``/opt/ttbox``）派生；
  · **仓库/release 树内** 的根位置经「根锚发现」确定（A-PATH-3，见 ``discover_root``），
    在开发机（仓库根）与板端（release 树 current/）都成立，杜绝硬编码 FHS 绝对路径，
    也杜绝硬编码层级深度（``parents[N]``）。
"""
from __future__ import annotations

import os
from pathlib import Path

# ---------------------------------------------------------------------------
# 跨语言同值常量（与 Paths.hpp 逐值一致 —— 门禁断言；改动须同步登记表 §三）
# ---------------------------------------------------------------------------
IPC_SOCKET_DEFAULT = "/run/ttbox/core.sock"
MOUSE_CMD_SOCK_DEFAULT = "/run/ttbox-mouse-passthrough/cmd.sock"
MOUSE_EVENT_SOCK_DEFAULT = "/run/ttbox-mouse-passthrough/event.sock"

# 面板监听端口默认（B-CONST-1）：web LISTEN_PORT / 配网引导 URL / 验收脚本共用。
# 真源 = plugins/web/bin/ttbox-web.py::LISTEN_PORT（改端口改这里 + 该常量；门禁断言同值）。
WEB_PORT_DEFAULT = 8000

# 日志目录（批次 1.4）：与 C++ 侧 Paths.hpp::kLogDirDefault **同值**（门禁⑤跨语言同值面）。
# 目录由 scripts/ttbox_fhs_init.sh 创建（ttbox:ttbox 0755）。core 在此写 ttbox.log /
# ttbox-error.log / operation.log，web 写 web.log。目录不存在时两边的文件 sink 都自降级为
# 「只输出 stdout/journald」—— 日志设施坏掉不许拖垮启动。
LOG_DIR_DEFAULT = "/var/log/ttbox"

# FHS 运行根（A-PATH-4）：板端 = /opt/ttbox；env 可覆盖（联调/排障）。
_DEFAULT_PREFIX = "/opt/ttbox"


def ttbox_prefix() -> str:
    """运行根前缀（A-PATH-4）。默认 /opt/ttbox，env TTBOX_PREFIX 可覆盖。"""
    return os.environ.get("TTBOX_PREFIX", _DEFAULT_PREFIX)


def log_dir() -> str:
    """日志目录：``TTBOX_LOG_DIR`` > ``/var/log/ttbox``（批次 1.4，规矩 §5.2）。

    与 ``core/src/common/Paths.hpp::kLogDirDefault`` 同值。**刻意不挂在 TTBOX_PREFIX 下**：
    §5 的日志属运行期数据，FHS 归 ``/var/log``，不随版本目录（releases/V1.0.xx）走 ——
    否则每次 OTA 换版本，历史日志会跟着被换掉。
    """
    env = os.environ.get("TTBOX_LOG_DIR", "").strip()
    return env if env else LOG_DIR_DEFAULT


def join_path(base: str, *parts: str) -> str:
    """按 FHS 口径拼接路径（**恒用 ``/``**）。

    这些常量的目标平台是 Linux 板端；但开发机（Windows）也要读同一份值 ——
    ``os.path.join`` 在 Windows 上产出 ``\\``，同一个逻辑路径会出现两种字符串
    （日志、API 返回值、验收断言全受影响）。所以统一用 ``/``。
    """
    return "/".join([str(base).rstrip("/")] + [str(p).strip("/") for p in parts])


def prefix_path(*parts: str) -> str:
    """运行根下的绝对路径（A-PATH-4）：``<prefix>/<parts…>``。

    给"必须在 FHS 根下、且不在 release 树内"的落点用（state/run/venv-convert/
    tools/converter/calib…）。这些位置由装机镜像层创建，**不能**用 ``repo_root()``
    派生 —— 它们不在出货树里。散写 ``"/opt/ttbox/…"`` 会让 TTBOX_PREFIX 失效，
    这里收敛成一处。
    """
    return join_path(ttbox_prefix(), *parts)


# ---------------------------------------------------------------------------
# 根锚（A-PATH-3）—— 仓库 / release 树定位的**唯一**锚点机制
# ---------------------------------------------------------------------------
# 为什么不用 `Path(__file__).resolve().parents[N]`：
#   ① 层级深度是硬编码常量。搬一次目录（plugins/web → board/src/web）就静默指错根，
#      而错根不会报错，只会在某个下游功能上表现为"文件找不到"；
#   ② 把 parents[3] 改成 parents[4] 只是把错的深度换成另一个错的深度，不是修复；
#   ③ 开发机（仓库根）与板端（releases/<ver>/）允许有不同深度，硬编码必然二选一错。
# 做法：从本文件所在目录出发**逐级向上**找"同时含下列全部顶层目录"的那一层。
#   仓库根   = {plugins, framework, scripts, deploy}
#   release 树（板端 current/）顶层同样是这 4 项（见 deploy/pack_manifest.txt 顶层闭集）
#   ⇒ 同一套判据覆盖两态，无需分支、无需 env。子目录（plugins/web/lib、plugins/web/bin、
#     scripts/…）都不满足 ⇒ 解唯一；取「最近的一层」⇒ 不会误命中更上层的 /opt/ttbox。
_ROOT_ANCHORS = ("plugins", "framework", "scripts", "deploy")


def discover_root(start) -> str:
    """从 ``start`` 逐级向上找主根（最近一层同时含全部 ``_ROOT_ANCHORS`` 的目录）。

    找不到就抛 ``RuntimeError`` —— **不允许静默回退**（回退到 ``.`` 或某一上层目录都会
    指错根，而且无声；宁可在调用点炸掉，也不要让下游读错文件）。
    """
    cur = Path(start).resolve()
    while True:
        if all((cur / name).is_dir() for name in _ROOT_ANCHORS):
            return str(cur)
        parent = cur.parent
        if parent == cur:          # 已经到文件系统根，仍无锚
            raise RuntimeError(
                "找不到 TTBOX 主根：从 %s 向上逐级未发现同时含 %s 的目录"
                % (start, " / ".join(_ROOT_ANCHORS))
            )
        cur = parent


def repo_root() -> str:
    """仓库 / release 树根（A-PATH-3 根锚发现）。

    开发机 <root> = 仓库根；板端 <root> = release 树（``current/``）。
    两态用同一条判据，**不依赖本文件在树里的层级深度**。
    """
    return discover_root(Path(__file__).resolve().parent)


def scripts_dir() -> str:
    """scripts 目录：TTBOX_SCRIPTS_DIR > <repo_root>/scripts。

    用相对派生而非 FHS 绝对路径：秒级联调机（仓库根）与板端（release tree）都成立；
    ``wifi_manager`` / ``edid`` 工具链即在此目录。
    """
    env = os.environ.get("TTBOX_SCRIPTS_DIR")
    if env:
        return env
    return str(Path(repo_root()) / "scripts")


def plugins_dir() -> str:
    """插件根目录（A-PATH-3）：TTBOX_PLUGINS_ROOT > <repo_root>/plugins。

    用相对派生而非 FHS 绝对路径：板端插件树随 release 走（``current/plugins``），
    与 ``/opt/ttbox/plugins`` 那条**过渡期软链**不是同一件事 —— 前者才是终态
    （DEP-06：全部运行树进 release）。开发机 = 仓库根下 ``plugins``。
    """
    env = os.environ.get("TTBOX_PLUGINS_ROOT")
    if env:
        return env
    return str(Path(repo_root()) / "plugins")


def web_dir() -> str:
    """本插件目录（plugins/web）：``lib`` 的父目录。

    这是**包内结构**（lib 是 web 的子包），不是仓库层级假设 —— 与
    ``parents[N]`` 那类"往上跳 N 层找仓库根"有本质区别：改仓库布局不影响它。
    """
    return str(Path(__file__).resolve().parent.parent)


def models_root() -> str:
    """模型库根（V-04）：TTBOX_MODELS_ROOT > <prefix>/models。

    单一真源：web 的模型元数据（installed/<id>/ui_meta.json）、转换产物落点
    （`_incoming`）、上传落点一律经此派生，杜绝散写绝对路径。
    原同义异名（无尾 ``s`` 的变体）已删除，**不保留兼容读**；门禁③扫描生产代码时亦须绝迹。
    """
    return os.environ.get("TTBOX_MODELS_ROOT", ttbox_prefix() + "/models")


def config_dir() -> str:
    """运行期配置目录：TTBOX_CONFIG_DIR > <prefix>/config。"""
    return os.environ.get("TTBOX_CONFIG_DIR", ttbox_prefix() + "/config")


def presets_dir() -> str:
    """Web 预设目录：TTBOX_PRESETS_DIR > <prefix>/presets。"""
    return os.environ.get("TTBOX_PRESETS_DIR", ttbox_prefix() + "/presets")


def web_credentials_file() -> str:
    """Web 云端凭据文件：TTBOX_WEB_CREDENTIALS > <config_dir>/default.json。

    ★ 这是 **web 拥有的部署凭据文件**（cloud.app_secret / license_base_url），
    **不是**运行期配置真源（运行期配置 = Core，经 IPC）。历史命名 default.json 保留
    只为与部署/验收脚本（scripts/ttbox_m207_accept.py）写入路径一致。
    """
    return os.environ.get(
        "TTBOX_WEB_CREDENTIALS", config_dir() + "/default.json"
    )


def hdmirx_edid_tool() -> str:
    """TTBOX 自有 EDID 工具：TTBOX_HDMIRX_EDID > <scripts_dir>/edid/hdmirx_edid.py。"""
    return os.environ.get(
        "TTBOX_HDMIRX_EDID", scripts_dir() + "/edid/hdmirx_edid.py"
    )


def motion_profiles_dir() -> str:
    """动作曲线目录：TTBOX_MOTION_PROFILES_DIR > <prefix>/config/motion-profiles。"""
    return os.environ.get(
        "TTBOX_MOTION_PROFILES_DIR", ttbox_prefix() + "/config/motion-profiles"
    )


def ipc_socket() -> str:
    """IPC socket：TTBOX_IPC_SOCKET > IPC_SOCKET_DEFAULT。"""
    return os.environ.get("TTBOX_IPC_SOCKET", IPC_SOCKET_DEFAULT)
