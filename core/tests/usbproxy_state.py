import socket,struct,time,os,sys
# mouse cmd.sock 默认单点真源（A-PATH-5）：复用同仓 plugins/web/lib/paths.py。
# 根锚发现（P6）：向上找同时含 plugins/framework/scripts/deploy 的目录，
# 不写死 "../../" 相对跳目录（换一次布局就静默指错根）。
_root = os.path.dirname(os.path.abspath(__file__))
while _root != os.path.dirname(_root) and not all(
        os.path.isdir(os.path.join(_root, _n))
        for _n in ("plugins", "framework", "scripts", "deploy")):
    _root = os.path.dirname(_root)
sys.path.append(_root)
from plugins.web.lib.paths import MOUSE_CMD_SOCK_DEFAULT as _CMD
# 0x4F50 / 1 / 6 (GET_STATE) -> resp 9B: <BQ mask, ts_ns
s=socket.socket(socket.AF_UNIX,socket.SOCK_SEQPACKET)
s.connect(_CMD)
s.settimeout(3)
hdr=struct.pack("<HBB",0x4F50,1,6)+struct.pack("<I",1)
s.sendall(hdr)
r=s.recv(128)
magic,ver,typ=struct.unpack_from("<HBB",r,0)
rid=struct.unpack_from("<I",r,4)[0]
mask=r[8]
ts=struct.unpack_from("<q",r,9)[0]
print(f"GET_STATE magic=0x{magic:04x} type={typ} mask={mask:#04x} ts_ns={ts}")
s.close()
# 连续两次 MOVE 验证 pending 计数行为
s=socket.socket(socket.AF_UNIX,socket.SOCK_SEQPACKET)
s.connect(_CMD)
s.settimeout(3)
for i in range(3):
    pkt=struct.pack("<HBB",0x4F50,1,4)+struct.pack("<I",100+i)+struct.pack("<iii",17,-9,0)
    s.sendall(pkt)
    time.sleep(0.1)
print("sent 3 test MOVE_CMD (17,-9)")
s.close()
