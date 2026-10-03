#!/usr/bin/env python3
"""OTA 本地假服务器（H-26 联调夹具；**不入发布包**，仅限本地联调）。

作用：无真分发服务器时，本地跑通"检查 → 安装"全链（契约见
docs/protocols/ota-server-contract.md）。

它做三件事：
  1. 在 --serve-dir 里自动生成：自签证书（cert.pem/key.pem）、
     一个最小合法更新包 ttbox-update-<ver>.tgz（含 RELEASE_MANIFEST.json）及其
     旁车签名 .sign.json（用 --priv 指定的私钥签，默认 ttbox-ota-2026b）；
  2. 提供 GET /latest → 三字段 JSON（契约 §2）；
  3. 提供 GET /pkgs/<file> → 静态下发包与签名。

★ 更新器强制 https：夹具用自签证书，客户端需 `SSL_CERT_FILE=<serve-dir>/cert.pem`
  让 urllib 信任之。夹具绝不上线，签出来的包只用于本地链路演练。
"""
from __future__ import annotations

import argparse
import hashlib
import http.server
import json
import ssl
import subprocess
import sys
import time
from pathlib import Path

HERE = Path(__file__).resolve().parent
# 根锚发现（P6/A-PATH-3）：不写死层级（原 `HERE.parents[1]`）。
_TREE_ROOT = HERE
while _TREE_ROOT != _TREE_ROOT.parent and not all(
        (_TREE_ROOT / _n).is_dir() for _n in ("plugins", "framework", "scripts", "deploy")):
    _TREE_ROOT = _TREE_ROOT.parent
# append（不用 insert(0)：顶到 stdlib 前有遮蔽同名标准库的风险）
for _entry in (str(HERE), str(_TREE_ROOT / "scripts")):
    if _entry not in sys.path:
        sys.path.append(_entry)

import ttbox_ota_sign as sign  # noqa: E402

FAKE_VERSION = "9.9.9"


def make_self_signed_cert(out_dir: Path, days: int = 2) -> tuple[Path, Path]:
    cert, key = out_dir / "cert.pem", out_dir / "key.pem"
    subprocess.run(
        ["openssl", "req", "-x509", "-newkey", "ec", "-pkeyopt", "ec_paramgen_curve:prime256v1",
         "-keyout", str(key), "-out", str(cert), "-days", str(days), "-nodes",
         "-subj", "/CN=ttbox-ota-fixture.local"],
        check=True, capture_output=True)
    return cert, key


def make_fixture_package(serve_dir: Path, priv: Path, key_id: str,
                         password: bytes | None = None) -> str:
    """生成最小合法包 + 旁车签名，返回包文件名。"""
    import tarfile
    import tempfile

    ver = FAKE_VERSION
    root = serve_dir / f"src-{ver}"
    (root / "payload" / "bin").mkdir(parents=True, exist_ok=True)
    (root / "payload" / "bin" / "ttbox_core_main").write_bytes(b"TTBOX-FIXTURE-BODY")
    bin_sha = hashlib.sha256((root / "payload/bin/ttbox_core_main").read_bytes()).hexdigest()
    (root / "RELEASE_MANIFEST.json").write_text(json.dumps(
        {"version": ver, "built_at": "2026-09-18T00:00:00Z", "git_sha": "",
         "files_sha256": {"bin/ttbox_core_main": bin_sha}}), encoding="utf-8")
    tgz = serve_dir / f"ttbox-update-{ver}.tgz"
    with tarfile.open(tgz, "w:gz") as t:
        t.add(root / "payload", arcname="payload")
        t.add(root / "RELEASE_MANIFEST.json", arcname="RELEASE_MANIFEST.json")
    sign.sign_package(str(tgz), ver, key_id, str(priv), password=password)
    return tgz.name


class Handler(http.server.BaseHTTPRequestHandler):

    def do_GET(self):
        if self.path.startswith("/latest"):
            pkg = f"https://{self.headers.get('Host', '127.0.0.1:8443')}/pkgs/ttbox-update-{FAKE_VERSION}.tgz"
            body = json.dumps({
                "latest_version": FAKE_VERSION,
                "package_url": pkg,
                "sign_url": pkg + ".sign.json",
            }).encode()
            self.send_response(200)
            self.send_header("Content-Type", "application/json")
            self.send_header("Content-Length", str(len(body)))
            self.end_headers()
            self.wfile.write(body)
            return
        name = self.path.removeprefix("/pkgs/").split("?")[0]
        root = self.server.serve_dir
        fp = (root / name).resolve()
        if not str(fp).startswith(str(root)) or not fp.is_file():
            self.send_error(404)
            return
        data = fp.read_bytes()
        self.send_response(200)
        self.send_header("Content-Type", "application/octet-stream")
        self.send_header("Content-Length", str(len(data)))
        self.end_headers()
        self.wfile.write(data)

    def log_message(self, fmt, *args):
        print("[fake-ota]", fmt % args)


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(description="OTA 本地假服务器（联调夹具，不上线）")
    ap.add_argument("--port", type=int, default=8443)
    ap.add_argument("--serve-dir", default="/tmp/ota-fixture")
    ap.add_argument("--priv", default="C:/ttbox-ota-keys/ttbox-ota-2026b.priv.pem",
                    help="签名私钥（口令加密时走 TTBOX_OTA_PRIV_PASSWORD 环境变量）")
    ap.add_argument("--key-id", default=sign.DEFAULT_KEY_ID)
    a = ap.parse_args(argv)

    serve = Path(a.serve_dir)
    serve.mkdir(parents=True, exist_ok=True)
    cert, key = make_self_signed_cert(serve)
    print(f"[fake-ota] 自签证书: {cert}")

    priv_pem = Path(a.priv).read_bytes()
    password = None
    import os
    if os.environ.get("TTBOX_OTA_PRIV_PASSWORD"):
        password = os.environ["TTBOX_OTA_PRIV_PASSWORD"].encode()
    name = make_fixture_package(serve, Path(a.priv), a.key_id, password=password)
    print(f"[fake-ota] 夹具包: {serve / name}")
    print(f"[fake-ota] 客户端信任证书: SSL_CERT_FILE={cert}")

    httpd = http.server.ThreadingHTTPServer(("0.0.0.0", a.port), Handler)
    httpd.serve_dir = serve
    ctx = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
    ctx.load_cert_chain(cert, key)
    httpd.socket = ctx.wrap_socket(httpd.socket, server_side=True)
    print(f"[fake-ota] https://127.0.0.1:{a.port}/latest  （Ctrl-C 退出）")
    try:
        httpd.serve_forever()
    except KeyboardInterrupt:
        pass
    return 0


if __name__ == "__main__":
    sys.exit(main())
