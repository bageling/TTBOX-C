import socket,struct,time,os,sys
# mouse cmd.sock 默认单点真源（A-PATH-5）：复用同仓 plugins/web/lib/paths.py。
# 根锚发现（P6）：向上找同时含 plugins/usbproxy/scripts/deploy 的目录，
# 不写死 "../../" 相对跳目录（换一次布局就静默指错根）。
_root = os.path.dirname(os.path.abspath(__file__))
while _root != os.path.dirname(_root) and not all(
        os.path.isdir(os.path.join(_root, _n))
        for _n in ("plugins", "usbproxy", "scripts", "deploy")):
    _root = os.path.dirname(_root)
sys.path.append(_root)
from _ttbox_paths import MOUSE_CMD_SOCK_DEFAULT as _CMD
def rpc(typ,rid,payload=b""):
    s=socket.socket(socket.AF_UNIX,socket.SOCK_SEQPACKET)
    s.connect(_CMD)
    s.settimeout(3)
    hdr=struct.pack("<HBBI",0x4F50,1,typ,rid)
    s.sendall(hdr+payload)
    r=s.recv(256)
    s.close()
    return r
# PING (1) -> PING_RESP (2)
try:
    r=rpc(1,1)
    print("PING resp",len(r),"bytes:",r.hex())
except Exception as ex:
    print("PING fail",ex)
# GET_STATE (6) -> resp(7) 9B payload
try:
    r=rpc(6,2)
    print("GET_STATE resp",len(r),"bytes:",r.hex())
    if len(r)>=15:
        magic,ver,typ,rid=struct.unpack("<HBBI",r[:8])
        mask=struct.unpack("<B",r[8:9])[0]
        ts=struct.unpack("<q",r[9:17])[0]
        print("type=",typ,"mask=",mask,"ts_ns=",ts)
except Exception as ex:
    print("GET_STATE fail",ex)
