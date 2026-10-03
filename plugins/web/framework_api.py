"""Web Plugin 的 Framework 兼容 API。

只做路由适配：所有插件安装/升级仍进入 PluginManager；系统能力由既有适配器提供。
"""
from __future__ import annotations
import os
import sys
from dataclasses import asdict, is_dataclass
from pathlib import Path
from flask import jsonify, request


# ── 路径 bootstrap（A-PATH-3）────────────────────────────────────────────────
# 本模块有两种加载方式：① web 进程内（sys.path 已就绪）；② 测试按文件 importlib 加载
# （sys.path 里没有本树）。这里自己保证「树根 + 本插件目录」在 sys.path **末尾**，
# 判据与 plugins/web/lib/paths.py::_ROOT_ANCHORS 同源，**不写死目录深度**
# （原写法靠调用方 parents[N] 先注入，改布局即静默指错根）。
def _ttbox_tree_root():
    cur = Path(__file__).resolve().parent
    while True:
        if all((cur / _n).is_dir() for _n in ("plugins", "framework", "scripts", "deploy")):
            return cur
        if cur.parent == cur:
            raise RuntimeError(
                "找不到 TTBOX 树根：从 %s 向上未发现同时含 "
                "plugins/framework/scripts/deploy 的目录" % __file__
            )
        cur = cur.parent


_TREE_ROOT = _ttbox_tree_root()
# append 到末尾（不用 insert(0)：顶到 stdlib 前会遮蔽同名标准库模块）
for _path_entry in (str(_TREE_ROOT), str(_TREE_ROOT / "plugins" / "web")):
    if _path_entry not in sys.path:
        sys.path.append(_path_entry)

from framework.plugin_manager import InstallRequest, InstallSource, LocalRepository, PluginManager
from framework.plugin_manager.models import PluginHealth, PluginState
from plugins.system_host import SystemPluginHost

# IPC socket 唯一真源（A-PATH-5）：TTBOX_IPC_SOCKET 环境变量 > lib/paths.py 默认。
# 本文件不再硬编码 "/run/ttbox/core.sock"（单一 Python 真源 = plugins/web/lib/paths.py）。
from plugins.web.lib import paths as _ttbox_paths

CORE_IPC_SOCKET = _ttbox_paths.ipc_socket()


# V-13：IPC 客户端**唯一实现** = lib/ipc.py（本处不再重复实现，避免超时/错误码/返回结构漂移）。
from plugins.web.lib import ipc as _ttbox_ipc


def _ipc_request(req_type: str, params: dict | None = None, timeout: float = 5) -> dict:
    """向 TTBOX Core IPC 发送请求（薄封装 → lib/ipc.py::request；socket = CORE_IPC_SOCKET）。"""
    return _ttbox_ipc.request(req_type, params, timeout, socket_path=CORE_IPC_SOCKET)


def _jsonable(value):
    if hasattr(value, "value"): return value.value
    if is_dataclass(value): return {k: _jsonable(v) for k, v in asdict(value).items()}
    if isinstance(value, dict): return {k: _jsonable(v) for k, v in value.items()}
    if isinstance(value, (list, tuple)): return [_jsonable(v) for v in value]
    return value


def install_framework_api(app):
    # 插件根默认值不再散写 FHS 字面量：走 lib/paths.py 的根锚派生（A-PATH-3）——
    # 开发机 = <repo>/plugins，板端 = <release 树>/plugins，两态同一判据。
    plugins_root = Path(os.environ.get("TTBOX_PLUGINS_ROOT", _ttbox_paths.plugins_dir()))
    repository_root = Path(os.environ.get("TTBOX_PLUGIN_REPOSITORY_ROOT", str(plugins_root / "repository")))
    manager = PluginManager(plugins_root=plugins_root)
    repository = LocalRepository(repository_root)
    system = SystemPluginHost.create()
    app.extensions["ttbox_plugin_manager"] = manager
    app.extensions["ttbox_plugin_repository"] = repository
    app.extensions["ttbox_system_plugins"] = system

    @app.get("/api/plugins")
    def plugin_list():
        manager.discover()
        # 实时状态同步：systemd 托管的 web/preview 已运行，registry 可能仍为 installed。
        # 用 HTTP 探活（进程级 is_running 只认自身 Popen，识别不了 systemd 托管进程）。
        import urllib.request
        _live = {}
        # 端口真源单点（V-06）：web 端口 = lib/paths.py::WEB_PORT_DEFAULT（跨语言同值）。
        for plugin_id, probe in (("web", f"http://127.0.0.1:{_ttbox_paths.WEB_PORT_DEFAULT}/api/health/frontend"),
                                 ("preview", "http://127.0.0.1:8001/api/preview/status")):
            try:
                with urllib.request.urlopen(probe, timeout=2) as resp:
                    _live[plugin_id] = 200 <= resp.status < 300
            except Exception:
                _live[plugin_id] = False
        for plugin_id, alive in _live.items():
            record = manager.get_plugin(plugin_id)
            if record is None:
                continue
            record.state = PluginState.RUNNING if alive else (PluginState.INSTALLED if record.state == PluginState.RUNNING else record.state)
            record.health = PluginHealth.HEALTHY if alive else (PluginHealth.UNKNOWN if record.state != PluginState.RUNNING else record.health)
        manager.save()
        return jsonify({"ok": True, "data": {"plugins": _jsonable(manager.get_installed_plugins())}})

    @app.get("/api/plugins/<plugin_id>")
    def plugin_detail(plugin_id):
        record = manager.get_plugin(plugin_id)
        if record is None: return jsonify({"ok": False, "error": "插件不存在"}), 404
        return jsonify({"ok": True, "data": _jsonable(record)})

    @app.get("/api/plugins/<plugin_id>/status")
    def plugin_status(plugin_id):
        try: return jsonify({"ok": True, "data": _jsonable(manager.get_plugin_status(plugin_id))})
        except KeyError: return jsonify({"ok": False, "error": "插件不存在"}), 404

    def _plugin_action(plugin_id, action):
        try: result = getattr(manager, action)(plugin_id)
        except Exception as exc: return jsonify({"ok": False, "error": str(exc)}), 400
        return jsonify({"ok": bool(result is not False), "data": _jsonable(result)})

    for action in ("enable", "disable", "start", "stop", "restart", "rollback", "uninstall"):
        endpoint = f"plugin_{action}"
        app.add_url_rule(f"/api/plugins/<plugin_id>/{action}", endpoint, lambda plugin_id, _action=action: _plugin_action(plugin_id, _action), methods=["POST"])

    @app.post("/api/plugins/install")
    def plugin_install():
        body = request.get_json(silent=True) or {}
        source = body.get("source", "local_file")
        try:
            request_model = InstallRequest(InstallSource(source), path=body.get("path"), url=body.get("url"), plugin_id=body.get("plugin_id"), version=body.get("version"), expected_sha256=body.get("sha256"))
            record = manager.install(request_model)
            return jsonify({"ok": True, "data": _jsonable(record)}), 201
        except Exception as exc: return jsonify({"ok": False, "error": str(exc)}), 400

    @app.post("/api/plugins/upgrade")
    def plugin_upgrade():
        body = request.get_json(silent=True) or {}
        try:
            source = InstallSource(body.get("source", "local_file"))
            request_model = InstallRequest(source, path=body.get("path"), url=body.get("url"), plugin_id=body.get("plugin_id"), version=body.get("version"), expected_sha256=body.get("sha256"))
            return jsonify({"ok": True, "data": _jsonable(manager.upgrade(request_model))})
        except Exception as exc: return jsonify({"ok": False, "error": str(exc)}), 400

    @app.get("/api/plugins/market")
    def plugin_market():
        query = request.args.get("q", "")
        entries = repository.search_plugins(query) if query else repository.list_plugins()
        return jsonify({"ok": True, "data": {"source": "local", "online": False, "plugins": _jsonable(entries)}})

    @app.get("/api/plugins/market/<plugin_id>")
    def market_plugin(plugin_id):
        entry = repository.get_plugin(plugin_id)
        if entry is None: return jsonify({"ok": False, "error": "市场插件不存在"}), 404
        return jsonify({"ok": True, "data": _jsonable(entry)})

    @app.get("/api/plugins/market/<plugin_id>/versions")
    def market_versions(plugin_id):
        return jsonify({"ok": True, "data": {"plugin_id": plugin_id, "versions": repository.get_versions(plugin_id)}})

    def _system_status(plugin_id):
        try: return jsonify({"ok": True, "data": _jsonable(system.service(plugin_id).status())})
        except KeyError: return jsonify({"ok": False, "error": "系统插件不存在"}), 404

    for plugin_id in ("system", "network", "wifi", "fan", "monitor", "upgrade"):
        endpoint = f"system_api_{plugin_id}"
        app.add_url_rule(f"/api/{plugin_id}/status", endpoint, lambda _plugin_id=plugin_id: _system_status(_plugin_id), methods=["GET"])

    @app.get("/api/core/status")
    def core_status():
        """真实 Core 状态（IPC GET_STATUS）。"""
        r = _ipc_request("GET_STATUS")
        if r.get("status") != 0:
            return jsonify({"ok": False, "error": r.get("error", "Core IPC 不可达")}), 503
        data = r.get("data", {})
        metrics = data.get("metrics", {}) if isinstance(data.get("metrics"), dict) else {}
        return jsonify({
            "ok": True,
            "data": {
                "source": "core_ipc",
                "available": True,
                "app_name": data.get("app_name"),
                "config_file": data.get("config_file"),
                "ipc_socket": data.get("ipc_socket"),
                "capture_fps": metrics.get("capture_fps"),
                "fps": metrics.get("fps"),
                "infer_ms": metrics.get("infer_ms"),
                "decode_ms": metrics.get("decode_ms"),
                "e2e_ms": metrics.get("e2e_ms"),
                "detect_count": metrics.get("detect_count"),
                "injection_allowed": metrics.get("injection_allowed"),
                "mouse_control_send_count": metrics.get("mouse_control_send_count"),
                "preview_fps": metrics.get("preview_fps"),
                "aim_has_target": metrics.get("aim_has_target"),
                "input_width": metrics.get("input_width"),
                "input_height": metrics.get("input_height"),
                # T1.15：输入通路诊断（与外层 /api/state 的 state.model_input 同源同义）
                "model_input_pass_mode": metrics.get("model_input_pass_mode"),
                "model_input_type_name": metrics.get("model_input_type_name"),
                "model_fast_path_active": metrics.get("model_fast_path_active"),
                "model_zero_copy_ready": metrics.get("model_zero_copy_ready"),
                "model_input_note": metrics.get("model_input_note"),
            },
        })

    @app.get("/api/model/list")
    def model_list():
        """真实模型列表（Core MODEL_LIST IPC）。"""
        r = _ipc_request("MODEL_LIST")
        if r.get("status") != 0:
            return jsonify({"ok": False, "error": r.get("error", "模型 IPC 不可达")}), 503
        d = r.get("data", {}) or {}
        models = []
        for m in d.get("models", []):
            models.append({
                "id": m.get("model_id"),
                "model_id": m.get("model_id"),
                "display_name": m.get("display_name") or m.get("model_id"),
                "backend": m.get("backend", "rknn"),
                "enabled": m.get("enabled", True),
            })
        return jsonify({"ok": True, "data": {"source": "model_plugin", "available": True, "models": models, "active": d.get("active", "")}})

    @app.get("/api/model/active")
    def model_active():
        """真实当前激活模型（Core MODEL_LIST IPC active 字段）。"""
        r = _ipc_request("MODEL_LIST")
        if r.get("status") != 0:
            return jsonify({"ok": False, "error": r.get("error", "模型 IPC 不可达")}), 503
        d = r.get("data", {}) or {}
        return jsonify({"ok": True, "data": {"source": "model_plugin", "available": True, "active": d.get("active", "")}})

    app.add_url_rule("/api/hwmon", "system_api_hwmon", lambda: _system_status("monitor"), methods=["GET"])
    return manager
