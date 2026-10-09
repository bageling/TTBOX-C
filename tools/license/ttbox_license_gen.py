#!/usr/bin/env python3
"""TTBOX 离线授权卡发卡工具（Ed25519 · M2.01，商户侧）。

卡格式（与 core/src/auth/LicenseCard.hpp 的契约逐字节对齐）：
  {
    "license": {
      "license_id": "ttbox-lic-2026-0001",   # [A-Za-z0-9_-]{1,64}
      "device":     "<cpu_serial>",           # = 板端 DeviceFingerprint.bind_string()
      "plan":       "none|trial|subscription|permanent",
      "is_pro":     true,
      "features":   ["capture","inference","aim","ota"],  # 闭集子集
      "ui_brand":   "acme",                   # [A-Za-z0-9_-] ≤32
      "issued_at":  1758096000,               # unix 秒
      "expires_at": 0                         # unix 秒；0 = 永久
    },
    "key_id":    "ttbox-license-2026a",
    "signature": "<base64(64B Ed25519)>"
  }

★ canonical 串（两侧逐字节一致；C++ 侧 = core/src/auth/LicenseCard.cpp
  license_canonical()）：按字段名字母序 "key\\nvalue\\n" 拼接；features 按闭集序
  逗号连接；is_pro 用 1/0；时间戳十进制。所有值来自受限字符集 ⇒ 无 JSON 转义歧义。

★ 密钥纪律（沿 OTA 工具 ttbox_ota_sign.py）：私钥落 .testkeys/（gitignore，不入库），
  仓库只有公钥（keys/<key_id>.pub.hex）+ core 内嵌（LicenseKeys.hpp）。
  ★ 换钥流程：gen-key → emit-c-header 重生成 LicenseKeys.hpp → 提交
  （旧 key_id 卡自动失效：core 侧 key_id 不匹配 ⇒ kInvalidCard，fail-closed）。

★ License 与 OTA 分用独立密钥对（路线 §1.3 裁决 1：授权面与固件面解耦）。
"""
from __future__ import annotations

import argparse
import base64
import json
import secrets
import sys
import time
from pathlib import Path

try:
    from cryptography.hazmat.primitives import serialization
    from cryptography.hazmat.primitives.asymmetric.ed25519 import Ed25519PrivateKey, Ed25519PublicKey
except ImportError:  # pragma: no cover
    sys.stderr.write("需要 cryptography：pip install cryptography\n")
    raise

DEFAULT_KEY_ID = "ttbox-license-2026a"
TOOLS_DIR = Path(__file__).resolve().parent
KEYS_DIR = TOOLS_DIR / "keys"            # 公钥（入库）
TESTKEYS_DIR = TOOLS_DIR / ".testkeys"   # 私钥（gitignore，不入库）

# ★ 闭集：必须与 core/src/auth/LicenseStateMachine.hpp known_features() 一字不差。
KNOWN_FEATURES = ["capture", "inference", "aim", "ota"]
KNOWN_PLANS = ["none", "trial", "subscription", "permanent"]

# canonical 字段序（字母序；两侧唯一权威，勿单方改）
CANON_FIELDS = ("device", "expires_at", "features", "is_pro",
               "issued_at", "license_id", "plan", "ui_brand")


def canonical_license(rec: dict) -> str:
    """canonical 签名原文。rec 须为**已归一化**字段（issue_card 产物）。

    与 C++ core/src/auth/LicenseCard.cpp::license_canonical() 逐字节一致——
    两侧任何单方改动都会导致签名失效（fail-closed，好事），但请同步两侧。
    """
    parts = []
    for k in CANON_FIELDS:
        v = rec[k]
        if k == "features":
            v = ",".join(v)
        elif k == "is_pro":
            v = "1" if v else "0"
        elif k in ("expires_at", "issued_at"):
            v = str(int(v))
        else:
            v = str(v)
        parts.append(k + "\n" + v + "\n")
    return "".join(parts)


def _check_id(name: str, value: str, max_len: int = 64) -> str:
    if not value or len(value) > max_len:
        raise SystemExit(f"{name} 非法：1~{max_len} 字符（实得 {value!r}）")
    for ch in value:
        if not (ch.isascii() and (ch.isalnum() or ch in "_-")):
            raise SystemExit(f"{name} 非法：只允许 [A-Za-z0-9_-]（实得 {value!r}）")
    return value


def _check_brand(brand: str) -> str:
    # 与 C++ sanitize_ui_brand 同规则；发卡侧**拒绝**而非回落（fail-early）。
    _check_id("ui_brand", brand, 32)
    first = brand[0]
    if not first.isascii() or not first.isalnum():
        raise SystemExit("ui_brand 非法：首字符必须是字母或数字")
    return brand


def normalize_features(features: list[str]) -> list[str]:
    """闭集过滤 + 闭集序（与 C++ parse_license_card 归一化一致）。"""
    got = set(features or [])
    unknown = got - set(KNOWN_FEATURES)
    if unknown:
        raise SystemExit(f"features 含闭集外名字 {sorted(unknown)}；闭集 = {KNOWN_FEATURES}")
    return [f for f in KNOWN_FEATURES if f in got]


# ==========================================================================
# M2.05：卡号「可读短码 + 校验位」（商户侧镜像实现）
# --------------------------------------------------------------------------
# ★ 单一实现纪律：本段是 Python 侧唯一实现，必须与 C++ 侧
#   core/src/auth/LicenseShortCode.cpp **跨语言逐字节一致**。
#   算法（勿单方改；改则两侧同改并更新两处向量测试）：
#     ① FNV-1a-32：h = 0x811C9DC5；逐字节 h ^= b; h = (h * 0x01000193) & 0xFFFFFFFF。
#     ② 7 个 Crockford 字符：32 位按 5 位一组、从高位到低位（步长 5，起点 30）。
#     ③ Luhn mod 32 校验字符（alphabet 下标参与）：
#          从右往左 factor 交替 2,1,…；addend = factor*idx；
#          addend = addend//32 + addend%32；check = (32 - total%32) % 32。
#   格式：TTB-XXXX-XXXX（13 字符；7 位数据 + 1 位校验）。
#   ★ 非安全边界：算法非密码学，只防抄错，不防伪造；准入判据永远是 Ed25519 签名。
# ==========================================================================
SHORT_CODE_ALPHABET = "0123456789ABCDEFGHJKMNPQRSTVWXYZ"   # Crockford（排除 I L O U）
SHORT_CODE_PREFIX = "TTB"
SHORT_CODE_BODY_LEN = 8
SHORT_CODE_RADIX = 32
_SHORT_CODE_DATA_LEN = 7


def _fnv1a32(data: bytes) -> int:
    h = 0x811C9DC5
    for b in data:
        h ^= b
        h = (h * 0x01000193) & 0xFFFFFFFF
    return h


def _crockford32_7(h: int) -> str:
    out = []
    for i in range(_SHORT_CODE_DATA_LEN):
        shift = 30 - i * 5
        out.append(SHORT_CODE_ALPHABET[(h >> shift) & 0x1F])
    return "".join(out)


def _alphabet_index(c: str) -> int:
    return SHORT_CODE_ALPHABET.find(c)   # 不在集合中 → -1（'U' 亦为 -1）


def _normalize_char(c: str) -> str:
    c = c.upper()
    if c == "O":
        return "0"
    if c in ("I", "L"):
        return "1"
    return c


def _luhn_mod32_check(body: str):
    """body = 7 位数据；返回校验字符（str），body 含闭集外字符 ⇒ None。"""
    factor = 2
    total = 0
    for ch in reversed(body):
        idx = _alphabet_index(ch)
        if idx < 0:
            return None
        addend = factor * idx
        factor = 1 if factor == 2 else 2
        addend = (addend // SHORT_CODE_RADIX) + (addend % SHORT_CODE_RADIX)
        total += addend
    return SHORT_CODE_ALPHABET[(SHORT_CODE_RADIX - (total % SHORT_CODE_RADIX)) % SHORT_CODE_RADIX]


def is_valid_license_id(license_id: str) -> bool:
    """[A-Za-z0-9_-]{1,64}（与 LicenseCard 契约 + C++ license_short_code_is_valid_id 一致）。"""
    if not license_id or len(license_id) > 64:
        return False
    for c in license_id:
        if not (c.isascii() and (c.isalnum() or c in "_-")):
            return False
    return True


def short_code(license_id: str) -> str:
    """license_id → "TTB-XXXX-XXXX"（非法 license_id ⇒ 空串，fail-closed，绝不臆造）。"""
    if not is_valid_license_id(license_id):
        return ""
    body = _crockford32_7(_fnv1a32(license_id.encode("utf-8")))
    check = _luhn_mod32_check(body)
    if check is None:                       # 理论上不可达（body 恒为 alphabet 子集）
        return ""
    return f"{SHORT_CODE_PREFIX}-{body[:4]}-{body[4:]}{check}"


def verify_short_code(code: str) -> bool:
    """校验短码的 Luhn 校验位（去分隔/前缀 + Crockford 归一）。**只查校验位，不查签名。**"""
    norm = "".join(_normalize_char(c)
                   for c in (code or "")
                   if c not in "- \t\r\n")
    if len(norm) >= 3 and norm[:3] == SHORT_CODE_PREFIX:
        norm = norm[3:]
    if len(norm) != SHORT_CODE_BODY_LEN:
        return False
    if any(_alphabet_index(c) < 0 for c in norm):
        return False
    check = _luhn_mod32_check(norm[:_SHORT_CODE_DATA_LEN])
    return check is not None and check == norm[_SHORT_CODE_DATA_LEN]


def load_priv(path: Path):
    priv = serialization.load_pem_private_key(path.read_bytes(), password=None)
    if not isinstance(priv, Ed25519PrivateKey):
        raise SystemExit(f"License 私钥必须是 Ed25519；实得 {type(priv).__name__}")
    return priv


def issue_card(priv_pem: Path, device: str, plan: str, is_pro: bool,
               features: list[str], ui_brand: str, license_id: str | None,
               issued_at: int | None, expires_at: int, key_id: str) -> dict:
    """构造 + 签名一张离线卡，返回信封 dict。"""
    _check_id("device", device)
    if plan not in KNOWN_PLANS:
        raise SystemExit(f"plan 非法：{plan!r}；合法 = {KNOWN_PLANS}")
    lic = {
        "license_id": license_id or (
            "ttbox-lic-" + time.strftime("%Y%m%d") + "-" + secrets.token_hex(3)),
        "device": device,
        "plan": plan,
        "is_pro": bool(is_pro),
        "features": normalize_features(features),
        "ui_brand": _check_brand(ui_brand),
        "issued_at": int(issued_at if issued_at is not None else time.time()),
        "expires_at": int(expires_at),
    }
    if lic["expires_at"] not in (0,) and lic["expires_at"] <= lic["issued_at"]:
        raise SystemExit("expires_at 必须大于 issued_at（或 0 = 永久）")
    _check_id("license_id", lic["license_id"])

    priv = load_priv(priv_pem)
    sig = base64.b64encode(priv.sign(canonical_license(lic).encode("utf-8"))).decode("ascii")
    return {"license": lic, "key_id": key_id, "signature": sig}


def verify_card(card: dict, pub_hex: str) -> tuple[bool, str]:
    """商户侧自检（core 侧另有独立实现：OfflineCardClient）。"""
    try:
        lic = card["license"]
        pk = Ed25519PublicKey.from_public_bytes(bytes.fromhex(pub_hex.strip()))
        sig = base64.b64decode(card["signature"])
        ok = pk.verify(sig, canonical_license(lic).encode("utf-8"))
        return (ok is None), "signature ok"
    except Exception as e:  # noqa: BLE001 —— 自检工具：任何异常都是"验不过"
        return False, f"verify failed: {e}"


def gen_keypair(out_dir: Path, key_id: str) -> tuple[Path, Path]:
    out_dir.mkdir(parents=True, exist_ok=True)
    KEYS_DIR.mkdir(parents=True, exist_ok=True)
    priv = Ed25519PrivateKey.generate()
    priv_path = out_dir / f"{key_id}.priv.pem"
    priv_path.write_bytes(priv.private_bytes(
        encoding=serialization.Encoding.PEM,
        format=serialization.PrivateFormat.PKCS8,
        encryption_algorithm=serialization.NoEncryption()))
    priv_path.chmod(0o600)
    pub_hex = priv.public_key().public_bytes(
        encoding=serialization.Encoding.Raw,
        format=serialization.PublicFormat.Raw).hex()
    pub_path = KEYS_DIR / f"{key_id}.pub.hex"
    pub_path.write_text(pub_hex + "\n")
    return priv_path, pub_path


def emit_c_header(key_id: str, pub_hex: str) -> str:
    """重生成 core/src/auth/LicenseKeys.hpp 的关键两行（换钥用）。
    手工替换文件中 kLicenseKeyId / kLicensePublicKeyHex 两常量即可。"""
    return (
        f'inline const char* kLicenseKeyId = "{key_id}";\n'
        f'inline const char* kLicensePublicKeyHex =\n'
        f'    "{pub_hex.strip()}";\n')


def local_fingerprint() -> str:
    """读本机 cpu_serial（在板上运行时即本板绑定串；商户机无则提示用 --device）。"""
    try:
        for line in Path("/proc/cpuinfo").read_text().splitlines():
            if ":" in line and line.split(":", 1)[0].strip() == "Serial":
                return line.split(":", 1)[1].strip()
    except OSError:
        pass
    raise SystemExit("本机读不到 /proc/cpuinfo Serial：请在设备 Web 管理页复制 cpu Serial，"
                    "或用 --device 显式传入")


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description="TTBOX 离线授权卡发卡工具（M2 商户侧）")
    sub = ap.add_subparsers(dest="cmd", required=True)

    g = sub.add_parser("gen-key", help="生成密钥对（私钥落 .testkeys/ 不入库）")
    g.add_argument("--key-id", default=DEFAULT_KEY_ID)
    g.add_argument("--out-dir", default=str(TESTKEYS_DIR))

    f = sub.add_parser("fingerprint", help="读本机设备指纹（板上运行 = 本板绑定串）")

    i = sub.add_parser("issue-card", help="发一张离线卡（输出 JSON 信封到 stdout）")
    i.add_argument("--device", default=None, help="绑定 cpu_serial（缺省读本机）")
    i.add_argument("--plan", default="subscription", choices=KNOWN_PLANS)
    i.add_argument("--pro", dest="is_pro", action="store_true", default=False)
    i.add_argument("--features", default="capture,inference,aim,ota",
                   help="逗号分隔，闭集子集")
    i.add_argument("--brand", default="ttbox")
    i.add_argument("--license-id", default=None)
    i.add_argument("--days", type=int, default=0, help="有效天数；0 = 永久")
    i.add_argument("--issued-at", type=int, default=None)
    i.add_argument("--key-id", default=DEFAULT_KEY_ID)
    i.add_argument("--priv", default=str(TESTKEYS_DIR / f"{DEFAULT_KEY_ID}.priv.pem"))

    v = sub.add_parser("verify", help="自检一张卡（对公钥验签）")
    v.add_argument("card_json")
    v.add_argument("--key-id", default=DEFAULT_KEY_ID)

    e = sub.add_parser("emit-c-header", help="输出换钥后的 LicenseKeys.hpp 关键两行")
    e.add_argument("--key-id", default=DEFAULT_KEY_ID)

    # ---- M2.05：短码（与 core/src/auth/LicenseShortCode.cpp 跨语言一致）----
    sc = sub.add_parser("short-code", help="license_id → 可读短码 TTB-XXXX-XXXX（stdout）")
    sc.add_argument("license_id")

    vs = sub.add_parser("verify-shortcode", help="校验短码校验位（PASS→0 / FAIL→1）")
    vs.add_argument("code", metavar="short_code")

    a = ap.parse_args(argv)
    if a.cmd == "gen-key":
        p, pub = gen_keypair(Path(a.out_dir), a.key_id)
        print(f"私钥（不入库）: {p}")
        print(f"公钥（入库）  : {pub}")
        print("★ 换钥须重生成 core/src/auth/LicenseKeys.hpp（emit-c-header）并重编译 core。")
        return 0
    if a.cmd == "fingerprint":
        print(local_fingerprint())
        return 0
    if a.cmd == "issue-card":
        device = a.device or local_fingerprint()
        issued_at = a.issued_at if a.issued_at is not None else int(time.time())
        expires_at = 0 if a.days in (0, None) else issued_at + a.days * 86400
        card = issue_card(
            priv_pem=Path(a.priv), device=device, plan=a.plan, is_pro=a.is_pro,
            features=[x for x in a.features.split(",") if x], ui_brand=a.brand,
            license_id=a.license_id, issued_at=issued_at,
            expires_at=expires_at, key_id=a.key_id)
        print(json.dumps(card, sort_keys=True, separators=(",", ":"),
                         ensure_ascii=False))
        # ★ M2.05：短码仅供商户出卡台账**人对账**用 —— 写 stderr，**不污染** stdout 的信封
        #   （stdout 直接进 license.key / 传板，多一行就会破坏卡 JSON 解析）。
        sc = short_code(card["license"]["license_id"])
        if sc:
            sys.stderr.write(f"short_code={sc}\n")
        return 0
    if a.cmd == "verify":
        card = json.loads(Path(a.card_json).read_text(encoding="utf-8"))
        pub_hex = (KEYS_DIR / f"{a.key_id}.pub.hex").read_text()
        ok, why = verify_card(card, pub_hex)
        print(("PASS: " if ok else "FAIL: ") + why)
        return 0 if ok else 1
    if a.cmd == "short-code":
        code = short_code(a.license_id)
        if not code:
            sys.stderr.write(f"license_id 非法（须 [A-Za-z0-9_-]{{1,64}}）：{a.license_id!r}\n")
            return 2
        print(code)
        return 0
    if a.cmd == "verify-shortcode":
        ok = verify_short_code(a.code)
        print(("PASS: " if ok else "FAIL: ") + a.code)
        return 0 if ok else 1
    if a.cmd == "emit-c-header":
        pub_hex = (KEYS_DIR / f"{a.key_id}.pub.hex").read_text()
        print(emit_c_header(a.key_id, pub_hex))
        return 0
    return 2


if __name__ == "__main__":
    sys.exit(main())
