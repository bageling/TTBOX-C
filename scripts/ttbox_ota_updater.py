#!/usr/bin/env python3
"""TTBOX OTA 更新器（root 独立进程；由 ttbox-ota.path 路径单元拉起，脱离 Web 存活）。

★ 结构必经步骤（t1.11 §0.1）——**任一前置不通过即终止，绝不进入下一步**：
  ① URL scheme 白名单（仅 https）
  ② 下载到**临时目录**（非正式目录）
  ③ sha256 比对（下载物 vs 签名记录的 `sha256`）
  ④ Ed25519 验签（对 canonical JSON）
  ⑤ 展开到 `releases/<ver>.ota.staging/`（勿与 release_install 的 <ver>.staging 同名）
  ⑥ 全量 sha256 复验（RELEASE_MANIFEST.json）
  ⑥b 降级拒绝：新包版本必须**高于**当前版本，否则 `downgrade_rejected`
     （例外：⑦b 健康检查失败后的自动回滚不受此限——那是保命路径，不是"装包"）
  ⑦ 调 T1.01 原子发布 → 健康检查 → 失败自动 rollback

★ 双因子是 `and`（③ 与 ④ 缺一不可，PRD SEC-01 验收 1）。
★ 更新后删包/清树（§0.2）：`finally` 清临时目录；staging 无论成败一律清
  （2026-09-30 补：成功路径原缺，板端残留 <ver>.ota.staging 12M/版）。
★ 签名对象 = **旁车 `<pkg>.tgz.sign.json`**（不在 tgz 内）—— 消除 §6 陷阱 3 的自指，
  详见 `tools/ota/ttbox_ota_sign.py` 文件头的契约裁定。
★ 私钥永不入库；本进程**只有公钥**（发布树内 `deploy/keys/`）。

★ 健康检查判据（2026-09-20 订正 §2.3）：三个服务进程 active + core IPC 已就绪
  （GET_STATUS 返回非空 `version`）；**不看授权** —— 授权是客户状态，与"本次升级是否
  成功"无关，放进门禁会造成死锁（授权到期 ⇒ 升不了级 ⇒ 升级恰是解授权问题的手段）。
  ★ **也不看 `current_model_id`**：更新完成后 core 会按 R6（刚更新过）或用户既有停用意愿
  保持 AI 流水线停止（`runtime_intent.want_runtime_running=false`），此刻没有模型在跑。
  旧口径要求「模型在跑」⇒ 更新后停止态必被判 `health_check_failed` 并自动回滚
  （业主 2026-09-20 实测：1.5.17 装完 30s 后回滚到 1.5.16）。判据与
  `scripts/ttbox_release_install.sh::health_check`（core.sock + IPC 应答 + web 端口）同口径。
  历史教训：更早的判据写 `model_loaded`/`license_available`，两个键在 IPC 契约里从未
  存在过 ⇒ 健康检查恒假 ⇒ 升级必自动回滚。键名一律以
  `core/src/ipc/IpcServer.cpp:882-891` 为准。

★ 任务文件模式（2026-09-18 定案 §2.2 特权通道）：`--from-jobs [DIR]` 无 URL 运行；
  web（User=ttbox）往任务目录丢 JSON（0770 root:ttbox），本进程（root）消费后把
  任务文件移入 processed/ 或 failed/ 留审计。任务文件字段：`url`（必填）、`key_id`、
  `version`（可选，缺省取签名记录）。
"""
from __future__ import annotations

import argparse
import base64
import hashlib
import json
import os
import re
import shutil
import subprocess
import sys
import tarfile
import tempfile
import time
import urllib.parse
import urllib.request
from pathlib import Path

try:
    from cryptography.hazmat.primitives import serialization
    from cryptography.hazmat.primitives.asymmetric.ed25519 import Ed25519PublicKey
except ImportError:  # pragma: no cover
    sys.stderr.write("需要 cryptography：apt-get install -y python3-cryptography\n")
    raise

DEFAULT_KEY_ID = "ttbox-ota-2026b"
RELEASES = "/opt/ttbox/releases"
CURRENT_LINK = "/opt/ttbox/current"
STATE = "/opt/ttbox/state"
STATUS_FILE = os.path.join(STATE, "ota_status.json")
# 发布树内公钥落点（2026-09-18 定案 §2.7）：<ver>/deploy/keys/<key_id>.pub。
# 旧路径（按仓库布局写的 tools/ota/keys）在板端发布树不存在，已废弃。
KEYS_DIR = Path(CURRENT_LINK) / "deploy" / "keys"
INSTALL_SCRIPT = "/opt/ttbox/current/scripts/ttbox_release_install.sh"
HEALTH_UNITS = ("ttbox-core", "ttbox-web", "ttbox-usbproxy")
HEALTH_TIMEOUT_S = 30
SIGNED_FIELDS = ("sha256", "version", "built_at", "key_id")
DEFAULT_JOBS_DIR = "/var/lib/ttbox/ota/jobs"


class OtaError(Exception):
    def __init__(self, state: str, detail: str):
        super().__init__(f"{state}: {detail}")
        self.state = state
        self.detail = detail


def canonical(rec: dict) -> bytes:
    return json.dumps(rec, sort_keys=True, separators=(",", ":"),
                      ensure_ascii=False).encode("utf-8")


def sha256_file(path: str) -> str:
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1024 * 1024), b""):
            h.update(chunk)
    return h.hexdigest()


# ---------------------------------------------------------------- 版本真源（单点）
def current_version() -> str:
    """当前运行版本：读 `current` 软链指向的 releases/<ver> 目录名；软链不可用时
    回退 `/opt/ttbox/state/version`；都拿不到返回空串（= 未知，降级判定放行）。"""
    try:
        name = os.path.basename(os.path.realpath(CURRENT_LINK).rstrip("/"))
        if name and name != "current":
            return name
    except Exception:
        pass
    try:
        return Path(STATE, "version").read_text(encoding="utf-8").strip()
    except Exception:
        return ""


def _version_key(v: str) -> tuple:
    """版本排序键：数字段按数值、其余段按字典序，段间逐位比较。"""
    parts = []
    for seg in re.split(r"[.\-_+]", str(v or "")):
        if not seg:
            continue
        if seg.isdigit():
            parts.append((0, int(seg), ""))
        else:
            parts.append((1, 0, seg))
    return tuple(parts)


def is_downgrade(new_ver: str, cur_ver: str) -> bool:
    """禁止降级（§2.4）：新包版本必须**高于**当前版本；相等也算拒绝（无意义重装）。"""
    if not cur_ver:
        return False
    return _version_key(new_ver) <= _version_key(cur_ver)


# ---------------------------------------------------------------- 默认（真实）实现
def default_fetch(url: str, dest: str, timeout: float = 120.0, attempts: int = 3) -> None:
    """仅 https；任何非 https 由调用方先拦（§0.1 ①）。

    2026-09-19 修复：七牛隧道到板端只有 ~12KB/s，且偶发 >60s 的读停顿，
    旧的 60s 超时一卡就整包失败。改为 120s 读超时 + 3 次重试（每次重写目标文件）。
    """
    last_exc: Exception | None = None
    for i in range(1, attempts + 1):
        try:
            req = urllib.request.Request(url, headers={"User-Agent": "ttbox-ota-updater"})
            with urllib.request.urlopen(req, timeout=timeout) as r, open(dest, "wb") as f:
                shutil.copyfileobj(r, f)
            return
        except Exception as e:  # 网络类异常一律重试；最后一次仍败则抛出
            last_exc = e
            sys.stderr.write(f"[ota] 下载第 {i}/{attempts} 次失败: {e!r}\n")
            if i < attempts:
                time.sleep(5)
    raise last_exc  # type: ignore[misc]


def default_install(staging: str, version: str) -> int:
    return subprocess.call([INSTALL_SCRIPT, version, staging, "--activate"])


def normalize_staging_perms(staging: str) -> None:
    """浇筑前规范化 staging 权限（2026-09-18 板端实测：Windows/msys 打的包会把
    ELF 记成 0644、属主还原成打包机 uid —— 两者都会让 unit 断言/服务运行失败）。
    发布树口径：目录 0755；bin/ 段下文件（含 plugins/*/bin 的 wrapper）、scripts/ 下
    文件与 *.sh 0755；其余文件 0644。"""
    for root, dirs, files in os.walk(staging):
        for d in dirs:
            os.chmod(os.path.join(root, d), 0o755)
        for f in files:
            p = os.path.join(root, f)
            rel = os.path.relpath(p, staging).replace(os.sep, "/")
            seg = rel.split("/")
            if (rel.endswith(".sh") or "scripts" in seg or "bin" in seg
                    or rel == "usbproxy/usb-proxy"):
                mode = 0o755
            else:
                mode = 0o644
            os.chmod(p, mode)


def default_rollback() -> int:
    return subprocess.call([INSTALL_SCRIPT, "--rollback"])


def _is_active(unit: str) -> bool:
    rc = subprocess.call(["systemctl", "is-active", "--quiet", unit])
    return rc == 0


def _ipc_get_status() -> dict:
    """读 core IPC（unix socket）拿业务能力；失败返回 {}（= 业务不可用）。

    请求体键名是 `type`（IpcServer.cpp:450 `request.find("type")`），
    不是旧版写的 `cmd` —— 那个键 core 根本不认识，GET_STATUS 永远答非所问。
    """
    import sys as _sys
    from pathlib import Path as _Path
    _sys.path.append(str(_Path(__file__).resolve().parents[1] / "plugins" / "web"))
    from lib.paths import IPC_SOCKET_DEFAULT as _IPC_DEFAULT
    sock_path = os.environ.get("TTBOX_IPC_SOCKET", _IPC_DEFAULT)
    try:
        import socket as _s
        with _s.socket(_s.AF_UNIX, _s.SOCK_STREAM) as c:
            c.settimeout(2.0)
            c.connect(sock_path)
            # NDJSON 行协议（lib/ipc.py 契约）：必须以 \n 结尾，core 只读一行即回；
            # 2026-09-18 板端实测：漏 \n 时 core 永远不响应 ⇒ 健康检查恒失败。
            c.sendall(b'{"type":"GET_STATUS"}\n')
            chunks = []
            while True:
                chunk = c.recv(65536)
                if not chunk:
                    break
                chunks.append(chunk)
                if chunk.rstrip(b"\r\n").endswith(b"}"):
                    break
            raw = b"".join(chunks)
        d = json.loads(raw.decode("utf-8", "replace"))
        return d.get("data", {}) if isinstance(d, dict) else {}
    except Exception:
        return {}


def _core_ipc_ready() -> bool:
    """core 是否已就绪：IPC GET_STATUS 能应答且返回非空 `version`。

    不用 `current_model_id` —— 更新后流水线按 R6/用户意愿保持停止，此刻没有模型在跑。
    """
    st = _ipc_get_status()
    return bool(str(st.get("version") or "").strip())


def default_health(timeout_s: int = HEALTH_TIMEOUT_S) -> bool:
    """升级成功判据（2026-09-20 订正）：三服务进程 active **且** core IPC 已就绪。

    ★ **不要求 `current_model_id` 非空**：更新完成后 core 会按 R6（刚更新过）或用户既有
    停用意愿保持 AI 流水线停止，此刻没有模型在跑；旧口径要求「模型在跑」会让每次更新
    都被判 `health_check_failed` 并自动回滚（业主 2026-09-20 实测 1.5.17）。
    授权同样不看（理由见文件头）。"""
    deadline = time.time() + timeout_s
    while time.time() < deadline:
        units_ok = all(_is_active(u) for u in HEALTH_UNITS)
        if units_ok and _core_ipc_ready():
            return True
        time.sleep(1)
    return False


# ---------------------------------------------------------------- 更新器
class OtaUpdater:
    """依赖全部可注入（fetch/install/rollback/health/pubkey），便于单测而不开后门。"""

    def __init__(self, releases_dir: str = RELEASES, status_path: str = STATUS_FILE,
                 keys_dir: Path | str = KEYS_DIR,
                 fetch=default_fetch, install=default_install,
                 rollback=default_rollback, health=default_health,
                 pubkey_pem: bytes | None = None):
        self.releases_dir = releases_dir
        self.status_path = status_path
        self.keys_dir = Path(keys_dir)
        self.fetch = fetch
        self.install = install
        self.rollback = rollback
        self.health = health
        self._pubkey_pem = pubkey_pem

    # -- 状态 --
    def _write_status(self, doc: dict) -> None:
        try:
            os.makedirs(os.path.dirname(self.status_path), exist_ok=True)
            with open(self.status_path, "w", encoding="utf-8") as f:
                json.dump(doc, f, ensure_ascii=False, sort_keys=True)
        except Exception as e:  # 状态写失败不得掩盖主流程判定
            sys.stderr.write(f"[warn] 写 ota_status 失败: {e!r}\n")

    def _fail(self, state: str, detail: str) -> int:
        self._write_status({"state": "FAILED", "error": state, "detail": detail})
        sys.stderr.write(f"OTA FAILED: {state}: {detail}\n")
        return 1

    def _progress(self, pct: int, phase: str, version: str | None = None) -> None:
        """分阶段进度（2026-09-19 修复「卡50」：前端轮询 ota_status.json，
        RUNNING 必须带真实 progress，否则 UI 永远停在硬编码值。）"""
        doc = {"state": "RUNNING", "progress": max(5, min(95, int(pct))), "phase": phase}
        if version:
            doc["version"] = version
        self._write_status(doc)

    # -- 公钥 --
    # ★ key_id/version 由任务文件（面板 /api/ota/install）传入，属不可信输入。
    #   两者都会拼进 root 上下文的文件路径（<keys_dir>/<key_id>.pub、
    #   releases/<ver>.ota.staging），不做字符白名单就是路径穿越：
    #   key_id="../../tmp/evil" 可让 root 验签加载攻击者自选公钥 ⇒ Ed25519 信任锚失效。
    _SAFE_ID_RE = re.compile(r"^[A-Za-z0-9][A-Za-z0-9._-]*$")

    @classmethod
    def _check_safe_id(cls, value: str, what: str) -> str:
        v = str(value or "").strip()
        if not cls._SAFE_ID_RE.match(v) or ".." in v:
            raise OtaError("unsafe_field", f"非法 {what}: {value!r}")
        return v

    def _load_pubkey(self, key_id: str) -> Ed25519PublicKey:
        if self._pubkey_pem:
            return serialization.load_pem_public_key(self._pubkey_pem)
        p = self.keys_dir / f"{self._check_safe_id(key_id, 'key_id')}.pub"
        if not p.exists():
            raise OtaError("pubkey_missing", f"无该 key_id 的公钥: {key_id}")
        return serialization.load_pem_public_key(p.read_bytes())

    def _verify_signature(self, signs: dict, key_id: str) -> bool:
        if signs.get("key_id") != key_id:
            return False
        if not all(k in signs for k in (*SIGNED_FIELDS, "signature")):
            return False
        unsigned = {k: signs[k] for k in SIGNED_FIELDS}
        try:
            self._load_pubkey(key_id).verify(base64.b64decode(signs["signature"]),
                                             canonical(unsigned))
            return True
        except Exception:
            return False

    # -- 展开（防路径穿越：root 进程解不可信 tar 是 RCE 面） --
    @staticmethod
    def _safe_members(tar: tarfile.TarFile, dest: str):
        dest_real = os.path.realpath(dest)
        out = []
        for m in tar.getmembers():
            name = m.name
            if not name or name.startswith("/") or ".." in Path(name).parts:
                raise OtaError("unsafe_member", f"非法成员名: {name!r}")
            target = os.path.realpath(os.path.join(dest_real, name))
            if not (target == dest_real or target.startswith(dest_real + os.sep)):
                raise OtaError("unsafe_member", f"成员越界: {name!r}")
            out.append(m)
        return out

    def _extract(self, tgz: str, staging: str) -> None:
        os.makedirs(staging, exist_ok=True)
        with tarfile.open(tgz, "r:*") as tar:
            members = self._safe_members(tar, staging)
            payload = [m for m in members if m.name.startswith("payload/")]
            manifest = [m for m in members if m.name == "RELEASE_MANIFEST.json"]
            if not manifest:
                raise OtaError("manifest_missing", "包内缺 RELEASE_MANIFEST.json")
            chosen = manifest + payload
            for m in chosen:
                if m.name == "RELEASE_MANIFEST.json":
                    tar.extract(m, staging)
                else:
                    m.name = m.name[len("payload/"):]      # 去掉 payload/ 前缀
                    if not m.name:
                        continue
                    tar.extract(m, staging)

    def _verify_manifest(self, staging: str) -> bool:
        mp = os.path.join(staging, "RELEASE_MANIFEST.json")
        try:
            doc = json.loads(Path(mp).read_text(encoding="utf-8"))
        except Exception:
            return False
        files = doc.get("files_sha256") or {}
        if not files:
            return False
        for rel, want in files.items():
            fp = os.path.join(staging, rel)
            if not os.path.isfile(fp) or sha256_file(fp) != want:
                return False
        return True

    def _read_manifest(self, staging: str) -> dict | None:
        try:
            return json.loads(Path(os.path.join(staging, "RELEASE_MANIFEST.json")).read_text(encoding="utf-8"))
        except Exception:
            return None

    def _materialize_delta(self, staging: str, mdoc: dict, base_ver: str) -> None:
        """增量包 → 完整 staging：payload 里的新文件 + 当前 release 树里的未变文件。

        fail-closed：manifest 是 Ed25519 签过的（真实性已验），但 rel 仍拒绝 '..'；
        任何一个文件在两边都对不上 sha256 ⇒ 整包失败，绝不带病浇筑。
        """
        cur_tree = os.path.join(self.releases_dir, base_ver)
        files = mdoc.get("files_sha256") or {}
        if not files:
            raise OtaError("manifest_mismatch", "增量包 manifest 缺 files_sha256")
        full = staging + ".full"
        os.makedirs(full, exist_ok=True)
        for rel, want in files.items():
            if rel.startswith("/") or ".." in rel.split("/"):
                raise OtaError("manifest_mismatch", f"manifest 出现非法路径: {rel}")
            dst = os.path.join(full, rel)
            os.makedirs(os.path.dirname(dst) or full, exist_ok=True)
            src_payload = os.path.join(staging, rel)
            if os.path.isfile(src_payload) and sha256_file(src_payload) == want:
                shutil.copyfile(src_payload, dst)
                continue
            src_cur = os.path.join(cur_tree, rel)
            if os.path.isfile(src_cur) and sha256_file(src_cur) == want:
                shutil.copyfile(src_cur, dst)
                continue
            raise OtaError("delta_base_mismatch", f"增量合并缺文件（payload 与当前树都没有/哈希不符）: {rel}")
        # manifest 自身不在 files_sha256 清单里，必须单独带上（2026-09-19 板端实测：
        # 漏带 ⇒ ⑥ 全量复验读不到 manifest ⇒ manifest_mismatch）
        manifest_src = os.path.join(staging, "RELEASE_MANIFEST.json")
        if os.path.isfile(manifest_src):
            shutil.copyfile(manifest_src, os.path.join(full, "RELEASE_MANIFEST.json"))
        shutil.rmtree(staging, ignore_errors=True)
        os.rename(full, staging)

    # -- 主流程 --
    def run(self, url: str, key_id: str = DEFAULT_KEY_ID, version: str | None = None) -> int:
        # ① scheme 白名单
        u = urllib.parse.urlparse(url)
        if u.scheme != "https":
            return self._fail("scheme_rejected", f"non-https URL: {u.scheme or '<空>'}")

        self._progress(5, "下载更新包")
        work = tempfile.mkdtemp(prefix="ttbox-ota-", dir="/var/tmp" if os.path.isdir("/var/tmp") else None)
        staging = ""
        try:
            tgz = os.path.join(work, "pkg.tgz")
            # ② 下载（仅 https）
            self.fetch(url, tgz)
            # ②b 旁车签名
            sign_path = os.path.join(work, "pkg.tgz.sign.json")
            self.fetch(url + ".sign.json", sign_path)
            self._progress(30, "下载完成，校验签名")
            try:
                signs = json.loads(Path(sign_path).read_text(encoding="utf-8"))
            except Exception as e:
                return self._fail("sign_unreadable", f"旁车签名不可解析: {e!r}")

            # ③ 完整性（and 的前半）
            actual = sha256_file(tgz)
            if actual != signs.get("sha256"):
                return self._fail("sha256_mismatch", "package digest != 记录 sha256")
            # ④ 真实性（and 的后半）
            if not self._verify_signature(signs, key_id):
                return self._fail("signature_invalid", "Ed25519 验签失败")

            ver = str(version or signs.get("version") or "").strip()
            if not ver:
                return self._fail("version_missing", "签名记录缺 version")
            # ★ 任务文件的 version 未消毒就拼 releases 路径 ⇒ "../../x" 可把 staging
            #   建到发布树之外（对比 _safe_members 对成员名做了穿越拒绝）。
            try:
                ver = self._check_safe_id(ver, "version")
            except OtaError as e:
                return self._fail(e.state, e.detail)
            self._progress(50, "校验通过，展开更新包", ver)
            # 命名注意（2026-09-18 板端实测）：release_install 的浇筑目标也是
            # releases/<ver>.staging —— 两者绝不能同名（否则其 tar 管道自拷自，
            # RELEASE_MANIFEST.json 会在拷贝中消失）。故本目录用 <ver>.ota.staging。
            staging = os.path.join(self.releases_dir, f"{ver}.ota.staging")

            # ⑤ 展开（仅双因子都过之后）
            try:
                self._extract(tgz, staging)
            except OtaError as e:
                shutil.rmtree(staging, ignore_errors=True)
                return self._fail(e.state, e.detail)
            # ⑤b 增量包（2026-09-19）：base 必须恰好等于当前版本，合并成完整树再走原流程
            mdoc = self._read_manifest(staging)
            if mdoc and str(mdoc.get("version") or "").strip():
                # 包内 manifest 的 version 是安装版本号的唯一真源（2026-09-20 板端教训：
                # 面板发起的 delta 安装 job 无 version 字段，从旁车签名取到
                # "1.5.6-delta-from-1.5.5" ⇒ releases 目录名被污染）。
                ver = str(mdoc["version"]).strip()
                try:
                    ver = self._check_safe_id(ver, "manifest version")
                except OtaError as e:
                    shutil.rmtree(staging, ignore_errors=True)
                    return self._fail(e.state, e.detail)
            if mdoc and mdoc.get("delta"):
                base = str(mdoc.get("base_version") or "").strip()
                cur0 = current_version()
                if base != cur0:
                    shutil.rmtree(staging, ignore_errors=True)
                    return self._fail("delta_base_mismatch",
                                      f"增量包基线 {base or '<缺>'} != 当前版本 {cur0 or '<未知>'}，请在面板改用全量包")
                self._progress(55, "校验通过，合并增量", ver)
                try:
                    self._materialize_delta(staging, mdoc, base)
                except OtaError as e:
                    shutil.rmtree(staging, ignore_errors=True)
                    return self._fail(e.state, e.detail)
            # ⑥ 全量复验
            if not self._verify_manifest(staging):
                shutil.rmtree(staging, ignore_errors=True)
                return self._fail("manifest_mismatch", "staging sha256 != RELEASE_MANIFEST")
            # ⑥b 降级拒绝（§2.4）：包必须新于当前版本；回滚路径不经过这里（天然豁免）
            cur = current_version()
            if is_downgrade(ver, cur):
                shutil.rmtree(staging, ignore_errors=True)
                return self._fail("downgrade_rejected",
                                  f"包版本 {ver} 不高于当前版本 {cur or '<未知>'}（禁止降级）")
            # ⑦ 原子发布（先规范化权限：打包机的 mode/uid 不可信，见函数 docstring）
            normalize_staging_perms(staging)
            self._progress(65, "安装新版本", ver)
            rc = self.install(staging, ver)
            if rc != 0:
                shutil.rmtree(staging, ignore_errors=True)
                return self._fail("install_failed", f"release_install rc={rc}")
            # ⑦b 健康检查（业务能力；失败自动回滚——保命路径，不受降级限制）
            self._progress(85, "重启服务，健康检查", ver)
            if not self.health(HEALTH_TIMEOUT_S):
                self.rollback()
                shutil.rmtree(staging, ignore_errors=True)
                return self._fail("health_check_failed", "已 rollback")
            self._write_status({"state": "SUCCESS", "progress": 100, "version": ver,
                                "sha256": actual, "key_id": key_id})
            return 0
        except OtaError as e:
            return self._fail(e.state, e.detail)
        except Exception as e:  # 未分类异常一律判失败（绝不静默放行）
            return self._fail("unexpected", repr(e))
        finally:
            shutil.rmtree(work, ignore_errors=True)   # §0.2：无论成败，临时目录零残留
            if staging:
                # 更新后删包/清树（2026-09-30 补）：成功路径此前漏清展开树，板端实测
                # 残留 <ver>.ota.staging 占 12M；release_install 的 list_versions 又明确
                # 排除 *.staging ⇒ 无人清理、逐版累积。改为与失败分支同口径：成败皆清。
                shutil.rmtree(staging, ignore_errors=True)


# ---------------------------------------------------------------- 任务文件模式
def process_jobs(jobs_dir: str, updater_factory=OtaUpdater) -> int:
    """消费任务目录里的 *.json（web 写入；本进程以 root 身份跑）。

    每个任务处理完**必须移走**（否则 path 单元会因文件仍在而再次触发）：
    成功 → processed/，失败 → failed/（都留审计）。返回值：全部成功 0，任一失败 1。
    """
    jp = Path(jobs_dir)
    if not jp.is_dir():
        sys.stderr.write(f"[ota] 任务目录不存在: {jobs_dir}\n")
        return 0
    done_dir = jp / "processed"
    fail_dir = jp / "failed"
    jobs = sorted(jp.glob("*.json"))
    rc_all = 0
    for f in jobs:
        stamp = time.strftime("%Y%m%d-%H%M%S")
        try:
            job = json.loads(f.read_text(encoding="utf-8"))
            url = str(job.get("url") or "").strip()
            key_id = str(job.get("key_id") or DEFAULT_KEY_ID).strip()
            if not url:
                raise ValueError("任务文件缺 url")
            rc = updater_factory().run(url, key_id, job.get("version"))
        except Exception as e:  # 任务文件本身坏 ⇒ 记失败，绝不留在原地造成触发循环
            sys.stderr.write(f"[ota] 任务文件不可用: {f.name}: {e!r}\n")
            updater_factory()._write_status(
                {"state": "FAILED", "error": "job_invalid", "detail": repr(e)})
            rc = 1
        dest_dir = done_dir if rc == 0 else fail_dir
        if rc != 0:
            rc_all = 1
        try:
            dest_dir.mkdir(parents=True, exist_ok=True)
            f.rename(dest_dir / f"{stamp}-{f.name}")
        except Exception as e:
            sys.stderr.write(f"[warn] 任务文件归档失败（原地删除以防触发循环）: {e!r}\n")
            try:
                f.unlink()
            except Exception:
                pass
    return rc_all


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description="TTBOX OTA 更新器（root 独立进程）")
    ap.add_argument("url", nargs="?", default=None,
                    help="https://…/ttbox-update-<ver>.tgz（省略时须给 --from-jobs）")
    ap.add_argument("key_id", nargs="?", default=DEFAULT_KEY_ID)
    ap.add_argument("--version", default=None)
    ap.add_argument("--from-jobs", dest="from_jobs", nargs="?", const=DEFAULT_JOBS_DIR,
                    default=None, metavar="JOBS_DIR",
                    help="任务文件模式：消费 <dir>/*.json（特权通道，§2.2）")
    a = ap.parse_args(argv)
    if a.from_jobs is not None:
        return process_jobs(a.from_jobs)
    if not a.url:
        ap.error("需要 url 或 --from-jobs")
    return OtaUpdater().run(a.url, a.key_id, a.version)


if __name__ == "__main__":
    sys.exit(main())
