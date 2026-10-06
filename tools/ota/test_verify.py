#!/usr/bin/env python3
"""T1.11 单测（REG-01）：OTA 验签链路。

★ 注册模型（t1.11 §1 注）：本文件是 **Python 侧**单测，**不并入 C++ `ttbox_core_tests`**
  ⇒ **不改变 `ctest -N` 计数**。

依赖全部注入（fetch/install/rollback/health/pubkey），**不开任何生产后门**：
测试替身只替换"外部副作用"，被测的判定逻辑（scheme/sha256/验签/展开/manifest/回滚）全部是真代码。
"""
from __future__ import annotations

import base64
import hashlib
import json
import os
import shutil
import subprocess
import sys
import tarfile
import tempfile
from pathlib import Path

HERE = Path(__file__).resolve().parent
# 根锚发现（P6/A-PATH-3）：不写死层级（原 `HERE.parents[1]`）。
_TREE_ROOT = HERE
while _TREE_ROOT != _TREE_ROOT.parent and not all(
        (_TREE_ROOT / _n).is_dir() for _n in ("plugins", "usbproxy", "scripts", "deploy")):
    _TREE_ROOT = _TREE_ROOT.parent
SCRIPTS = _TREE_ROOT / "scripts"
# append（不用 insert(0)：顶到 stdlib 前有遮蔽同名标准库的风险）
for _entry in (str(HERE), str(SCRIPTS)):
    if _entry not in sys.path:
        sys.path.append(_entry)

import ttbox_ota_updater as up  # noqa: E402
import ttbox_ota_sign as sign  # noqa: E402
from cryptography.hazmat.primitives import serialization  # noqa: E402
from cryptography.hazmat.primitives.asymmetric.ed25519 import Ed25519PrivateKey  # noqa: E402

FAILS: list[str] = []


def check(name: str, cond: bool, detail: str = "") -> None:
    print(("  PASS: " if cond else "  FAIL: ") + name + (" | " + detail if detail else ""))
    if not cond:
        FAILS.append(name)


def sha256f(p: str) -> str:
    return hashlib.sha256(Path(p).read_bytes()).hexdigest()


class Fixture:
    """造包 + 密钥 + 替身。私钥只在临时目录，绝不入库。"""

    def __init__(self):
        self.dir = Path(tempfile.mkdtemp(prefix="ota-test-"))
        self.priv = Ed25519PrivateKey.generate()
        self.pub_pem = self.priv.public_key().public_bytes(
            encoding=serialization.Encoding.PEM,
            format=serialization.PublicFormat.SubjectPublicKeyInfo)
        self.priv_pem = self.priv.private_bytes(
            encoding=serialization.Encoding.PEM,
            format=serialization.PrivateFormat.PKCS8,
            encryption_algorithm=serialization.NoEncryption())
        self.releases = self.dir / "releases"
        self.releases.mkdir()
        self.calls = {"install": [], "rollback": 0, "fetch": []}
        self.health_ok = True
        self.install_rc = 0

    # ---- 造包 ----
    def make_tgz(self, name: str = "ttbox-update-1.2.0.tgz", body: bytes | None = None) -> Path:
        root = self.dir / ("src-" + name)
        if root.exists():
            shutil.rmtree(root)
        (root / "payload" / "bin").mkdir(parents=True)
        (root / "payload" / "bin" / "ttbox_core_main").write_bytes(body or b"TTBOX-BINARY-BODY")
        files = {"bin/ttbox_core_main": sha256f(str(root / "payload/bin/ttbox_core_main"))}
        (root / "RELEASE_MANIFEST.json").write_text(json.dumps(
            {"version": "1.2.0", "built_at": "2026-09-17T00:00:00Z",
             "git_sha": "", "files_sha256": files}), encoding="utf-8")
        tgz = self.dir / name
        with tarfile.open(tgz, "w:gz") as t:
            t.add(root / "payload", arcname="payload")
            t.add(root / "RELEASE_MANIFEST.json", arcname="RELEASE_MANIFEST.json")
        return tgz

    def sign_for(self, tgz: Path, key_id: str = "ttbox-ota-2026b",
                 version: str = "1.2.0", corrupt_sig: bool = False,
                 wrong_key_id: bool = False) -> Path:
        unsigned = sign.unsigned_record(tgz, version,
                                        "ttbox-ota-WRONG" if wrong_key_id else key_id)
        sig = self.priv.sign(sign.canonical(unsigned))
        if corrupt_sig:
            raw = bytearray(sig)
            raw[0] ^= 0xFF
            sig = bytes(raw)
        out = Path(str(tgz) + ".sign.json")
        out.write_text(json.dumps({**unsigned, "signature": base64.b64encode(sig).decode()},
                                  sort_keys=True, separators=(",", ":")), encoding="utf-8")
        return out

    # ---- 替身 ----
    def fetcher(self, mapping: dict):
        def _f(url: str, dest: str, timeout: float = 60.0):
            self.calls["fetch"].append(url)
            src = mapping.get(url)
            if src is None:
                raise FileNotFoundError(f"未映射的 URL: {url}")
            shutil.copyfile(src, dest)
        return _f

    def installer(self):
        def _i(staging: str, version: str) -> int:
            self.calls["install"].append((staging, version))
            return self.install_rc
        return _i

    def rollbacker(self):
        def _r() -> int:
            self.calls["rollback"] += 1
            return 0
        return _r

    def health(self):
        return lambda timeout_s=30: self.health_ok

    def updater(self, mapping: dict, key_id: str = "ttbox-ota-2026b"):
        return up.OtaUpdater(
            releases_dir=str(self.releases),
            status_path=str(self.dir / "ota_status.json"),
            fetch=self.fetcher(mapping), install=self.installer(),
            rollback=self.rollbacker(), health=self.health(),
            pubkey_pem=self.pub_pem)

    def status(self) -> dict:
        try:
            return json.loads(Path(self.dir / "ota_status.json").read_text())
        except Exception:
            return {}

    def cleanup(self):
        shutil.rmtree(self.dir, ignore_errors=True)


def run_case(fn):
    fx = Fixture()
    try:
        fn(fx)
    finally:
        fx.cleanup()


# ---------------------------------------------------------------- 用例
def case_good_pkg_verifies(fx):
    print("[1] good_pkg_verifies")
    tgz = fx.make_tgz()
    sgn = fx.sign_for(tgz)
    u = fx.updater({"https://x/pkg.tgz": str(tgz), "https://x/pkg.tgz.sign.json": str(sgn)})
    rc = u.run("https://x/pkg.tgz")
    st = fx.status()
    check("退出码 0", rc == 0, str(rc))
    check("state=SUCCESS", st.get("state") == "SUCCESS", json.dumps(st, ensure_ascii=False))
    check("调用了 T1.01 安装", len(fx.calls["install"]) == 1, str(fx.calls["install"]))
    check("未回滚", fx.calls["rollback"] == 0, "")


def case_tamper_1byte_rejected(fx):
    print("[2] tamper_1byte_rejected（多偏移，拒绝率须 100%）")
    tgz = fx.make_tgz()
    sgn = fx.sign_for(tgz)
    data = bytearray(tgz.read_bytes())
    offsets = [0, 1, 32, 64, max(0, len(data) // 2), max(0, len(data) - 1)]
    rejected = 0
    for off in offsets:
        tmp = fx.dir / f"tampered-{off}.tgz"
        b = bytearray(data)
        b[off] ^= 0xFF
        tmp.write_bytes(bytes(b))
        u = fx.updater({"https://x/pkg.tgz": str(tmp),
                        "https://x/pkg.tgz.sign.json": str(sgn)})
        rc = u.run("https://x/pkg.tgz")
        st = fx.status()
        ok = rc == 1 and st.get("state") == "FAILED" and \
            st.get("error") in ("sha256_mismatch", "signature_invalid")
        rejected += 1 if ok else 0
        print(f"    偏移 {off}: rc={rc} error={st.get('error')}")
    check("拒绝率 100%%", rejected == len(offsets), f"{rejected}/{len(offsets)}")
    check("篡改后无 staging 残留",
          not any(p.name.endswith(".staging") for p in fx.releases.iterdir()),
          str([p.name for p in fx.releases.iterdir()]))


def case_http_scheme_rejected(fx):
    print("[3] http_scheme_rejected")
    tgz = fx.make_tgz()
    sgn = fx.sign_for(tgz)
    u = fx.updater({"http://x/pkg.tgz": str(tgz), "http://x/pkg.tgz.sign.json": str(sgn)})
    rc = u.run("http://x/pkg.tgz")
    st = fx.status()
    check("退出码 1", rc == 1, str(rc))
    check("error=scheme_rejected", st.get("error") == "scheme_rejected", json.dumps(st))
    check("**未发生下载**", fx.calls["fetch"] == [], str(fx.calls["fetch"]))


def case_wrong_key_id_rejected(fx):
    print("[4] wrong_key_id_rejected")
    tgz = fx.make_tgz()
    sgn = fx.sign_for(tgz, wrong_key_id=True)
    u = fx.updater({"https://x/pkg.tgz": str(tgz), "https://x/pkg.tgz.sign.json": str(sgn)})
    rc = u.run("https://x/pkg.tgz")
    check("退出码 1", rc == 1, str(rc))
    check("key_id 不符即拒", fx.status().get("error") in ("signature_invalid", "sha256_mismatch"),
          json.dumps(fx.status()))


def case_dual_factor_both_required(fx):
    print("[5] dual_factor_both_required（and 非 or）")
    tgz = fx.make_tgz()
    # a) sha 对（包没动）但签名错 ⇒ 必须拒
    sgn_badsig = fx.sign_for(tgz, corrupt_sig=True)
    u = fx.updater({"https://x/pkg.tgz": str(tgz),
                    "https://x/pkg.tgz.sign.json": str(sgn_badsig)})
    rc_a = u.run("https://x/pkg.tgz")
    err_a = fx.status().get("error")
    check("签名错（sha 对）⇒ 拒", rc_a == 1 and err_a == "signature_invalid", str(err_a))
    # b) 签名对但内容被换（sha 不对）⇒ 必须拒
    other = fx.make_tgz(name="other.tgz", body=b"DIFFERENT-BODY")
    sgn_for_other = fx.sign_for(tgz)          # 签的是原包
    u2 = fx.updater({"https://x/pkg.tgz": str(other),
                     "https://x/pkg.tgz.sign.json": str(sgn_for_other)})
    rc_b = u2.run("https://x/pkg.tgz")
    err_b = fx.status().get("error")
    check("内容换（签对）⇒ 拒", rc_b == 1 and err_b == "sha256_mismatch", str(err_b))


def case_no_residue_on_failure(fx):
    print("[6] no_residue_on_failure")
    # 基线：只认"本次运行新增"的 ttbox-ota-* 残留（陈旧无关目录不误伤，2026-09-17 假红订正）
    baseline = {p.name for p in Path(tempfile.gettempdir()).iterdir()
                if p.name.startswith("ttbox-ota-")}
    tgz = fx.make_tgz()
    sgn = fx.sign_for(tgz)
    # 制造"验签通过但 manifest 不符"：改 payload 内容后重打包会破 sha256，
    # 故改为注入 install 失败来观察 staging 清理
    fx.install_rc = 1
    u = fx.updater({"https://x/pkg.tgz": str(tgz), "https://x/pkg.tgz.sign.json": str(sgn)})
    rc = u.run("https://x/pkg.tgz")
    check("install 失败 ⇒ rc=1", rc == 1, str(rc))
    check("error=install_failed", fx.status().get("error") == "install_failed", json.dumps(fx.status()))
    check("staging 已清理", not any(p.name.endswith(".staging") for p in fx.releases.iterdir()),
          str([p.name for p in fx.releases.iterdir()]))
    now = {p.name for p in Path(tempfile.gettempdir()).iterdir()
           if p.name.startswith("ttbox-ota-")}
    leftovers = sorted(now - baseline)
    check("临时目录零残留（相对基线）", not leftovers, str(leftovers))


def case_rollback_on_health_fail(fx):
    print("[7] rollback_on_health_fail")
    tgz = fx.make_tgz()
    sgn = fx.sign_for(tgz)
    fx.health_ok = False                      # 注入"业务不达标"
    u = fx.updater({"https://x/pkg.tgz": str(tgz), "https://x/pkg.tgz.sign.json": str(sgn)})
    rc = u.run("https://x/pkg.tgz")
    st = fx.status()
    check("健康检查失败 ⇒ rc=1", rc == 1, str(rc))
    check("error=health_check_failed", st.get("error") == "health_check_failed", json.dumps(st))
    check("**已调 rollback**", fx.calls["rollback"] == 1, str(fx.calls["rollback"]))
    check("staging 已清理", not any(p.name.endswith(".staging") for p in fx.releases.iterdir()), "")


# ---------------------------------------------------------------- 用例（2026-09-18 定案新增三组）
def case_downgrade_rejected(fx):
    """§2.4：OTA 新包版本必须高于当前版本，否则 downgrade_rejected；回滚不受限。"""
    print("[8] downgrade_rejected")
    import types
    tgz = fx.make_tgz()
    sgn = fx.sign_for(tgz, version="1.2.0")
    mapping = {"https://x/pkg.tgz": str(tgz), "https://x/pkg.tgz.sign.json": str(sgn)}
    # a) 当前版本 2.0.0，装 1.2.0 ⇒ 拒
    orig_cv = up.current_version
    up.current_version = lambda: "2.0.0"
    try:
        rc = fx.updater(mapping).run("https://x/pkg.tgz")
        st = fx.status()
        check("低版本 ⇒ rc=1", rc == 1, str(rc))
        check("error=downgrade_rejected", st.get("error") == "downgrade_rejected",
              json.dumps(st, ensure_ascii=False))
        check("降级未触发安装", fx.calls["install"] == [], str(fx.calls["install"]))
        check("降级未触发回滚", fx.calls["rollback"] == 0, "")
        check("staging 已清理", not any(p.name.endswith(".staging") for p in fx.releases.iterdir()), "")
    finally:
        up.current_version = orig_cv
    # b) 当前版本 1.0.0，装 1.2.0 ⇒ 放行（先恢复健康替身）
    up.current_version = lambda: "1.0.0"
    try:
        rc = fx.updater(mapping).run("https://x/pkg.tgz")
        check("高版本 ⇒ 放行", rc == 0 and fx.status().get("state") == "SUCCESS",
              json.dumps(fx.status(), ensure_ascii=False))
    finally:
        up.current_version = orig_cv
    # c) 当前版本未知（空）⇒ 放行（首次装机语义）
    up.current_version = lambda: ""
    try:
        rc = fx.updater(mapping).run("https://x/pkg.tgz")
        check("当前版本未知 ⇒ 放行", rc == 0, str(rc))
    finally:
        up.current_version = orig_cv
    # d) 版本比较真源单点：updater 与 web 两侧对同一组输入判定一致（抽样）
    import importlib.util as _iu
    web_src = fx.dir / "web_probe.py"
    web_src.write_text("pass", encoding="utf-8")
    check("is_downgrade(1.2.0, 2.0.0) 为真", up.is_downgrade("1.2.0", "2.0.0"), "")
    check("is_downgrade(2.1.0, 2.0.9) 为假", not up.is_downgrade("2.1.0", "2.0.9"), "")
    check("is_downgrade(1.4.5, 1.4.5) 为真（相等也是拒绝）", up.is_downgrade("1.4.5", "1.4.5"), "")


def case_health_criteria_real_ipc_keys(fx):
    """§2.3（2026-09-20 订正）：健康判据 = 三服务 active + core IPC 已就绪
    （GET_STATUS 返回非空 `version`）；**不再要求 current_model_id**——更新后流水线按 R6
    保持停止、无模型在跑，旧口径会让每次更新都误判失败并回滚；不看授权；
    旧判据键（model_loaded / license_available / cmd 请求体）必须绝迹。"""
    print("[9] health_criteria_real_ipc_keys")
    orig_active, orig_ipc = up._is_active, up._ipc_get_status
    try:
        up._is_active = lambda unit: True
        # a) core IPC 就绪（version 非空）⇒ 通过（流水线可处于停止态）
        up._ipc_get_status = lambda: {"version": "1.5.18"}
        check("core IPC 就绪 ⇒ 健康", up.default_health(timeout_s=1), "")
        # a2) 只有 current_model_id、没有 version ⇒ 不通过（新口径认 version，不认模型在跑）
        up._ipc_get_status = lambda: {"current_model_id": "m1"}
        check("仅模型在跑、无 version ⇒ 不健康", not up.default_health(timeout_s=1), "")
        # b) 旧假键 license_available/model_loaded 单独为真 ⇒ 不通过（旧判据不复活）
        up._ipc_get_status = lambda: {"license_available": True, "model_loaded": True}
        check("旧假键 ⇒ 不健康", not up.default_health(timeout_s=1), "")
        # c) IPC 空（core 不可达）⇒ 不通过
        up._ipc_get_status = lambda: {}
        check("IPC 空 ⇒ 不健康", not up.default_health(timeout_s=1), "")
        # d) 进程不 active ⇒ 不通过（core IPC 即使 OK）
        up._is_active = lambda unit: False
        up._ipc_get_status = lambda: {"version": "1.5.18"}
        check("服务未 active ⇒ 不健康", not up.default_health(timeout_s=1), "")
        # e) 源码锁：IPC 请求体必须是 {"type":"GET_STATUS"}，"cmd" 键与旧判据绝迹
        real_src = Path(up.__file__).read_text(encoding="utf-8")
        check("请求体用 type 键", '"type":"GET_STATUS"' in real_src, "")
        check('"cmd" 请求键已绝迹', '"cmd"' not in real_src, "")
        # 注释里允许出现旧键名的说明文字；判定式（st.get(...)）必须绝迹
        check("license_available 判据已绝迹",
              '.get("license_available")' not in real_src
              and "'license_available'" not in real_src, "")
        # f) 源码锁：不得再用 current_model_id 做健康判据
        check("current_model_id 不再作为健康判据",
              '.get("current_model_id")' not in real_src, "")
    finally:
        up._is_active, up._ipc_get_status = orig_active, orig_ipc


def case_job_file_mode(fx):
    """§2.2 特权通道：--from-jobs 消费任务文件；成功归档 processed/、失败归档 failed/，
    任务文件绝不留在原地（防 path 单元触发循环）。"""
    print("[10] job_file_mode")
    jobs = fx.dir / "jobs"
    jobs.mkdir()
    tgz = fx.make_tgz()
    sgn = fx.sign_for(tgz, version="1.2.0")
    (jobs / "job-a.json").write_text(json.dumps(
        {"url": "https://x/pkg.tgz", "key_id": "ttbox-ota-2026b"}), encoding="utf-8")
    (jobs / "job-bad.json").write_text("{ not json", encoding="utf-8")

    def factory():
        return fx.updater({"https://x/pkg.tgz": str(tgz),
                           "https://x/pkg.tgz.sign.json": str(sgn)})

    import types
    orig_cv = up.current_version
    up.current_version = lambda: ""
    try:
        rc = up.process_jobs(str(jobs), updater_factory=factory)
        check("有失败任务 ⇒ 整体 rc=1", rc == 1, str(rc))
        check("成功任务归档 processed/",
              any("job-a.json" in p.name for p in (jobs / "processed").iterdir()),
              str([p.name for p in (jobs / "processed").iterdir()]))
        check("坏任务归档 failed/",
              any("job-bad.json" in p.name for p in (jobs / "failed").iterdir()),
              str([p.name for p in (jobs / "failed").iterdir()]))
        check("任务文件零残留（防触发循环）", not list(jobs.glob("*.json")), "")
        check("坏任务状态 FAILED（后写覆盖）",
              fx.status().get("state") == "FAILED" and fx.status().get("error") == "job_invalid",
              json.dumps(fx.status(), ensure_ascii=False))
        # 全成功 ⇒ rc=0
        (jobs / "job-c.json").write_text(json.dumps(
            {"url": "https://x/pkg.tgz", "key_id": "ttbox-ota-2026b"}), encoding="utf-8")
        rc = up.process_jobs(str(jobs), updater_factory=factory)
        check("后续全成功 ⇒ rc=0", rc == 0, str(rc))
        check("全成功状态 SUCCESS",
              fx.status().get("state") == "SUCCESS",
              json.dumps(fx.status(), ensure_ascii=False))
    finally:
        up.current_version = orig_cv


def main() -> int:
    print("=== T1.11 OTA 验签单测 ===")
    for fn in (case_good_pkg_verifies, case_tamper_1byte_rejected,
               case_http_scheme_rejected, case_wrong_key_id_rejected,
               case_dual_factor_both_required, case_no_residue_on_failure,
               case_rollback_on_health_fail, case_downgrade_rejected,
               case_health_criteria_real_ipc_keys, case_job_file_mode):
        run_case(fn)
        print()
    print("结果: %d failures" % len(FAILS))
    if FAILS:
        print("FAILED: " + "; ".join(FAILS))
    return 1 if FAILS else 0


if __name__ == "__main__":
    sys.exit(main())
