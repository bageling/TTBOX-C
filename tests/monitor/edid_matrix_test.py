#!/usr/bin/env python3
"""EDID 注入矩阵测试：不同分辨率/名称/刷新率，逐个注入实测协商结果。"""
import json
import subprocess
import sys
import time
from pathlib import Path

# A-PATH-3 / P6：根锚发现（原 parents[2]），append 防遮蔽 stdlib。
_TREE_ROOT = Path(__file__).resolve().parent
while _TREE_ROOT != _TREE_ROOT.parent and not all(
        (_TREE_ROOT / _n).is_dir() for _n in ("plugins", "framework", "scripts", "deploy")):
    _TREE_ROOT = _TREE_ROOT.parent
sys.path.append(str(_TREE_ROOT / "scripts"))
from edid.builder import EdidBuilder

HPD = "/sys/devices/platform/fdee0000.hdmirx-controller/hdmirx/hdmirx/status"
VIDEO = "/dev/video0"
EDID_TMP = "/tmp/edid_matrix.bin"


def write_sys(path, text):
    try:
        with open(path, "w") as f:
            f.write(text)
        return True
    except OSError:
        return False


def inject_and_measure(edid_bytes, wait_sec=14):
    with open(EDID_TMP, "wb") as f:
        f.write(edid_bytes)
    write_sys(HPD, "off")
    time.sleep(1)
    r = subprocess.run(
        ["v4l2-ctl", "-d", VIDEO, "--set-edid=pad=0,file=" + EDID_TMP + ",format=raw"],
        capture_output=True, text=True, timeout=15)
    if r.returncode != 0:
        write_sys(HPD, "on")
        return {"injected": False, "error": (r.stderr or r.stdout).strip()[:80]}
    write_sys(HPD, "on")
    time.sleep(wait_sec)
    # 读协商结果
    st = subprocess.run(
        ["cat", "/sys/kernel/debug/hdmirx/status"],
        capture_output=True, text=True, timeout=10).stdout
    timing = ""
    mode = ""
    for line in st.splitlines():
        if "Timing:" in line:
            timing = line.split("Timing:")[1].split("(")[0].strip()
        if "Mode:" in line:
            mode = line.split("Mode:")[1].strip()
    fmt = subprocess.run(
        ["v4l2-ctl", "-d", VIDEO, "--get-fmt-video"],
        capture_output=True, text=True, timeout=10).stdout
    width = height = ""
    for line in fmt.splitlines():
        if "Width/Height" in line:
            parts = line.split(":")[1].strip().split("/")
            width, height = parts[0].strip(), parts[1].strip()
    return {
        "injected": True,
        "timing": timing,
        "mode": mode,
        "captured": f"{width}x{height}",
    }


def run_case(native, name, vendor, added=None, native_only=False):
    cfg = {
        "vendor": vendor, "product_id": "0xccd5", "serial": "0xdf6d7185",
        "name": name, "native_mode": native, "native_only": native_only,
        "profile": "boot-safe-full",
    }
    if added:
        cfg["added_modes"] = added
    edid = EdidBuilder(cfg).build()
    res = inject_and_measure(edid)
    res["native"] = native
    res["name"] = name
    return res


CASES = [
    # 分辨率/刷新率矩阵
    ("1080p60",  "ZWX-D41C26", "ZWX", None, False),
    ("1080p120", "ZWX-D41C26", "ZWX", None, False),
    ("1080p144", "ZWX-D41C26", "ZWX", None, False),
    ("1080p240", "ZWX-D41C26", "ZWX", None, False),
    ("1440p60",  "ZWX-D41C26", "ZWX", None, False),
    ("1440p120", "ZWX-D41C26", "ZWX", None, False),
    ("1440p144", "ZWX-D41C26", "ZWX", None, False),
    ("2160p60",  "ZWX-D41C26", "ZWX", None, False),
    # 名称矩阵（相同 1440p144）
    ("1440p144", "TTBox-1",    "TTB", None, False),
    ("1440p144", "ABCDEFGH-12345", "ABC", None, False),
    ("1440p144", "test-name-2",  "QWE", None, False),
    # 动态分辨率（非内置 token）
    ("1920x1080@90", "ZWX-D41C26", "ZWX", None, False),
]

results = []
for native, name, vendor, added, nonly in CASES:
    print(f"测试: {native} | {name} | {vendor} ...", flush=True)
    r = run_case(native, name, vendor, added, nonly)
    r["case"] = f"{native}/{name}"
    results.append(r)
    print(f"  → {r.get('captured','?')} | {r.get('mode','?')} | {r.get('timing','?')}", flush=True)
    time.sleep(2)

print()
print("=== 矩阵汇总 ===")
print(f"{'模式':<12} {'名称':<16} {'协商结果':<14} {'链路':<6} {'时序'}")
for r in results:
    ok = "✅" if r["injected"] and r.get("captured") not in ("800x600", "0x0", "") else "❌"
    print(f"{r['native']:<12} {r['name']:<16} {r.get('captured','?'):<14} {ok:<4} {r.get('mode','')} {r.get('timing','')}")

with open("/tmp/edid_matrix_result.json", "w") as f:
    json.dump(results, f, ensure_ascii=False, indent=2)
print("\n结果已存 /tmp/edid_matrix_result.json")
