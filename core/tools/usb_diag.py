#!/usr/bin/env python3
# ★ 分工（业主 2026-10-06 定案「代码全部用 C++，只有脚本用 py」）：
#   本脚本只做「调 C++ / 生成输入 / 判阈值 / 出报告」，不含任何控制逻辑。
#   控制器与物理仿真全在 C++（core/src/aim/*.hpp、core/tools/replay/replay_main.cpp）。
import socket, struct, os, sys, subprocess, json
# 路径默认单点真源（A-PATH-5）：复用同仓 plugins/web/lib/paths.py，禁止在此散写 socket 字面量。
# 根锚发现（P6）：向上找同时含 plugins/usbproxy/scripts/deploy 的目录，
# 不写死 "../../" 相对跳目录（换一次布局就静默指错根）。
_root = os.path.dirname(os.path.abspath(__file__))
while _root != os.path.dirname(_root) and not all(
        os.path.isdir(os.path.join(_root, _n))
        for _n in ("plugins", "usbproxy", "scripts", "deploy")):
    _root = os.path.dirname(_root)
sys.path.append(_root)
from _ttbox_paths import IPC_SOCKET_DEFAULT as _IPC_DEFAULT, MOUSE_CMD_SOCK_DEFAULT as _MOUSE_CMD_DEFAULT

# 1. cmd.sock test
print("=== cmd.sock SEQPACKET test ===")
s = socket.socket(socket.AF_UNIX, socket.SOCK_SEQPACKET)
try:
    s.connect(_MOUSE_CMD_DEFAULT)
    print("  connect OK")
    hdr = struct.pack("<HBB I", 0x4F50, 1, 1, 1)
    s.send(hdr)
    s.settimeout(2)
    resp = s.recv(4096)
    print(f"  recv {len(resp)} bytes, hdr_type={resp[3]}")
    s.close()
except Exception as e:
    print(f"  FAILED: {e}")
    try: s.close()
    except: pass

# 2. ttbox_core status via raw socket
print()
print("=== ttbox_core GET_STATUS (IPC socket) ===")
s2 = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
try:
    s2.connect(os.environ.get("TTBOX_IPC_SOCKET", _IPC_DEFAULT))
    msg = json.dumps({"type": "GET_STATUS"}).encode()
    s2.send(msg)
    s2.settimeout(3)
    chunks = []
    while True:
        try:
            chunk = s2.recv(65536)
            if not chunk:
                break
            chunks.append(chunk)
        except socket.timeout:
            break
    raw = b"".join(chunks)
    if raw:
        data = json.loads(raw)
        d = data.get("data", {})
        m = d.get("metrics", {})
        for k in ["detect_count","injection_allowed","mouse_control_connected",
                  "mouse_control_send_count","capture_fps","frames_superseded",
                  "gated_frames","aim_active","last_mouse_control_dx","last_mouse_control_dy",
                  "last_mouse_control_timestamp_us"]:
            print(f"  {k}: {m.get(k)}")
        print(f"  runtime_running: {d.get('runtime_running')}")
        print(f"  running: {d.get('running')}")
    else:
        print("  no data received")
    s2.close()
except Exception as e:
    print(f"  FAILED: {e}")
    try: s2.close()
    except: pass

# 3. DWC3 / typec
def _first_udc():
    """首个 UDC 名（P8：不写死 fc000000.usb）。

    UDC 编号是内核枚举顺序的产物，不是板子身份（换内核/加控制器即变）。
    USB_PROXY_DEVICE 优先（与 usbproxy 启动脚本同一覆盖链，见
    docs/protocols/config-path-env-registry.md），否则取 /sys/class/udc 里
    名字排序首个。都没有则返回空串，调用方打印"无 UDC"而不是指一个不存在的节点。
    """
    env = os.environ.get("USB_PROXY_DEVICE")
    if env:
        return env
    try:
        names = sorted(n for n in os.listdir("/sys/class/udc") if not n.startswith("."))
    except OSError:
        return ""
    return names[0] if names else ""


print()
print("=== DWC3 ===")
_UDC = _first_udc()
if not _UDC:
    print("  (无 UDC：/sys/class/udc 为空)")
else:
    print(f"  udc: {_UDC}")
    for f in ["state", "uevent", "maximum_speed", "current_speed"]:
        p = f"/sys/class/udc/{_UDC}/{f}"
        try: print(f"  {f}: {open(p).read().strip()}")
        except: pass

print()
print("=== typec ===")
for f in ["data_role","power_role"]:
    p = f"/sys/class/typec/port0/{f}"
    try: print(f"  {f}: {open(p).read().strip()}")
    except: pass

partner = "/sys/class/typec/port0-partner"
print(f"  partner exists: {os.path.isdir(partner)}")
if os.path.isdir(partner):
    print(f"  partner contents: {os.listdir(partner)}")

# 4. USB devices
print()
print("=== USB devices ===")
usb = "/sys/bus/usb/devices"
for name in sorted(os.listdir(usb)):
    vd = os.path.join(usb, name, "idVendor")
    if not os.path.exists(vd): continue
    vid = open(vd).read().strip()
    pid = open(os.path.join(usb, name, "idProduct")).read().strip()
    try: spd = open(os.path.join(usb, name, "speed")).read().strip()
    except: spd = "?"
    try: prod = open(os.path.join(usb, name, "product")).read().strip()
    except: prod = ""
    try: cls = open(os.path.join(usb, name, "bDeviceClass")).read().strip()
    except: cls = "?"
    print(f"  {name}: {vid}:{pid} class={cls} spd={spd} {prod}")

# 5. dmesg
print()
print("=== dmesg usb (tail) ===")
r2 = subprocess.run(["dmesg"], capture_output=True, text=True)
for line in r2.stdout.splitlines()[-60:]:
    ll = line.lower()
    if any(k in ll for k in ["usb","gadget","raw","dwc","hidg"]):
        print(f"  {line}")


★ 分工（业主 2026-10-06 定案「代码全部用 C++，只有脚本用 py」）：
  本脚本是**板端运维/诊断**工具，只读不写、不含任何控制逻辑。
  控制器与仿真逻辑全在 C++（core/src/aim/*.hpp、core/tools/replay/replay_main.cpp）。
