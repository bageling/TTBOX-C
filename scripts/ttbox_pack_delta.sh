#!/usr/bin/env bash
# ttbox_pack_delta.sh — 增量 OTA 打包（2026-09-19）
#
# 用法（WSL 里跑，Windows 侧缺 tar/python3 生态不保证）：
#   bash scripts/ttbox_pack_delta.sh <新全量.tgz> <基线全量.tgz> <输出目录>
#
# 产物：ttbox-<新ver>-delta-from-<基线ver>.ota.tgz（不签名——上传到分发服务器
# 由 publish API 统一签名并生成旁车 sign.json，与全量包同一条验签链）。
#
# 格式：根 RELEASE_MANIFEST.json（完整新清单 files_sha256 + delta:true +
# base_version）+ payload/（只含与基线不同的文件）。板端更新器收到后
# 按 manifest 从「payload + 当前 release 树」合并出完整 staging，再走原浇筑流程。
#
# fail-closed 断言（任一条不过不出包）：
#   d0 两包 manifest 可解析且 version 不同
#   d1 无删除文件（基线有而新版没有 ⇒ 拒绝，v1 不支持删除）
#   d2 有差异文件（否则 delta 无意义）
#   d3 产物逐文件 sha256 == 新 manifest（重组后全量复验）
set -euo pipefail

NEW="${1:?用法: ttbox_pack_delta.sh <新全量.tgz> <基线全量.tgz> <输出目录>}"
BASE="${2:?缺基线全量包}"
OUT="${3:?缺输出目录}"

[[ -f "$NEW" && -f "$BASE" ]] || { echo "[delta][FAIL] 输入包不存在"; exit 1; }

python3 - "$NEW" "$BASE" "$OUT" <<'PY'
import hashlib, io, json, os, sys, tarfile, time

new_path, base_path, out_dir = sys.argv[1], sys.argv[2], sys.argv[3]

def manifest_of(p):
    with tarfile.open(p, "r:*") as t:
        f = t.extractfile("RELEASE_MANIFEST.json")
        if f is None:
            raise SystemExit(f"[delta][FAIL] {p} 缺 RELEASE_MANIFEST.json")
        return json.loads(f.read().decode("utf-8"))

nm, bm = manifest_of(new_path), manifest_of(base_path)
nver, bver = str(nm.get("version") or ""), str(bm.get("version") or "")
nf, bf = nm.get("files_sha256") or {}, bm.get("files_sha256") or {}
if not nver or nver == bver or not nf or not bf:
    raise SystemExit(f"[delta][FAIL] d0 断言：version 异常（new={nver!r} base={bver!r}）或清单为空")

deleted = sorted(set(bf) - set(nf))
if deleted:
    raise SystemExit(f"[delta][FAIL] d1 断言：检测到删除文件，v1 不支持: {deleted}")

changed = sorted(r for r in nf if bf.get(r) != nf[r])
if not changed:
    raise SystemExit("[delta][FAIL] d2 断言：无差异文件，delta 无意义（直接发全量）")

out_name = f"ttbox-{nver}-delta-from-{bver}.ota.tgz"
os.makedirs(out_dir, exist_ok=True)
out_path = os.path.join(out_dir, out_name)

md = dict(nm)
md["delta"] = True
md["base_version"] = bver
md["delta_files"] = changed
mdata = json.dumps(md, ensure_ascii=False, sort_keys=True, indent=2).encode("utf-8")

changed_set = set(changed)
with tarfile.open(new_path, "r:*") as src, tarfile.open(out_path, "w:gz") as out:
    ti = tarfile.TarInfo("RELEASE_MANIFEST.json")
    ti.size, ti.mtime, ti.mode = len(mdata), int(time.time()), 0o644
    out.addfile(ti, io.BytesIO(mdata))
    for m in src.getmembers():
        if not m.isfile():
            continue
        rel = m.name[len("payload/"):] if m.name.startswith("payload/") else None
        if rel and rel in changed_set:
            out.addfile(m, src.extractfile(m))  # 必须显式给 fileobj，否则只写头不写数据

# d3：产物逐文件复验
with tarfile.open(out_path, "r:*") as t:
    for rel in changed:
        f = t.extractfile("payload/" + rel)
        if f is None or hashlib.sha256(f.read()).hexdigest() != nf[rel]:
            raise SystemExit(f"[delta][FAIL] d3 断言：产物 sha256 不符: {rel}")

size = os.path.getsize(out_path)
with tarfile.open(new_path, "r:*") as t:
    pass
full_size = os.path.getsize(new_path)
print(f"[delta][ OK ] d0-d3 全过：{len(changed)}/{len(nf)} 个差异文件")
print(f"[delta] 包: {out_path} ({size} B，全量 {full_size} B，省 {100 - size * 100 // full_size}%)")
PY
