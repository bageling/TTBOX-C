from __future__ import annotations

import hashlib
import json
import os
import re
import shutil
import subprocess
import tempfile
from pathlib import Path
from typing import Any


# ── 路径 bootstrap（A-PATH-3）────────────────────────────────────────────────
# 树根用**锚点发现**确定，不写死目录深度（原 `parents[1]`：换布局即静默指错根）。
def _ttbox_tree_root() -> Path:
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
_WEB_DIR = _TREE_ROOT / "plugins" / "web"


DEFAULT_PASSWORD = os.environ.get("TTBOX_WIFI_DEFAULT_PASSWORD", "12345678")
_default_ssids_raw = os.environ.get("TTBOX_WIFI_DEFAULT_SSIDS")
if _default_ssids_raw is None:
    _default_ssids_raw = os.environ.get("TTBOX_WIFI_DEFAULT_SSID", "TTBOX")
DEFAULT_SSIDS = [item.strip() for item in re.split(r"[\s,]+", _default_ssids_raw) if item.strip()] or ["TTBOX", "TTBOX-5G"]
DEFAULT_SSID = DEFAULT_SSIDS[0]
DEFAULT_CONNECTION_PREFIX = os.environ.get("TTBOX_WIFI_DEFAULT_CONNECTION_PREFIX", "ttbox-default-")
USER_CONNECTION_PREFIX = os.environ.get("TTBOX_WIFI_USER_CONNECTION_PREFIX", "ttbox-wifi-")
AP_CONNECTION = os.environ.get("TTBOX_WIFI_AP_CONNECTION", "ttbox-ap-hotspot")
WIFI_BOOTSTRAP_SERVICE = os.environ.get("TTBOX_WIFI_BOOTSTRAP_SERVICE", "ttbox-wifi-bootstrap.service")
# 面板端口默认（B-CONST-1 / V-06）：跨语言同值真源 = plugins/web/lib/paths.py::WEB_PORT_DEFAULT，
# 门禁 scripts/ttbox_conventions_gate.sh 断言本值与之一致。运行时覆盖名 = TTBOX_WEB_PORT（**唯一名**；
# 旧 TTBOX_DEFAULT_WEB_PORT / TTBOX_PORT 属同义异名，已删除，不保留兼容读 —— D-ENV-3/D-ENV-5）。
WEB_PORT_DEFAULT = 8000


# ---- M2.04：品牌表 ↔ 广播 SSID 一致性自检钩子（诊断用；不抛异常、不改状态）----
# 背景：面板展示的热点名来自 plugins/web/config/ui_brands.json 的 default_hotspot_ssid，
#   但板子**实际广播**的 SSID 是本文件的 DEFAULT_SSIDS（源 = TTBOX_WIFI_DEFAULT_SSIDS）。
#   两者是**两个真源**，渠道白标上线时必须一起改；本钩子把"漂移"显性化，供渠道自检/开机巡检。
def brand_registry_ssids() -> list[str]:
    """读取品牌注册表里所有 `default_hotspot_ssid`（文件缺失/损坏 ⇒ 返回 []）。"""
    cfg = _WEB_DIR / "config" / "ui_brands.json"
    try:
        raw = json.loads(cfg.read_text(encoding="utf-8"))
    except (OSError, ValueError):
        return []
    brands = raw.get("brands") if isinstance(raw, dict) else None
    if not isinstance(brands, dict):
        return []
    out: list[str] = []
    for entry in brands.values():
        if isinstance(entry, dict):
            ssid = entry.get("default_hotspot_ssid")
            if isinstance(ssid, str) and ssid.strip():
                out.append(ssid.strip())
    return out


def verify_brand_ssid_consistency() -> dict[str, Any]:
    """★ M2.04 自检钩子：品牌表声明的热点名必须 ⊆ 本机实际会广播的 DEFAULT_SSIDS。

    返回 `{ok, missing, default_ssids, brand_ssids}`：
      · `missing` = 品牌表声明、但本机**不会广播**的名字（正是"面板显示渠道名、
        板子仍广播 TTBOX"的病灶）。
      · 仅诊断：不抛异常、不改状态 —— 板端只打 WARN，绝不阻断开机。
    """
    brand_ssids = brand_registry_ssids()
    missing = [s for s in brand_ssids if s not in DEFAULT_SSIDS]
    return {
        "ok": not missing,
        "missing": missing,
        "default_ssids": list(DEFAULT_SSIDS),
        "brand_ssids": brand_ssids,
    }


class WifiError(RuntimeError):
    def __init__(self, message: str, payload: dict[str, Any] | None = None):
        super().__init__(message)
        self.payload = payload or {}


def _nmcli_path() -> str | None:
    return shutil.which("nmcli")


def _web_port() -> int:
    """面板端口：TTBOX_WEB_PORT（唯一名，D-ENV-3）> WEB_PORT_DEFAULT。"""
    try:
        port = int(os.environ.get("TTBOX_WEB_PORT", "") or WEB_PORT_DEFAULT)
    except (TypeError, ValueError):
        return WEB_PORT_DEFAULT
    return port if 1 <= port <= 65535 else WEB_PORT_DEFAULT


def _web_url(host: str) -> str:
    return f"http://{host}:{_web_port()}/"


def _nmcli_split(line: str) -> list[str]:
    fields: list[str] = []
    buf: list[str] = []
    escaped = False
    for char in line.rstrip("\n"):
        if escaped:
            buf.append(char)
            escaped = False
        elif char == "\\":
            escaped = True
        elif char == ":":
            fields.append("".join(buf))
            buf = []
        else:
            buf.append(char)
    if escaped:
        buf.append("\\")
    fields.append("".join(buf))
    return fields


def _run_nmcli(args: list[str], timeout: int = 15, check: bool = False) -> subprocess.CompletedProcess[str]:
    nmcli = _nmcli_path()
    if not nmcli:
        raise WifiError("NetworkManager nmcli 不可用，请先安装 network-manager")
    env = os.environ.copy()
    env["LANG"] = "C.UTF-8"
    env["LC_ALL"] = "C.UTF-8"
    try:
        completed = subprocess.run(
            [nmcli, *args],
            text=True,
            capture_output=True,
            check=False,
            timeout=timeout,
            env=env,
        )
    except subprocess.TimeoutExpired as exc:
        raise WifiError("Wi-Fi 操作超时") from exc
    if check and completed.returncode != 0:
        raise WifiError(_nmcli_error(completed))
    return completed


def _connection_up(name: str, iface: str, timeout: int = 35, password: str = "") -> None:
    args = ["--wait", str(max(1, timeout - 5)), "connection", "up", name, "ifname", iface]
    password_file = ""
    if password:
        fd, password_file = tempfile.mkstemp(prefix="ttbox-nmcli-", text=True)
        try:
            os.fchmod(fd, 0o600)
            with os.fdopen(fd, "w", encoding="utf-8") as handle:
                handle.write(f"802-11-wireless-security.psk:{password}\n")
            args.extend(["passwd-file", password_file])
            _run_nmcli(args, timeout=timeout, check=True)
        finally:
            if password_file:
                try:
                    os.unlink(password_file)
                except OSError:
                    pass
        return
    _run_nmcli(args, timeout=timeout, check=True)


def _default_connection_name(ssid: str) -> str:
    legacy = os.environ.get("TTBOX_WIFI_DEFAULT_CONNECTION", "").strip()
    if legacy and ssid == DEFAULT_SSID:
        return legacy
    safe = re.sub(r"[^A-Za-z0-9_.-]+", "-", ssid).strip("-") or "network"
    return f"{DEFAULT_CONNECTION_PREFIX}{safe[:48]}"


def _nmcli_error(completed: subprocess.CompletedProcess[str]) -> str:
    raw = (completed.stderr or completed.stdout or "").strip()
    raw = re.sub(r"\s+", " ", raw)
    return raw or "Wi-Fi 操作失败"


def _systemctl(*args: str) -> None:
    if not shutil.which("systemctl"):
        return
    subprocess.run(["systemctl", *args], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, check=False)


def _stop_wifi_bootstrap() -> None:
    _systemctl("stop", WIFI_BOOTSTRAP_SERVICE)


def _start_wifi_bootstrap() -> None:
    _systemctl("start", WIFI_BOOTSTRAP_SERVICE)


def _wireless_interfaces() -> list[str]:
    sysfs_interfaces = [path.name for path in sorted(Path("/sys/class/net").glob("*")) if (path / "wireless").is_dir()]
    if not _nmcli_path():
        return _sort_wireless_interfaces(sysfs_interfaces, {})

    nmcli_interfaces: list[str] = []
    states: dict[str, str] = {}
    completed = _run_nmcli(["-t", "-f", "DEVICE,TYPE,STATE", "device", "status"], check=False)
    for line in completed.stdout.splitlines():
        fields = _nmcli_split(line)
        if len(fields) >= 3 and fields[1] == "wifi":
            iface = fields[0]
            nmcli_interfaces.append(iface)
            states[iface] = fields[2].strip().lower()
    for iface in sysfs_interfaces:
        if iface not in nmcli_interfaces:
            nmcli_interfaces.append(iface)
    return _sort_wireless_interfaces(nmcli_interfaces, states)


def _wifi_state_rank(state: str) -> int:
    state = state.strip().lower()
    if state == "connected":
        return 0
    if state in {"connecting", "preparing", "configuring", "need-auth", "ip-config", "ip-check", "secondaries"}:
        return 1
    if state == "disconnected":
        return 2
    if state in {"unavailable", "unmanaged"}:
        return 8
    if state == "failed":
        return 9
    return 5


def _sort_wireless_interfaces(interfaces: list[str], states: dict[str, str]) -> list[str]:
    seen: set[str] = set()
    unique = [iface for iface in interfaces if iface and not (iface in seen or seen.add(iface))]
    return sorted(unique, key=lambda iface: (_wifi_state_rank(states.get(iface, "")), iface.startswith("p2p"), iface))


def _ethernet_connected() -> bool:
    for path in sorted(Path("/sys/class/net").glob("*")):
        name = path.name
        if name == "lo" or (path / "wireless").is_dir():
            continue
        carrier_path = path / "carrier"
        if not carrier_path.exists():
            continue
        try:
            if carrier_path.read_text(encoding="utf-8").strip() == "1":
                return True
        except OSError:
            continue
    return False


def _first_wireless_interface() -> str:
    interfaces = _wireless_interfaces()
    if not interfaces:
        raise WifiError("未检测到无线网卡")
    return interfaces[0]


def _prepare_wifi_device(iface: str) -> None:
    if shutil.which("rfkill"):
        subprocess.run(["rfkill", "unblock", "wifi"], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, check=False)
    _run_nmcli(["radio", "wifi", "on"], check=False)
    _run_nmcli(["device", "set", iface, "managed", "yes"], check=False)
    if shutil.which("ip"):
        subprocess.run(["ip", "link", "set", iface, "up"], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, check=False)


def _connection_exists(name: str) -> bool:
    return _run_nmcli(["connection", "show", name], check=False).returncode == 0


def _connection_value(name: str, field: str) -> str:
    completed = _run_nmcli(["-g", field, "connection", "show", name], check=False)
    if completed.returncode != 0:
        return ""
    return completed.stdout.strip().splitlines()[0] if completed.stdout.strip() else ""


def _connection_autoconnect(name: str) -> bool:
    return _connection_value(name, "connection.autoconnect").strip().lower() == "yes"


def _connection_mode(name: str) -> str:
    return _connection_value(name, "802-11-wireless.mode").strip().lower()


def _wifi_connection_names() -> list[str]:
    names: list[str] = []
    completed = _run_nmcli(["-t", "-f", "NAME,TYPE", "connection", "show"], check=False)
    if completed.returncode != 0:
        return names
    for line in completed.stdout.splitlines():
        fields = _nmcli_split(line)
        if len(fields) >= 2 and fields[1] in {"802-11-wireless", "wifi"}:
            names.append(fields[0])
    return names


def _delete_saved_wifi_connections(except_name: str = "") -> None:
    default_connections = {_default_connection_name(ssid) for ssid in DEFAULT_SSIDS}
    for name in _wifi_connection_names():
        if name in default_connections or name == AP_CONNECTION or (except_name and name == except_name):
            continue
        _delete_connection(name)


def _delete_wifi_connections_for_ssid(ssid: str, except_name: str = "") -> None:
    default_connections = {_default_connection_name(default_ssid) for default_ssid in DEFAULT_SSIDS}
    for name in _wifi_connection_names():
        if name in default_connections or name == AP_CONNECTION or (except_name and name == except_name):
            continue
        if name == ssid or _connection_ssid(name) == ssid:
            _delete_connection(name)


def _delete_connection(name: str) -> None:
    if _connection_exists(name):
        _run_nmcli(["connection", "delete", name], check=False)


def _connection_ssid(name: str) -> str:
    return _connection_value(name, "802-11-wireless.ssid")


def _saved_wifi_ssids() -> set[str]:
    saved: set[str] = set()
    completed = _run_nmcli(["-t", "-f", "NAME,TYPE", "connection", "show"], check=False)
    if completed.returncode != 0:
        return saved
    for line in completed.stdout.splitlines():
        fields = _nmcli_split(line)
        if len(fields) < 2 or fields[1] not in {"802-11-wireless", "wifi"}:
            continue
        if fields[0] == AP_CONNECTION:
            continue
        ssid = _connection_ssid(fields[0])
        if ssid:
            saved.add(ssid)
    return saved


def _ip4_for_interface(iface: str) -> str:
    completed = _run_nmcli(["-g", "IP4.ADDRESS", "device", "show", iface], check=False)
    if completed.returncode != 0:
        return ""
    for line in completed.stdout.splitlines():
        value = line.strip()
        if value:
            return value.split("/", 1)[0]
    return ""


def _scan_networks(iface: str, force_scan: bool = False) -> list[dict[str, Any]]:
    rescan = "yes" if force_scan else "no"
    completed = _run_nmcli(
        ["-t", "-f", "SSID,SIGNAL,SECURITY,IN-USE", "device", "wifi", "list", "ifname", iface, "--rescan", rescan],
        timeout=20 if force_scan else 10,
        check=False,
    )
    if completed.returncode != 0:
        raise WifiError(_nmcli_error(completed))

    saved = _saved_wifi_ssids()
    by_ssid: dict[str, dict[str, Any]] = {}
    for line in completed.stdout.splitlines():
        fields = _nmcli_split(line)
        if len(fields) < 4:
            continue
        ssid = fields[0].strip()
        if not ssid:
            continue
        try:
            signal = int(fields[1])
        except ValueError:
            signal = 0
        entry = by_ssid.get(ssid)
        active = fields[3].strip() == "*"
        security = fields[2].strip() or "open"
        if entry is None or signal > int(entry.get("signal", 0)):
            by_ssid[ssid] = {
                "ssid": ssid,
                "signal": signal,
                "security": security,
                "saved": ssid in saved,
                "active": active,
            }
        elif active:
            entry["active"] = True
    return sorted(by_ssid.values(), key=lambda item: (not item.get("active"), -int(item.get("signal", 0)), item["ssid"]))


def _connected_wifi() -> dict[str, Any] | None:
    completed = _run_nmcli(["-t", "-f", "DEVICE,TYPE,STATE,CONNECTION", "device", "status"], check=False)
    if completed.returncode != 0:
        return None
    for line in completed.stdout.splitlines():
        fields = _nmcli_split(line)
        if len(fields) < 4:
            continue
        iface, conn_type, state, connection = fields[:4]
        if conn_type != "wifi" or state != "connected":
            continue
        mode = _connection_mode(connection)
        ssid = _connection_ssid(connection) or connection
        return {
            "interface": iface,
            "ssid": ssid,
            "connection": connection,
            "ip4": _ip4_for_interface(iface),
            "signal": None,
            "mode": "ap" if mode == "ap" or connection == AP_CONNECTION else "client",
        }
    return None


def _disconnect_wifi_devices() -> None:
    for iface in _wireless_interfaces():
        _run_nmcli(["device", "disconnect", iface], timeout=8, check=False)


def _ap_support_status(iface: str = "") -> dict[str, Any]:
    status: dict[str, Any] = {
        "supported": False,
        "checked": False,
        "reason": "",
        "interface": iface,
    }
    if not _nmcli_path():
        status["reason"] = "NetworkManager nmcli 不可用"
        return status
    if not iface:
        status["reason"] = "未检测到无线网卡"
        return status
    iw = shutil.which("iw")
    if not iw:
        status["reason"] = "iw 不可用，无法确认无线网卡是否支持 AP 模式"
        return status
    try:
        completed = subprocess.run([iw, "list"], text=True, capture_output=True, check=False, timeout=8)
    except subprocess.TimeoutExpired:
        status["reason"] = "检测 AP 支持超时"
        return status
    status["checked"] = True
    if completed.returncode != 0:
        status["reason"] = re.sub(r"\s+", " ", (completed.stderr or completed.stdout or "").strip()) or "iw list 执行失败"
        return status
    if re.search(r"(?m)^\s*\*\s*AP\s*$", completed.stdout):
        status["supported"] = True
        return status
    status["reason"] = "当前无线网卡未声明支持 AP 模式"
    return status


def _set_connection_autoconnect(name: str, enabled: bool) -> None:
    if _connection_exists(name):
        _run_nmcli(["connection", "modify", name, "connection.autoconnect", "yes" if enabled else "no"], check=False)


def _set_client_connections_autoconnect(enabled: bool, except_name: str = "") -> None:
    for name in _wifi_connection_names():
        if name == AP_CONNECTION or (except_name and name == except_name):
            continue
        _set_connection_autoconnect(name, enabled)


def _disable_ap_connection() -> None:
    if _connection_exists(AP_CONNECTION):
        _run_nmcli(["connection", "modify", AP_CONNECTION, "connection.autoconnect", "no"], check=False)
        _run_nmcli(["connection", "down", AP_CONNECTION], timeout=10, check=False)


def _ap_status(iface: str = "", connected: dict[str, Any] | None = None) -> dict[str, Any]:
    support = _ap_support_status(iface)
    exists = _connection_exists(AP_CONNECTION) if _nmcli_path() else False
    ssid = _connection_ssid(AP_CONNECTION) if exists else DEFAULT_SSID
    active = bool(connected and connected.get("connection") == AP_CONNECTION and connected.get("mode") == "ap")
    ip4 = str(connected.get("ip4") or "") if active and connected else ""
    return {
        "supported": bool(support.get("supported")),
        "checked": bool(support.get("checked")),
        "support_error": str(support.get("reason") or ""),
        "connection": AP_CONNECTION,
        "configured": exists,
        "autoconnect": _connection_autoconnect(AP_CONNECTION) if exists else False,
        "active": active,
        "ssid": ssid or DEFAULT_SSID,
        "ip4": ip4,
        "gateway_url": _web_url(ip4) if ip4 else _web_url("10.42.0.1"),
        "mdns_url": _web_url("ttbox.local"),
        "default_ssid": DEFAULT_SSID,
        "default_ssids": DEFAULT_SSIDS,
        "default_password": DEFAULT_PASSWORD,
    }


def wifi_status(force_scan: bool = False) -> dict[str, Any]:
    payload: dict[str, Any] = {
        "available": False,
        "manager": "networkmanager",
        "mode": "client",
        "interface": "",
        "interfaces": [],
        "ethernet_connected": _ethernet_connected(),
        "connected": None,
        "ap": {
            "supported": False,
            "checked": False,
            "support_error": "",
            "connection": AP_CONNECTION,
            "configured": False,
            "autoconnect": False,
            "active": False,
            "ssid": DEFAULT_SSID,
            "ip4": "",
            "gateway_url": _web_url("10.42.0.1"),
            "mdns_url": _web_url("ttbox.local"),
            "default_ssid": DEFAULT_SSID,
            "default_ssids": DEFAULT_SSIDS,
            "default_password": DEFAULT_PASSWORD,
        },
        "default_ssid": DEFAULT_SSID,
        "default_ssids": DEFAULT_SSIDS,
        "networks": [],
        "error": "",
    }
    if not _nmcli_path():
        payload["error"] = "NetworkManager nmcli 不可用"
        return payload

    try:
        interfaces = _wireless_interfaces()
        payload["interfaces"] = interfaces
        payload["available"] = bool(interfaces)
        if not interfaces:
            payload["error"] = "未检测到无线网卡"
            return payload
        iface = interfaces[0]
        payload["interface"] = iface
        connected = _connected_wifi()
        ap = _ap_status(iface, connected)
        payload["ap"] = ap
        payload["mode"] = "ap" if ap.get("active") else "client"
        networks: list[dict[str, Any]] = []
        if payload["mode"] != "ap":
            networks = _scan_networks(iface, force_scan=force_scan)
        if connected:
            for network in networks:
                if network.get("ssid") == connected.get("ssid"):
                    connected["signal"] = network.get("signal")
                    network["active"] = True
                    break
        payload["connected"] = connected
        payload["networks"] = networks
    except WifiError as exc:
        payload["error"] = str(exc)
    return payload


def ensure_default_connection(ssid: str = DEFAULT_SSID, priority: int = 30) -> str:
    connection = _default_connection_name(ssid)
    if not _connection_exists(connection):
        _run_nmcli(
            [
                "connection",
                "add",
                "type",
                "wifi",
                "ifname",
                "*",
                "con-name",
                connection,
                "ssid",
                ssid,
                "ipv4.method",
                "auto",
                "ipv6.method",
                "ignore",
            ],
            check=True,
        )
    _run_nmcli(
        [
            "connection",
            "modify",
            connection,
            "connection.autoconnect",
            "yes",
            "connection.autoconnect-priority",
            str(priority),
            "802-11-wireless.ssid",
            ssid,
            "wifi-sec.key-mgmt",
            "wpa-psk",
            "wifi-sec.psk-flags",
            "0",
            "wifi-sec.psk",
            DEFAULT_PASSWORD,
            "ipv4.method",
            "auto",
            "ipv6.method",
            "ignore",
        ],
        check=True,
    )
    return connection


def ensure_default_connections() -> list[str]:
    connections: list[str] = []
    for index, ssid in enumerate(DEFAULT_SSIDS):
        connections.append(ensure_default_connection(ssid, 30 - index))
    return connections


def connect_default_wifi() -> dict[str, Any]:
    iface = _first_wireless_interface()
    _stop_wifi_bootstrap()
    _prepare_wifi_device(iface)
    _disable_ap_connection()
    ensure_default_connections()
    try:
        last_error: WifiError | None = None
        for ssid in DEFAULT_SSIDS:
            try:
                _connection_up(_default_connection_name(ssid), iface, timeout=35, password=DEFAULT_PASSWORD)
                return wifi_status(force_scan=False)
            except WifiError as exc:
                last_error = exc
        raise WifiError(f"默认 Wi-Fi 均连接失败（{', '.join(DEFAULT_SSIDS)}）: {last_error or 'unknown error'}")
    finally:
        _start_wifi_bootstrap()


def reset_to_default_wifi() -> dict[str, Any]:
    iface = _first_wireless_interface()
    _stop_wifi_bootstrap()
    _prepare_wifi_device(iface)
    _disable_ap_connection()
    ensure_default_connections()
    _delete_saved_wifi_connections()
    _disconnect_wifi_devices()
    try:
        last_error: WifiError | None = None
        for ssid in DEFAULT_SSIDS:
            try:
                _connection_up(_default_connection_name(ssid), iface, timeout=35, password=DEFAULT_PASSWORD)
                return wifi_status(force_scan=False)
            except WifiError as exc:
                last_error = exc
        raise WifiError(f"默认 Wi-Fi 均连接失败（{', '.join(DEFAULT_SSIDS)}）: {last_error or 'unknown error'}")
    finally:
        _start_wifi_bootstrap()


def _user_connection_name(ssid: str) -> str:
    safe = re.sub(r"[^A-Za-z0-9_.-]+", "-", ssid).strip("-") or "network"
    digest = hashlib.sha1(ssid.encode("utf-8")).hexdigest()[:8]
    return f"{USER_CONNECTION_PREFIX}{safe[:40]}-{digest}"


def _fallback_result() -> dict[str, Any]:
    result: dict[str, Any] = {"attempted": True, "connected": False, "ssid": DEFAULT_SSID, "ssids": DEFAULT_SSIDS}
    try:
        status = connect_default_wifi()
        connected = status.get("connected") or {}
        result["connected"] = connected.get("ssid") in DEFAULT_SSIDS
        result["ssid"] = connected.get("ssid") or DEFAULT_SSID
        result["status"] = status
    except WifiError as exc:
        result["error"] = str(exc)
    return result


def _write_psk_file(password: str) -> str:
    """把 PSK 写进 0600 临时 passwd-file（nmcli passwd-file 消费）。
    ★ 2026-09-26（第四轮审计）：密码放 argv 会经 /proc/<pid>/cmdline 泄漏给
    同机所有本地用户（--wait 期间存活最长 40s）。全部改走 passwd-file。"""
    fd, path = tempfile.mkstemp(prefix="ttbox-nmcli-psk-", text=True)
    os.fchmod(fd, 0o600)
    with os.fdopen(fd, "w", encoding="utf-8") as handle:
        handle.write(f"802-11-wireless-security.psk:{password}\n")
    return path


def connect_wifi(ssid: str, password: str = "") -> dict[str, Any]:
    ssid = str(ssid or "").strip()
    password = str(password or "")
    if not ssid:
        raise WifiError("请选择 Wi-Fi")
    if "\n" in password or "\r" in password:
        raise WifiError("Wi-Fi 密码不能包含换行")
    if password and len(password) < 8:
        raise WifiError("Wi-Fi 密码至少 8 位")
    if ssid in DEFAULT_SSIDS:
        return reset_to_default_wifi()

    iface = _first_wireless_interface()
    _stop_wifi_bootstrap()
    _prepare_wifi_device(iface)
    _disable_ap_connection()
    connection = _user_connection_name(ssid)
    _delete_connection(connection)
    _delete_wifi_connections_for_ssid(ssid)
    # ★ 密码经 passwd-file 下发，不再出现在 argv（/proc/cmdline 可读）
    psk_file = _write_psk_file(password) if password else ""
    try:
        args = ["--wait", "40", "device", "wifi", "connect", ssid, "ifname", iface, "name", connection]
        if psk_file:
            args.extend(["passwd-file", psk_file])
        _run_nmcli(args, timeout=45, check=True)
        _run_nmcli(
            [
                "connection",
                "modify",
                connection,
                "connection.autoconnect",
                "yes",
                "connection.autoconnect-priority",
                "100",
                "ipv4.method",
                "auto",
                "ipv6.method",
                "ignore",
            ],
            check=False,
        )
        _delete_saved_wifi_connections(except_name=connection)
        return wifi_status(force_scan=False)
    except WifiError as exc:
        _delete_connection(connection)
        fallback = _fallback_result()
        raise WifiError(f"连接 {ssid} 失败，已尝试回到默认 Wi-Fi（{', '.join(DEFAULT_SSIDS)}）: {exc}", {"fallback": fallback}) from exc
    finally:
        if psk_file:
            try:
                os.unlink(psk_file)
            except OSError:
                pass
        _start_wifi_bootstrap()


def _validate_ap_credentials(ssid: Any, password: Any) -> tuple[str, str]:
    resolved_ssid = DEFAULT_SSID if ssid is None else str(ssid).strip()
    resolved_password = DEFAULT_PASSWORD if password is None else str(password)
    ssid_bytes = resolved_ssid.encode("utf-8")
    if not ssid_bytes:
        raise WifiError("热点名称不能为空")
    if len(ssid_bytes) > 32:
        raise WifiError("热点名称最多 32 字节")
    if len(resolved_password) < 8 or len(resolved_password) > 63:
        raise WifiError("热点密码必须为 8-63 位")
    return resolved_ssid, resolved_password


def _ensure_ap_connection(ssid: str, password: str) -> None:
    if not _connection_exists(AP_CONNECTION):
        _run_nmcli(
            [
                "connection",
                "add",
                "type",
                "wifi",
                "ifname",
                "*",
                "con-name",
                AP_CONNECTION,
                "ssid",
                ssid,
                "ipv6.method",
                "ignore",
            ],
            check=True,
        )
    _run_nmcli(
        [
            "connection",
            "modify",
            AP_CONNECTION,
            "connection.autoconnect",
            "yes",
            "connection.autoconnect-priority",
            "200",
            "802-11-wireless.mode",
            "ap",
            "802-11-wireless.ssid",
            ssid,
            "wifi-sec.key-mgmt",
            "wpa-psk",
            "wifi-sec.psk-flags",
            "0",
            "wifi-sec.psk",
            # ★ 残留说明（第四轮审计）：nmcli connection modify 无 passwd-file 机制，
            #   PSK 经 argv 短暂暴露是 nmcli 的平台限制；热点密码本就是设备本机的
            #   /etc/NetworkManager/system-connections 明文存量（0600），风险边界不变。
            # 用户自设的 client 密码已在 connect_wifi 改走 passwd-file。
            password,
            "ipv4.method",
            "shared",
            "ipv6.method",
            "ignore",
        ],
        check=True,
    )


def apply_ap_hotspot(ssid: Any = None, password: Any = None) -> dict[str, Any]:
    iface = _first_wireless_interface()
    support = _ap_support_status(iface)
    if not support.get("supported"):
        raise WifiError(str(support.get("reason") or "当前无线网卡不支持 AP 模式"), {"ap": support})

    resolved_ssid, resolved_password = _validate_ap_credentials(ssid, password)
    _stop_wifi_bootstrap()
    try:
        _prepare_wifi_device(iface)
        _ensure_ap_connection(resolved_ssid, resolved_password)
        _set_client_connections_autoconnect(False)
        _disconnect_wifi_devices()
        _run_nmcli(["--wait", "30", "connection", "up", AP_CONNECTION, "ifname", iface], timeout=35, check=True)
        return wifi_status(force_scan=False)
    except WifiError as exc:
        _disable_ap_connection()
        _set_client_connections_autoconnect(True)
        _start_wifi_bootstrap()
        raise WifiError(f"开启 AP 热点失败: {exc}", {"ap": _ap_status(iface)}) from exc


def activate_client_wifi() -> dict[str, Any]:
    iface = _first_wireless_interface()
    _stop_wifi_bootstrap()
    _prepare_wifi_device(iface)
    _disable_ap_connection()
    ensure_default_connections()
    _set_client_connections_autoconnect(True)
    _disconnect_wifi_devices()
    _start_wifi_bootstrap()
    return wifi_status(force_scan=False)
