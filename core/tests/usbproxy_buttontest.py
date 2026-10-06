#!/usr/bin/env python3
"""板端验证：usbproxy BUTTON_CMD -> event.sock STATE_SNAPSHOT 按钮掩码链路。"""
import socket, struct, sys, time

# mouse cmd/event sock 默认单点真源（A-PATH-5）：复用同仓 plugins/web/lib/paths.py。
# 根锚发现（P6）：向上找同时含 plugins/usbproxy/scripts/deploy 的目录，
# 不写死 "../.." 相对跳目录（换一次布局就静默指错根）。
import os as _os, sys as _sys
_root = _os.path.dirname(_os.path.abspath(__file__))
while _root != _os.path.dirname(_root) and not all(
        _os.path.isdir(_os.path.join(_root, _n))
        for _n in ("plugins", "usbproxy", "scripts", "deploy")):
    _root = _os.path.dirname(_root)
_sys.path.append(_root)
from _ttbox_paths import MOUSE_CMD_SOCK_DEFAULT as CMD_SOCK, MOUSE_EVENT_SOCK_DEFAULT as EVENT_SOCK


def hdr(typ, rid):
    return struct.pack("<HBB", 0x4F50, 1, typ) + struct.pack("<I", rid)


def get_state():
    s = socket.socket(socket.AF_UNIX, socket.SOCK_SEQPACKET)
    s.settimeout(3)
    s.connect(CMD_SOCK)
    s.sendall(hdr(6, 1))
    r = s.recv(128)
    s.close()
    assert len(r) >= 17 and r[3] == 7, "GET_STATE response abnormal: %s" % r.hex()
    return r[8]


def send_button(button, action):
    s = socket.socket(socket.AF_UNIX, socket.SOCK_SEQPACKET)
    s.settimeout(3)
    s.connect(CMD_SOCK)
    s.sendall(hdr(5, 2) + struct.pack("<BB", button, action))
    s.close()


def read_snapshot(ev, timeout=2.0):
    ev.settimeout(timeout)
    r = ev.recv(128)
    assert r[3] == 10, "expected STATE_SNAPSHOT, got %s" % r.hex()
    return r[8]


def main():
    ev = socket.socket(socket.AF_UNIX, socket.SOCK_SEQPACKET)
    ev.settimeout(3)
    ev.connect(EVENT_SOCK)
    ev.sendall(hdr(8, 3))
    ack = ev.recv(64)
    assert ack[3] == 9, "subscribe ACK abnormal: %s" % ack.hex()

    print("initial get_state mask=%#04x" % get_state())
    print("initial snapshot mask=%#04x" % read_snapshot(ev))

    send_button(2, 1)  # button2 down
    time.sleep(0.15)
    down_masks = [read_snapshot(ev) for _ in range(3)]
    print("after down snapshot masks=%s" % ["%#04x" % m for m in down_masks])

    send_button(2, 2)  # button2 up
    time.sleep(0.15)
    up_masks = [read_snapshot(ev) for _ in range(3)]
    print("after up snapshot masks=%s" % ["%#04x" % m for m in up_masks])
    print("final get_state mask=%#04x" % get_state())

    ok = all(m == 0x02 for m in down_masks) and all(m == 0x00 for m in up_masks)
    print("BUTTON_CMD->SNAPSHOT %s" % ("PASS" if ok else "FAIL"))
    ev.close()
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
