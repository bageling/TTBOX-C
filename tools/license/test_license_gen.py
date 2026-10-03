#!/usr/bin/env python3
"""M2.01 单测：离线授权卡发卡工具（ttbox_license_gen.py）。

★ 注册模型（沿 t1.11 §1 注）：本文件是 **Python 侧**单测，**不并入 C++
  `ttbox_core_tests` ⇒ **不改变 `ctest -N` 计数**。

★ 跨语言契约锁：canonical_license() 的黄金串必须与 C++
  core/tests/test_license_card.cpp::license_card_canonical_golden 逐字节一致
  （任何单方改动 ⇒ 两侧签名/验签对不上 ⇒ 红灯逼对账）。

依赖全注入（每用例临时密钥），**不碰生产 .testkeys/ 生产公钥**。
"""
from __future__ import annotations

import base64
import json
import subprocess
import sys
import tempfile
from pathlib import Path

HERE = Path(__file__).resolve().parent
# append（不用 insert(0)：顶到 stdlib 前有遮蔽同名标准库的风险）
if str(HERE) not in sys.path:
    sys.path.append(str(HERE))

import ttbox_license_gen as gen  # noqa: E402
from cryptography.hazmat.primitives import serialization  # noqa: E402
from cryptography.hazmat.primitives.asymmetric.ed25519 import Ed25519PrivateKey  # noqa: E402

FAILS: list[str] = []


def check(name: str, cond: bool, detail: str = "") -> None:
    print(("  PASS: " if cond else "  FAIL: ") + name + (" | " + detail if detail else ""))
    if not cond:
        FAILS.append(name)


class Fixture:
    """临时密钥 + 临时 pub hex（不动生产 keys/.testkeys）。"""

    def __init__(self):
        self.dir = Path(tempfile.mkdtemp(prefix="lic-test-"))
        self.priv = Ed25519PrivateKey.generate()
        self.priv_pem = self.priv.private_bytes(
            encoding=serialization.Encoding.PEM,
            format=serialization.PrivateFormat.PKCS8,
            encryption_algorithm=serialization.NoEncryption())
        self.priv_path = self.dir / "test.priv.pem"
        self.priv_path.write_bytes(self.priv_pem)
        self.pub_hex = self.priv.public_key().public_bytes(
            encoding=serialization.Encoding.Raw,
            format=serialization.PublicFormat.Raw).hex()


def test_canonical_golden_matches_cpp():
    """跨语言契约锁：与 C++ test_license_card.cpp 黄金串逐字节一致。"""
    lic = {
        "license_id": "ttbox-lic-test-0001",
        "device": "TESTSERIAL0001",
        "plan": "subscription",
        "is_pro": True,
        "features": ["capture", "aim"],
        "ui_brand": "acme",
        "issued_at": 1750000000,
        "expires_at": 0,
    }
    got = gen.canonical_license(lic)
    golden = ("device\nTESTSERIAL0001\nexpires_at\n0\nfeatures\ncapture,aim\n"
              "is_pro\n1\nissued_at\n1750000000\nlicense_id\nttbox-lic-test-0001\n"
              "plan\nsubscription\nui_brand\nacme\n")
    check("canonical 黄金串 == C++ 黄金串", got == golden, got.replace("\n", "\\n"))


def test_issue_and_verify_roundtrip():
    fx = Fixture()
    card = gen.issue_card(priv_pem=fx.priv_path, device="ABC123DEF456",
                          plan="subscription", is_pro=True,
                          features=["aim", "ota"], ui_brand="vendor1",
                          license_id="lic-rt-0001", issued_at=1750000000,
                          expires_at=0, key_id="k-test")
    ok, why = gen.verify_card(card, fx.pub_hex)
    check("签发→验签往返", ok, why)
    check("字段投影 device", card["license"]["device"] == "ABC123DEF456")
    check("features 归一化闭集序", card["license"]["features"] == ["aim", "ota"],
          str(card["license"]["features"]))
    check("is_pro 编码", card["license"]["is_pro"] is True)


def test_tampered_card_rejected():
    fx = Fixture()
    card = gen.issue_card(priv_pem=fx.priv_path, device="ABC123DEF456",
                          plan="trial", is_pro=False, features=["capture"],
                          ui_brand="ttbox", license_id="lic-tamper-1",
                          issued_at=1750000000, expires_at=0, key_id="k-test")
    ok0, _ = gen.verify_card(card, fx.pub_hex)
    # 篡改 plan（签名覆盖 plan）
    bad = json.loads(json.dumps(card))
    bad["license"]["plan"] = "permanent"
    ok1, _ = gen.verify_card(bad, fx.pub_hex)
    # 篡改 features（加一个功能位）
    bad2 = json.loads(json.dumps(card))
    bad2["license"]["features"] = ["capture", "aim"]
    ok2, _ = gen.verify_card(bad2, fx.pub_hex)
    # 篡改 expires_at（延长有效期）
    bad3 = json.loads(json.dumps(card))
    bad3["license"]["expires_at"] = 1893456000
    ok3, _ = gen.verify_card(bad3, fx.pub_hex)
    check("原卡验过（前置）", ok0)
    check("篡改 plan ⇒ 拒", not ok1)
    check("篡改 features ⇒ 拒", not ok2)
    check("篡改 expires_at ⇒ 拒", not ok3)


def test_wrong_key_rejected():
    """签名有效但用另一把公钥验 ⇒ 拒（密钥分族的负控）。"""
    fx = Fixture()
    card = gen.issue_card(priv_pem=fx.priv_path, device="ABC123DEF456",
                          plan="none", is_pro=False, features=[],
                          ui_brand="ttbox", license_id="lic-wk-1",
                          issued_at=1750000000, expires_at=0, key_id="k-test")
    other = Ed25519PrivateKey.generate()
    other_hex = other.public_key().public_bytes(
        encoding=serialization.Encoding.Raw,
        format=serialization.PublicFormat.Raw).hex()
    ok, _ = gen.verify_card(card, other_hex)
    check("换公钥验 ⇒ 拒", not ok)


def test_issue_rejects_unknown_feature():
    fx = Fixture()
    try:
        gen.issue_card(priv_pem=fx.priv_path, device="ABC123DEF456",
                       plan="none", is_pro=False, features=["teleport"],
                       ui_brand="ttbox", license_id=None,
                       issued_at=1750000000, expires_at=0, key_id="k-test")
        check("闭集外 feature 拒发", False, "未抛 SystemExit")
    except SystemExit:
        check("闭集外 feature 拒发", True)


def test_issue_rejects_bad_charset():
    fx = Fixture()
    for bad_brand in ("bad brand", "x" * 33, "1st ok!"):
        try:
            gen.issue_card(priv_pem=fx.priv_path, device="ABC123DEF456",
                           plan="none", is_pro=False, features=[],
                           ui_brand=bad_brand, license_id=None,
                           issued_at=1750000000, expires_at=0, key_id="k-test")
            check(f"ui_brand {bad_brand!r} 拒发", False, "未抛 SystemExit")
        except SystemExit:
            check(f"ui_brand {bad_brand!r} 拒发", True)


def test_cli_issue_and_verify():
    """CLI 冒烟：issue-card → verify（同一临时钥族）。"""
    fx = Fixture()
    pub_path = gen.KEYS_DIR / "k-cli-test.pub.hex"
    pub_path.write_text(fx.pub_hex + "\n")
    try:
        out = subprocess.run(
            [sys.executable, str(HERE / "ttbox_license_gen.py"), "issue-card",
             "--device", "FFFF0000EEEE", "--plan", "permanent", "--pro",
             "--features", "capture,inference", "--brand", "cli-brand",
             "--days", "0", "--key-id", "k-cli-test", "--priv", str(fx.priv_path)],
            capture_output=True, text=True, check=True).stdout.strip()
        card = json.loads(out)
        check("CLI 卡 JSON 可解析", "signature" in card and "license" in card)
        tmp = fx.dir / "card.json"
        tmp.write_text(out)
        vout = subprocess.run(
            [sys.executable, str(HERE / "ttbox_license_gen.py"), "verify",
             str(tmp), "--key-id", "k-cli-test"],
            capture_output=True, text=True)
        check("CLI verify 通过", vout.returncode == 0, vout.stdout.strip())
    finally:
        pub_path.unlink(missing_ok=True)


# ==========================================================================
# M2.05：短码跨语言契约锁 + 校验位强度（镜像 core/tests/test_license_shortcode.cpp）
# ==========================================================================
def test_shortcode_cross_language_vector():
    """★ 跨语言契约锁：与 C++ test_license_shortcode.cpp::shortcode_cross_language_vector 同向量。

    向量：license_id "ttbox-lic-20260917-3842ff" ⇒ "TTB-006Z-0C33"。
    任何单方改动（FNV/Crockford/Luhn/格式）都会让本用例红灯 ⇒ 逼两侧对账。
    """
    got = gen.short_code("ttbox-lic-20260917-3842ff")
    check("short_code 向量 == TTB-006Z-0C33", got == "TTB-006Z-0C33", got)
    check("派生串长度 == 13（TTB-XXXX-XXXX）", len(got) == 13, str(len(got)))


def test_shortcode_single_char_substitution_rejected():
    """单字符替换必须 0 漏检（8 位 × 31 替代 = 248）。"""
    body = gen.short_code("ttbox-lic-20260917-3842ff").replace("TTB", "").replace("-", "")
    assert len(body) == 8
    total = 0
    undetected = []
    for i in range(8):
        for alt in gen.SHORT_CODE_ALPHABET:
            if alt == body[i]:
                continue
            total += 1
            mutant = body[:i] + alt + body[i + 1:]
            if gen.verify_short_code(f"TTB-{mutant[:4]}-{mutant[4:]}"):
                undetected.append((i, alt))
    check("单字符替换 0/248 漏检", total == 248 and not undetected,
          f"total={total} undetected={undetected}")


def test_shortcode_adjacent_transposition_blind_pair_registered():
    """相邻换位唯一漏检对 = {0,31}（值 0='0' ↔ 值 31='Z'，Luhn mod 32 已知边界）。"""
    body = gen.short_code("ttbox-lic-20260917-3842ff").replace("TTB", "").replace("-", "")
    alpha = gen.SHORT_CODE_ALPHABET
    undetected = []
    for i in range(7):
        if body[i] == body[i + 1]:
            continue
        sw = body[:i] + body[i + 1] + body[i] + body[i + 2:]
        if gen.verify_short_code(f"TTB-{sw[:4]}-{sw[4:]}"):
            undetected.append((i, tuple(sorted((alpha.index(body[i]), alpha.index(body[i + 1]))))))
    check("相邻换位仅 {0,31} 这一对漏检（已知边界，显式登记）",
          bool(undetected) and all(v == (0, 31) for _, v in undetected), str(undetected))


def test_shortcode_normalization_accepts_handwritten_forms():
    """人手抄写容错：大小写不敏感 / 可省分隔符 / 可省前缀 / O→0、I·L→1。"""
    code = gen.short_code("ttbox-lic-20260917-3842ff")      # TTB-006Z-0C33
    variants = [code, code.lower(), code.replace("-", ""), code[4:],
                "  " + code + "  "]
    for v in variants:
        check(f"归一接受 {v!r}", gen.verify_short_code(v), v)
    # 一位错（校验位改掉）⇒ 必须拒
    bad = code[:-1] + ("1" if code[-1] != "1" else "2")
    check("改校验位 ⇒ 拒", not gen.verify_short_code(bad), bad)


def test_shortcode_invalid_license_id_fails_closed():
    check("空 license_id ⇒ 空串", gen.short_code("") == "")
    check("超长(65) license_id ⇒ 空串", gen.short_code("a" * 65) == "")
    check("含非法字符 ⇒ 空串", gen.short_code("bad id") == "")


def test_shortcode_not_security_boundary():
    """★ 红线：verify **只查校验位、不查签名** —— 合法校验≠真卡（准入永远靠 Ed25519）。"""
    code = gen.short_code("ttbox-lic-20260917-3842ff")
    # 构造一个"校验位合法但并非任何真实卡派生"的短码：任取 7 位数据 + 补正确校验位。
    body7 = "ABCDEFG"
    chk = gen._luhn_mod32_check(body7)
    forged = f"TTB-{body7[:4]}-{body7[4:]}{chk}"
    check("伪造（校验位合法）短码仍被 verify 接受（证明非安全边界）",
          gen.verify_short_code(forged), forged)


def test_cli_shortcode_and_verify():
    """CLI 冒烟：short-code → verify-shortcode（exit 0/1）。"""
    out = subprocess.run(
        [sys.executable, str(HERE / "ttbox_license_gen.py"), "short-code",
         "ttbox-lic-20260917-3842ff"],
        capture_output=True, text=True)
    check("CLI short-code stdout == 向量", out.stdout.strip() == "TTB-006Z-0C33",
          out.stdout.strip())
    vok = subprocess.run(
        [sys.executable, str(HERE / "ttbox_license_gen.py"), "verify-shortcode",
         "TTB-006Z-0C33"], capture_output=True, text=True)
    check("CLI verify-shortcode 合法 ⇒ 0", vok.returncode == 0, vok.stdout.strip())
    vbad = subprocess.run(
        [sys.executable, str(HERE / "ttbox_license_gen.py"), "verify-shortcode",
         "TTB-006Z-0C34"], capture_output=True, text=True)
    check("CLI verify-shortcode 非法 ⇒ 1", vbad.returncode == 1, vbad.stdout.strip())


def main() -> int:
    tests = [v for k, v in sorted(globals().items()) if k.startswith("test_")]
    for t in tests:
        print(f"== {t.__name__} ==")
        t()
    print()
    if FAILS:
        print(f"FAILED: {len(FAILS)} 项")
        for f in FAILS:
            print(f"  - {f}")
        return 1
    print("ALL PASS")
    return 0


if __name__ == "__main__":
    sys.exit(main())
