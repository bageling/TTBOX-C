#!/usr/bin/env bash
# ttbox_pack_ota.sh — OTA 出货包【唯一打包入口】（S3 / 2026-09-19 交付方案批 1）
#
# ─────────────────────────────────────────────────────────────────────────────
# 为何存在：
#   此前 OTA 包是手工 `tar` 整棵仓库树，与装机 sync_tree 的白名单闭集互不约束
#   ⇒ 1.4.7 出货树 368 个文件混入 docs(123)/tools(8)/platform(25)/modules(10)
#   （含发卡工具 ttbox_license_gen.py、fake_ota_server.py —— 定案 H-26 明写不入包）。
#   本脚本与 sync_tree 读【同一份白名单】deploy/pack_manifest.txt，改清单一处两处生效；
#   并加打包前置断言（fail-closed，任一不过不出包）。
#
# 包布局（1.4.7 联调钉死，见 docs/交付前运行验证报告-2026-09-19.md:100-101）：
#   包根 RELEASE_MANIFEST.json + 全部发布树文件在 payload/ 下
#   tar 成员【不带 ./ 前缀】（带前缀 updater 判 manifest_missing；
#   成员无 payload/ 前缀 updater 展开必挂）
#
# 版本号真源（S7）：core/CMakeLists.txt 的 project(... VERSION x.y.z)。
#   发布目录名 / manifest.version / 包名 全部从这一处派生。
#
# 用法:
#   ttbox_pack_ota.sh <build_dir> <out_dir> [选项]
#     build_dir            交叉编译产物目录（含 ttbox_core_main 与 CMakeCache.txt）
#     out_dir              产物输出目录（自动创建）
#   选项:
#     --priv <pem>         签名私钥（出货必填；缺省不签名 = 仅限本地装配自测）
#     --password-file <f>  私钥口令文件
#     --key-id <id>        签名 key_id（缺省 ttbox-ota-2026b）
#     --dry-run            装配 + 断言 + 清单，不 tar 不签名，装配目录留档不删
#     --git-sha <sha>      显式注入 manifest.git_sha（缺省 git rev-parse HEAD）
#
# 退出码：0 = 出包成功（或 dry-run 断言全过）；1 = 断言失败/参数错（fail-closed 不出包）。
# ─────────────────────────────────────────────────────────────────────────────
set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
MANIFEST="${REPO_ROOT}/deploy/pack_manifest.txt"
SIGN_TOOL="${REPO_ROOT}/tools/ota/ttbox_ota_sign.py"

die()  { printf '[pack][FAIL] %s\n' "$*" >&2; exit 1; }
info() { printf '[pack] %s\n' "$*"; }
ok()   { printf '[pack][ OK ] %s\n' "$*"; }

# ---- 参数 ----
BUILD_DIR="" OUT_DIR="" PRIV="" PWFILE="" KEY_ID="ttbox-ota-2026b" DRY=0 GIT_SHA=""
while [ $# -gt 0 ]; do
    case "$1" in
        --priv)          PRIV="${2:?}"; shift 2 ;;
        --password-file) PWFILE="${2:?}"; shift 2 ;;
        --key-id)        KEY_ID="${2:?}"; shift 2 ;;
        --dry-run)       DRY=1; shift ;;
        --git-sha)       GIT_SHA="${2:?}"; shift 2 ;;
        -*)              die "未知选项: $1" ;;
        *)  if [ -z "$BUILD_DIR" ]; then BUILD_DIR="$1"
            elif [ -z "$OUT_DIR" ];  then OUT_DIR="$1"
            else die "多余参数: $1"; fi; shift ;;
    esac
done
[ -n "$BUILD_DIR" ] || die "缺 build_dir（交叉编译产物目录，含 ttbox_core_main）"
[ -n "$OUT_DIR" ]   || die "缺 out_dir"
[ -f "$MANIFEST" ]  || die "缺打包白名单: $MANIFEST"

# ---- 版本号真源（S7）：core/include/ttbox/core/version.hpp::kCoreVersion ----
#   ★ 2026-09-29 起产品版本带字母前缀（"V1.0.01"），而 CMake 的 project VERSION 只解析数字
#     ⇒ 它降级为「数字镜像」；本脚本改读 version.hpp（唯一真源）。
#     包名 / manifest.version / releases 目录名 / 云端版本串全部派生自这里。
VERSION_HPP="${REPO_ROOT}/core/include/ttbox/core/version.hpp"
[ -f "$VERSION_HPP" ] || die "缺 ${VERSION_HPP}"
VER="$(sed -n 's/.*kCoreVersion[[:space:]]*=[[:space:]]*"\([^"]*\)".*/\1/p' "$VERSION_HPP" | head -1)"
[ -n "$VER" ] || die "无法从 ${VERSION_HPP} 解析版本号（kCoreVersion = \"x.y.z\"）"
case "$VER" in
    *[!A-Za-z0-9._-]*) die "版本号含非法字符: ${VER}（云端 bridge 与板端更新器要求 [A-Za-z0-9._-]+）" ;;
esac
info "版本（真源 core/include/ttbox/core/version.hpp）: ${VER}"

# ---- git_sha（与 fhs_init gen_manifest 同一套三级回退）----
if [ -z "$GIT_SHA" ]; then
    GIT_SHA="$(git -C "${REPO_ROOT}" rev-parse HEAD 2>/dev/null || echo '')"
fi
if [ -z "$GIT_SHA" ] && [ -f "${REPO_ROOT}/.git_sha" ]; then
    GIT_SHA="$(tr -d '[:space:]' < "${REPO_ROOT}/.git_sha" 2>/dev/null || echo '')"
fi
[ -n "$GIT_SHA" ] || die "无法解析 git_sha（用 --git-sha 显式注入，或仓库需有 .git/.git_sha）"

# ---- 构建产物校验（bin/ 闭集 + RUNPATH）----
CORE_BIN="${BUILD_DIR}/ttbox_core_main"
[ -f "$CORE_BIN" ] || die "缺交叉编译产物: ${CORE_BIN}"
file "$CORE_BIN" | grep -q 'ELF' || die "${CORE_BIN} 不是 ELF"
readelf -d "$CORE_BIN" 2>/dev/null | grep -q 'RUNPATH' \
    || die "${CORE_BIN} 无 RUNPATH 段（出货判据：逐段 \$ORIGIN，见 ttbox_release_verify.sh）"
readelf -d "$CORE_BIN" 2>/dev/null | grep 'RUNPATH' | grep -q '\$ORIGIN' \
    || die "${CORE_BIN} RUNPATH 不含 \$ORIGIN"

# librknnrt.so：链接期同源铁律（与 fhs_init resolve_rknnrt_so 同一判据；禁拷仓库 lib/）
RKNNRT_SO="${TTBOX_RKNNRT_SO:-}"
if [ -z "$RKNNRT_SO" ]; then
    RKNNRT_SO="$(sed -n 's/^RKNNRT_LIBRARY:FILEPATH=\(.*\)$/\1/p; s/^RKNNRT_LIBRARY:INTERNAL=\(.*\)$/\1/p' \
        "${BUILD_DIR}/CMakeCache.txt" 2>/dev/null | head -1)"
fi
[ -n "$RKNNRT_SO" ] && [ -f "$RKNNRT_SO" ] \
    || die "无法解析链接期 librknnrt.so（设 TTBOX_RKNNRT_SO 或提供 ${BUILD_DIR}/CMakeCache.txt 的 RKNNRT_LIBRARY）；拒绝出货仓库 lib/ 副本（1.5.2 过期残留）"
info "librknnrt 出货源（链接期同源）: ${RKNNRT_SO}"

# ---- 装配 ----
WS="$(mktemp -d "${TMPDIR:-/tmp}/ttbox-pack-XXXXXX")"
PAYLOAD="${WS}/payload"
trap '[ "$DRY" = 1 ] || rm -rf -- "$WS"' EXIT
mkdir -p "${PAYLOAD}/bin" "${PAYLOAD}/lib"

install -m 0755 "$CORE_BIN" "${PAYLOAD}/bin/ttbox_core_main"
install -m 0644 "$RKNNRT_SO" "${PAYLOAD}/lib/librknnrt.so"

# 白名单循环（与 fhs_init sync_tree 同一语义，改清单两处一起变）
#   目录条目：tar 整树（剔除 __pycache__/*.pyc/.git/tests，与 tcopy 同一剔除集）
#   文件/glob 条目：逐个 install 到【展开后的真实相对路径】（不能用 glob 原串当目标）
#   ★ $entry 必须裸奔（不加引号）才能做 glob 展开；REPO_ROOT 引号保留防分词。
#     条目自身含空白 = 清单写坏，fail-closed。
entry="" src="" rel="" mode=""
while IFS= read -r entry <&3; do
    case "$entry" in ''|'#'*) continue ;; esac
    case "$entry" in *[[:space:]]*) die "白名单条目含空白: ${entry}" ;; esac
    matched=0
    for src in "${REPO_ROOT}"/$entry; do
        [ -e "$src" ] || break   # glob 未匹配 → 原样字面量 → 视为条目失效
        matched=1
        rel="${src#${REPO_ROOT}/}"
        if [ -d "$src" ]; then
            mkdir -p -- "${PAYLOAD}/$(dirname "$rel")"
            tar -C "${REPO_ROOT}" --exclude='__pycache__' --exclude='*.pyc' \
                --exclude=.git --exclude=tests -cf - "$rel" | tar -C "$PAYLOAD" -xf -
        else
            mode=0644
            case "$src" in *.sh|*.py) mode=0755 ;; esac
            install -D -m "$mode" "$src" "${PAYLOAD}/${rel}"
        fi
    done
    [ "$matched" = 1 ] || die "白名单条目不存在（或 glob 展开为空）: ${entry}"
done 3< "$MANIFEST"

# 定向修剪（与 sync_tree 同步：只做减法/权限修正，不新增文件）
rm -f "${PAYLOAD}/plugins/web/api_v1.py" \
      "${PAYLOAD}/plugins/web/framework_api.py"
rm -rf "${PAYLOAD}/plugins/web/static/legacy"
chmod 0755 -- "${PAYLOAD}/usbproxy/usb-proxy"

# ---- 权限归一化（与 ota_updater.normalize_staging_perms 同一口径）----
#   drvfs（/mnt/g）源侧无真实权限位，tar 拷贝会把文件带成 777。
#   出包前统一浇筑：目录 0755；*.sh / scripts 段 / bin 段（含 plugins/*/bin）/usb-proxy
#   0755；其余 0644。包自洽，不依赖装包端归一化兜底。
find "$PAYLOAD" -type d -exec chmod 0755 {} +
find "$PAYLOAD" -type f -exec chmod 0644 {} +
find "$PAYLOAD" -type f \( -name '*.sh' \
    -o -path '*/scripts/*' -o -path '*/bin/*' \) -exec chmod 0755 {} +
chmod 0755 -- "${PAYLOAD}/usbproxy/usb-proxy"

# ---- 断言 0：无 world-writable（权限归一化的反向验证）----
WW="$(find "$PAYLOAD" \( -type f -o -type d \) -perm -o+w -print)"
[ -z "$WW" ] || die "payload 存在 world-writable（归一化失效）:
${WW}"
ok "断言0 无 world-writable"

# ---- 断言 1：顶层闭集（docs/tools/platform/modules/tests/config 等禁入）----
EXPECT_TOP="bin deploy framework lib plugins scripts ttbox_motion usbproxy"
ACTUAL_TOP="$(cd "$PAYLOAD" && ls -A | sort | tr '\n' ' ' | sed 's/ $//')"
EXPECT_TOP_E="$(printf '%s\n' $EXPECT_TOP | sort | tr '\n' ' ' | sed 's/ $//')"
[ "$ACTUAL_TOP" = "$EXPECT_TOP_E" ] \
    || die "payload 顶层与白名单闭集不符：实际[${ACTUAL_TOP}] 期望[${EXPECT_TOP_E}]"
ok "断言1 顶层闭集: ${ACTUAL_TOP}"

# ---- 断言 2：禁入物逐条扫（密钥/垃圾/构建残渣）----
#   deploy/keys 是唯一放行区（白名单公钥，.pub/.pem 只许出现在这里）
BAD="$(cd "$PAYLOAD" && {
    # 坏目录（出现即报，不 prune 不下钻）
    find . -type d \( -name '__pycache__' -o -name '.git' -o -name '.testkeys' \
        -o -name 'CMakeFiles' -o -name 'tests' \) -print
    # 坏文件（deploy/keys 整目录放行，其余命中即报）
    #   ★ 不能用 *_backup*：正当脚本 ttbox_backup.sh 会被误杀；只认备份残渣后缀
    find . -path './deploy/keys' -prune -o -type f \( \
        -name '*.pem' -o -name '*.priv.pem' -o -name '*.pub' -o -name 'id_rsa*' \
        -o -name '*.pyc' -o -name '*.bak' -o -name '*.bak-*' -o -name '*.backup' \
        -o -name '*.orig' -o -name '*.rej' \
        -o -name '*.o' -o -name '*.a' -o -name '*.cmake' \) -print
})"
[ -z "$BAD" ] || die "payload 混入禁用物:
${BAD}"
ok "断言2 无密钥/无垃圾/无构建残渣"

# ---- 断言 3：bin/ 闭集 = 恰 1 文件 ttbox_core_main ----
BIN_N="$(find "${PAYLOAD}/bin" -type f | wc -l)"
[ "$BIN_N" = 1 ] && [ -f "${PAYLOAD}/bin/ttbox_core_main" ] \
    || die "bin/ 应恰 1 个文件 ttbox_core_main，实得 ${BIN_N} 个"
ok "断言3 bin/ 闭集: 恰 1 文件 ttbox_core_main"

# ---- 断言 4：双向核对（混入必现、漏装必现）----
# 反向：payload 里除 bin/lib 外的每个文件，仓库里必须存在（否则 = 混入）
LEAK="$(cd "$PAYLOAD" && find . -type f | sed 's|^\./||' \
        | grep -v -e '^bin/' -e '^lib/' \
        | while IFS= read -r f; do [ -e "${REPO_ROOT}/${f}" ] || echo "$f"; done)"
[ -z "$LEAK" ] || die "payload 存在仓库外文件（混入）:
${LEAK}"
# 正向：白名单展开的每个文件，payload 里必须存在（api_v1/framework_api/legacy 是定向修剪，
#   2026-09-24 起 legacy 源头已删、此条 continue 仅作兜底；
#   find 剔除集必须与装配 tar 的 --exclude 完全一致，否则 tests 会假报漏装）
MISS=""
while IFS= read -r entry <&3; do
    case "$entry" in ''|'#'*) continue ;; esac
    for src in "${REPO_ROOT}"/$entry; do   # $entry 裸奔才能 glob 展开（与装配循环同语义）
        if [ -d "$src" ]; then
            while IFS= read -r f; do
                rel="${f#${REPO_ROOT}/}"
                case "$rel" in
                    plugins/web/api_v1.py|plugins/web/framework_api.py) continue ;;
                    plugins/web/static/legacy/*) continue ;;
                esac
                [ -e "${PAYLOAD}/${rel}" ] || MISS="${MISS}${rel}
"
            done < <(find "$src" -type f ! -name '*.pyc' ! -path '*__pycache__*' \
                        ! -path '*/tests/*' ! -path '*/.git/*')
        else
            rel="${src#${REPO_ROOT}/}"
            [ -e "${PAYLOAD}/${rel}" ] || MISS="${MISS}${rel}
"
        fi
    done
done 3< "$MANIFEST"
[ -z "$MISS" ] || die "白名单文件未进 payload（漏装）:
${MISS}"
ok "断言4 双向核对: 无混入、无漏装"

# ---- 断言 5：版本号一致性（S7）----
[ -n "$VER" ] || die "版本号为空"
ok "断言5 版本号: ${VER}（真源 core/CMakeLists.txt）"

# ---- RELEASE_MANIFEST.json（schema 与 fhs_init gen_manifest 一致）----
info "生成 RELEASE_MANIFEST.json ..."
python3 - "$PAYLOAD" "$VER" "$GIT_SHA" <<'PY'
import datetime, hashlib, json, os, sys
root, ver, gitsha = sys.argv[1], sys.argv[2], sys.argv[3]
files = {}
for dp, dn, fn in os.walk(root):
    for f in fn:
        full = os.path.join(dp, f)
        rel = os.path.relpath(full, root)
        with open(full, "rb") as fh:
            files[rel] = hashlib.sha256(fh.read()).hexdigest()
doc = {
    "version": ver,
    "built_at": datetime.datetime.now(datetime.timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ"),
    "git_sha": gitsha,
    "files_sha256": files,
}
with open(os.path.join(os.path.dirname(root), "RELEASE_MANIFEST.json"), "w", encoding="utf-8") as fh:
    json.dump(doc, fh, ensure_ascii=False, indent=2, sort_keys=True)
    fh.write("\n")
print(f"[pack][ OK ] manifest: {len(files)} 个文件的 sha256")
PY

N_FILES="$(find "$PAYLOAD" -type f | wc -l)"
info "payload 就绪: ${N_FILES} 个文件（含 bin/lib）"

# ---- 出包 ----
if [ "$DRY" = 1 ]; then
    ok "dry-run：断言全过，装配目录留档 ${WS}（不 tar、不签名）"
    trap - EXIT
    exit 0
fi

mkdir -p -- "$OUT_DIR"
TGZ="${OUT_DIR}/ttbox-${VER}.ota.tgz"
tar -C "$WS" -czf "$TGZ" RELEASE_MANIFEST.json payload

# 断言 6：tar 成员布局（无 ./ 前缀；根 manifest + payload/ 前缀）
TAR_BAD="$(tar -tzf "$TGZ" | grep -v -e '^RELEASE_MANIFEST.json$' -e '^payload/' || true)"
[ -z "$TAR_BAD" ] || die "tar 成员布局不符（存在非 [RELEASE_MANIFEST.json | payload/*] 成员）:
${TAR_BAD}"
tar -tzf "$TGZ" | sed -n '1p' | grep -q '^RELEASE_MANIFEST.json$' \
    || die "首个 tar 成员应为 RELEASE_MANIFEST.json"
ok "断言6 tar 布局: 根 manifest + payload/，无 ./ 前缀"

info "包: ${TGZ} ($(du -h "$TGZ" | cut -f1)) sha256=$(sha256sum "$TGZ" | cut -d' ' -f1)"

# ---- 签名（出货必签；缺 --priv 视为本地装配自测，警告但放行）----
if [ -n "$PRIV" ]; then
    [ -f "$PRIV" ] || die "签名私钥不存在: $PRIV"
    [ -f "$SIGN_TOOL" ] || die "缺签名工具: $SIGN_TOOL"
    SIGN_ARGS=(sign "$TGZ" "$VER" --key-id "$KEY_ID" --priv "$PRIV")
    [ -n "$PWFILE" ] && SIGN_ARGS+=(--password-file "$PWFILE")
    python3 "$SIGN_TOOL" "${SIGN_ARGS[@]}"
    ok "已签名（key_id=${KEY_ID}）"
else
    printf '[pack][WARN] 未提供 --priv：本包【未签名】，仅限本地装配自测，不得分发！\n' >&2
fi

rm -rf -- "$WS"
ok "完成: ttbox-${VER}.ota.tgz"
