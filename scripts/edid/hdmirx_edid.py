#!/usr/bin/env python3
"""TTBOX RK3588 HDMI-RX EDID 工具。"""
import argparse
import os
import struct
import subprocess
import sys

# A-PATH-3：自身位置相对派生（原硬编码 "/opt/ttbox/scripts" 已删；且用 append 防遮蔽 stdlib）。
# 本文件在 <root>/scripts/edid/ ⇒ 父目录 = scripts/（edid 包所在），再上 = <root>。
sys.path.append(os.path.dirname(os.path.abspath(__file__)))
sys.path.append(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

try:
    from edid.timing_db import TIMING_MAP
except Exception:
    TIMING_MAP = {}

PROFILES = {
    "standard-dual": ["1080p120", "1440p60"],
    "boot-safe-1080p240": ["1080p60compat", "1080p120compat", "1080p240compat", "1440p60"],
    "boot-safe-full": ["1080p60compat", "1080p120", "1080p144", "1080p240compat", "1440p60", "1440p120", "1440p144", "2160p60"],
    "rk3588-full": ["1440p144", "1080p60", "1080p90", "1080p120", "1080p144", "1080p240", "1440p60", "1440p120", "2160p60"],
    "single-1440p60": ["1440p60"], "single-1080p120-compat": ["1080p120compat"],
    "single-1080p60": ["1080p60"], "single-1080p60-compat": ["1080p60compat"],
    "single-1080p90": ["1080p90"], "single-1080p120": ["1080p120"],
    "single-1080p144": ["1080p144"], "single-1080p240": ["1080p240"],
    "single-1080p240-compat": ["1080p240compat"], "single-1440p120": ["1440p120"],
    "single-1440p144": ["1440p144"], "single-2160p60": ["2160p60"],
}


def _validate_video_device(device):
    """EDID 属于 HDMI-RX V4L2 节点，禁止误传 DRM 输出节点。"""
    if device != "/dev/video0":
        raise ValueError(
            f"错误的 HDMI-RX 设备 {device}：EDID 注入必须使用 /dev/video0，"
            "/dev/dri/card0 仅用于 loopout"
        )


def _decode_vendor(data):
    if len(data) < 10:
        return "???"
    value = (data[8] << 8) | data[9]
    chars = [chr(((value >> shift) & 0x1F) + 64) for shift in (10, 5, 0)]
    return "".join(chars) if all("A" <= c <= "Z" for c in chars) else "???"


def _descriptor_name(edid):
    for off in (54, 72, 90, 108):
        if off + 18 <= len(edid) and edid[off + 3] == 0xFC:
            return bytes(ch for ch in edid[off + 5:off + 18] if 32 <= ch <= 126).decode("ascii", "replace").strip()
    return ""


def cmd_list():
    print("Profiles:")
    for name, tokens in PROFILES.items():
        print(f"  {name:<24} {tokens}")
    print("\nModes:")
    for token, timing in TIMING_MAP.items():
        pc_khz = int(round(timing.pixel_clock * 1000))
        total = (timing.h_active + timing.h_blank) * (timing.v_active + timing.v_blank)
        hz = timing.pixel_clock * 1_000_000 / total if total else timing.refresh
        print(f"  {token:<14} {timing.width}x{timing.height}@{int(timing.refresh)} pixel_clock={pc_khz} kHz actual={hz:.2f} Hz")


def _current_edid_raw(device):
    try:
        result = subprocess.run(["v4l2-ctl", "-d", device, "--get-edid=pad=0,format=raw"], capture_output=True, timeout=5)
        return result.stdout if result.returncode == 0 and result.stdout else None
    except (OSError, subprocess.SubprocessError):
        return None


def _print_actual_rx(device):
    try:
        fmt = subprocess.run(["v4l2-ctl", "-d", device, "--get-fmt-video"], capture_output=True, text=True, timeout=5)
        print("Actual Format:")
        for line in fmt.stdout.splitlines():
            if "Width/Height" in line or "Pixel Format" in line:
                print("  " + line.strip())
        path = "/sys/kernel/debug/hdmirx/status"
        if os.path.exists(path):
            text = open(path, encoding="utf-8", errors="ignore").read()
            for line in text.splitlines():
                if "Clk-Ch:" in line or line.startswith("Timing:") or line.startswith("Mode:"):
                    print("Actual RX: " + line.strip())
    except (OSError, subprocess.SubprocessError):
        print("Actual RX: unavailable")


def cmd_status(device):
    try:
        _validate_video_device(device)
    except ValueError as exc:
        print(str(exc), file=sys.stderr)
        return 2
    print(f"Device: {device}")
    raw = _current_edid_raw(device)
    if raw is None or len(raw) < 128:
        print("Current EDID: none\nFormat: unknown")
        return 1
    pid = struct.unpack("<H", raw[10:12])[0]
    serial = struct.unpack("<I", raw[12:16])[0]
    print(f"Current EDID: name={_descriptor_name(raw)} vendor={_decode_vendor(raw)} product=0x{pid:04x} serial=0x{serial:08x}")
    pc = raw[54] | (raw[55] << 8)
    h_active = raw[56] | ((raw[58] & 0xF0) << 4)
    h_blank = raw[57] | ((raw[58] & 0x0F) << 8)
    v_active = raw[59] | ((raw[61] & 0xF0) << 4)
    v_blank = raw[60] | ((raw[61] & 0x0F) << 8)
    total = (h_active + h_blank) * (v_active + v_blank)
    fps = pc * 10000 / total if total else 0
    print(f"Advertised DTD: {h_active}x{v_active}")
    print(f"Advertised Timings: {h_active}x{v_active} pixelclock={pc * 10000} Hz fps={fps:.2f}")
    _print_actual_rx(device)
    return 0


def cmd_build(profile, native, added, name, vendor, product_id, serial, device, apply, output):
    import json
    from edid.builder import build_from_config
    from edid.mode_builder import load_config
    cfg = load_config()  # A-PATH-4：路径经 TTBOX_DISPLAY_CONFIG / TTBOX_PREFIX 派生
    if profile:
        cfg["profile"] = profile
        if not native and profile in PROFILES:
            cfg["native_mode"] = PROFILES[profile][0]
    if native:
        cfg["native_mode"] = native
    if added:
        cfg["added_modes"] = list(added)
        cfg["native_only"] = False
    if name:
        cfg["name"] = name[:13]
    if vendor:
        cfg["vendor"] = vendor.upper()[:3]
    if product_id:
        cfg["product_id"] = product_id
    if serial:
        cfg["serial"] = serial
    try:
        edid = build_from_config(cfg)
    except Exception as exc:
        print(f"EDID 生成失败: {exc}", file=sys.stderr)
        return 1
    # 运行根派生（A-PATH-4）：与 scripts/edid/mode_builder.py 同口径，不散写 "/opt/ttbox/…"。
    out = output or os.path.join(
        os.environ.get("TTBOX_PREFIX", "/opt/ttbox"), "runtime", "edid", "current.bin")
    os.makedirs(os.path.dirname(out) or ".", exist_ok=True)
    with open(out, "wb") as handle:
        handle.write(edid)
    print(f"Generated {len(edid)}B EDID -> {out} (name={cfg['name']} vendor={cfg['vendor']} native={cfg['native_mode']})")
    if apply:
        try:
            _validate_video_device(device)
        except ValueError as exc:
            print(str(exc), file=sys.stderr)
            return 2
        result = subprocess.run(["v4l2-ctl", "-d", device, f"--set-edid=pad=0,file={out},format=raw"], capture_output=True, text=True, timeout=10)
        if result.returncode != 0:
            print(f"注入失败: {result.stderr or result.stdout}", file=sys.stderr)
            return 1
        expected = open(out, "rb").read()
        readback = subprocess.run(["v4l2-ctl", "-d", device, "--get-edid=pad=0,format=raw"], capture_output=True, timeout=10)
        if readback.returncode != 0 or readback.stdout != expected:
            print("注入失败：驱动回读 EDID 与写入文件不一致", file=sys.stderr)
            return 1
        print(f"Applied EDID to {device}")
    return 0


def cmd_builtin(version):
    node = "/sys/devices/platform/fdee0000.hdmirx-controller/hdmirx/hdmirx/edid"
    if not os.access(node, os.W_OK):
        print(f"EDID node not writable: {node}", file=sys.stderr)
        return 1
    try:
        with open(node, "w") as handle:
            handle.write(str(version))
        print(f"EDID builtin group: {open(node).read().strip()}")
        return 0
    except OSError as exc:
        print(f"写入失败: {exc}", file=sys.stderr)
        return 1


def main():
    parser = argparse.ArgumentParser(prog="hdmirx_edid.py", description="TTBOX EDID 工具")
    group = parser.add_mutually_exclusive_group(required=True)
    group.add_argument("--list", action="store_true")
    group.add_argument("--status", action="store_true")
    group.add_argument("--profile")
    group.add_argument("--native")
    group.add_argument("--builtin")
    parser.add_argument("--device", default="/dev/video0")
    parser.add_argument("--add", action="append", default=[])
    parser.add_argument("--name")
    parser.add_argument("--vendor")
    parser.add_argument("--product-id")
    parser.add_argument("--serial")
    parser.add_argument("--apply", action="store_true")
    parser.add_argument("--output")
    args = parser.parse_args()
    if args.list:
        cmd_list(); return 0
    if args.status:
        return cmd_status(args.device)
    if args.builtin:
        return cmd_builtin(args.builtin)
    return cmd_build(args.profile, args.native, args.add, args.name, args.vendor, args.product_id, args.serial, args.device, args.apply, args.output)


if __name__ == "__main__":
    sys.exit(main())
