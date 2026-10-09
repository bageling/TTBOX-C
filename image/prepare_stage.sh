#!/usr/bin/env bash
# 组装 staging：把要推进镜像的所有物料集中到 WSL 本地 ext4 工作区
#
#   <STAGE>/steps/            镜像内执行步骤脚本
#   <STAGE>/factory/          出厂覆盖（优先于仓库 deploy/config 种子）
#   <STAGE>/deploy/config/    出厂配置基线
#   <STAGE>/deploy/systemd/   单元文件（权威源 = 仓库 deploy/systemd/）
#   <STAGE>/scripts/          装机/自愈真脚本（release_install / ensure_services）
#   <STAGE>/payload/          release 运行树（已摊平：RELEASE_MANIFEST.json + bin/lib/...）
#
# ★ 为什么 staging 必须落在 WSL 本地 ext4 而非 /mnt/g：
#   仓库在 drvfs 上，**源侧无真实权限位**（ttbox_pack_ota.sh:140 同一坑）——
#   tar 从那种源侧拷贝会把文件带成 777，于是核心二进制会以 world-writable 落进镜像。
#   放到 ext4 后 tar 解出的 mode 与 OTA 包内声明一致（755/644）。
#   此外仍按 pack_ota 的归一化口径复核一遍，并以「无 world-writable」断言收口。
set -eu
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO="$(cd "$HERE/.." && pwd)"
VER="${TTBOX_VER:-1.5.21}"
TGZ="${REPO}/dist/ttbox-${VER}.ota.tgz"
STAGE="${TTBOX_STAGE_DIR:-/root/ttbox-image/_stage}"

[ -f "$TGZ" ] || { echo "ERROR: 找不到发布包 $TGZ" >&2; exit 1; }

rm -rf "$STAGE"
mkdir -p "$STAGE"

echo "== 1. steps（仓库 -> ext4，显式设权）=="
cp -a "$HERE/steps" "$STAGE/steps"
find "$STAGE/steps" -type f -name '*.sh' -exec chmod 0755 {} +
find "$STAGE/steps" -type d -exec chmod 0755 {} +
ls "$STAGE/steps" | sed 's/^/   /'

echo
echo "== 2. factory 出厂覆盖（业主指定，优先于仓库种子）=="
mkdir -p "$STAGE/factory"
if compgen -G "$HERE/factory/*" >/dev/null 2>&1; then
    cp -a "$HERE/factory/." "$STAGE/factory/"
    # ★ 只能对文件 chmod 0644：factory/ 下现在有 model_EP/ 这样的目录，
    #   对目录 chmod 0644 会去掉执行位 ⇒ chroot 内连 cd 都进不去，模型直接消失。
    find "$STAGE/factory" -type d -exec chmod 0755 {} + 2>/dev/null || true
    find "$STAGE/factory" -type f -exec chmod 0644 {} + 2>/dev/null || true
    ls "$STAGE/factory" | sed 's/^/   /'
else
    echo "   (无覆盖)"
fi

echo
echo "== 3. deploy/config + deploy/systemd（仓库 -> ext4）=="
mkdir -p "$STAGE/deploy/config" "$STAGE/deploy/systemd"
for f in 00-factory.json 10-device.json hardware_display.json; do
    cp -a "$REPO/deploy/config/$f" "$STAGE/deploy/config/$f"; echo "   config/$f"
done
cp -a "$REPO"/deploy/systemd/*.service "$REPO"/deploy/systemd/*.timer \
      "$REPO"/deploy/systemd/*.path "$STAGE/deploy/systemd/" 2>/dev/null || true
ls "$STAGE/deploy/systemd" | sed 's/^/   systemd\//'
chmod 0644 "$STAGE"/deploy/config/* "$STAGE"/deploy/systemd/*

echo
echo "== 4. scripts（装机链路真源 + 产品自带验证器）=="
mkdir -p "$STAGE/scripts"
for f in ttbox_release_install.sh ttbox_ensure_services.sh ttbox_fhs_init.sh \
         ttbox_release_verify.sh ttbox_doctor.sh; do
    [ -f "$REPO/scripts/$f" ] || { echo "   [!] 仓库缺 scripts/$f"; continue; }
    cp -a "$REPO/scripts/$f" "$STAGE/scripts/$f"; echo "   $f"
done
chmod 0755 "$STAGE"/scripts/*.sh

echo
echo "== 5. payload（解出 OTA 包并摊平）=="
# 口径（与 scripts/ttbox_ota_updater.py::_extract 一致）：
#   包内 = RELEASE_MANIFEST.json（根）+ payload/<树>；manifest 路径键【不带】payload/ 前缀
#   ⇒ 喂给 ttbox_release_install.sh 的 payload_dir 必须是摊平后的
rm -rf "$STAGE/_raw" "$STAGE/payload"
mkdir -p "$STAGE/_raw" "$STAGE/payload"
tar xzf "$TGZ" -C "$STAGE/_raw"
mv "$STAGE/_raw/RELEASE_MANIFEST.json" "$STAGE/payload/RELEASE_MANIFEST.json"
for e in "$STAGE/_raw/payload"/*; do [ -e "$e" ] && mv "$e" "$STAGE/payload/"; done
rm -rf "$STAGE/_raw"
echo "   顶层: $(ls "$STAGE/payload" | tr '\n' ' ')"

echo
echo "== 6. payload 权限复核（口径同 ttbox_pack_ota.sh:143-147）=="
printf '   bin/ttbox_core_main   %s\n' "$(stat -c %a "$STAGE/payload/bin/ttbox_core_main")"
printf '   usbproxy/usb-proxy    %s\n' "$(stat -c %a "$STAGE/payload/usbproxy/usb-proxy")"
printf '   lib/librknnrt.so      %s\n' "$(stat -c %a "$STAGE/payload/lib/librknnrt.so")"
BAD_EXEC=""
for f in bin/ttbox_core_main bin/ttbox_web bin/ttbox_edid \
         usbproxy/usb-proxy usbproxy/board/run-ttbox-usb-proxy.sh; do
    m="$(stat -c %a "$STAGE/payload/$f" 2>/dev/null || echo missing)"
    [ "$m" = "755" ] || BAD_EXEC="$BAD_EXEC\n     $f = $m (期望 755)"
done
if [ -n "$BAD_EXEC" ]; then
    echo "   [!] 存在非 755 的可执行文件，按 pack_ota 口径就地归一：$BAD_EXEC"
    find "$STAGE/payload" -type d -exec chmod 0755 {} +
    find "$STAGE/payload" -type f -exec chmod 0644 {} +
    find "$STAGE/payload" -type f \( -name '*.sh' -o -path '*/scripts/*' -o -path '*/bin/*' \) \
         -exec chmod 0755 {} +
    chmod 0755 -- "$STAGE/payload/usbproxy/usb-proxy"
    for f in bin/ttbox_core_main bin/ttbox_web bin/ttbox_edid \
             usbproxy/usb-proxy usbproxy/board/run-ttbox-usb-proxy.sh; do
        printf '   %-46s -> %s\n' "$f" "$(stat -c %a "$STAGE/payload/$f")"
    done
else
    echo "   [✓] 全部可执行文件已是 755，无需归一"
fi

echo
echo "== 7. 断言：payload 内无 world-writable（同 pack_ota 断言 0）=="
WW="$(find "$STAGE/payload" \( -type f -o -type d \) -perm -o+w -print)"
if [ -z "$WW" ]; then
    echo "   [✓] 无 world-writable"
else
    echo "   [✗] 存在 world-writable："; printf '%s\n' "$WW" | sed 's/^/     /'; exit 1
fi

echo
echo "== 8. manifest sha256 抽验（bin/lib 全验）=="
if ! python3 - "$STAGE/payload/RELEASE_MANIFEST.json" <<'PY'
import hashlib, json, os, sys
root = os.path.dirname(sys.argv[1])
m = json.load(open(sys.argv[1], encoding="utf-8"))
bad = 0; n = 0
for rel, want in m["files_sha256"].items():
    if not (rel.startswith("bin/") or rel.startswith("lib/")): continue
    n += 1
    got = hashlib.sha256(open(os.path.join(root, rel), "rb").read()).hexdigest()
    if got != want:
        print("     sha256 不符: %s" % rel); bad += 1
print("   校验 %d 项，失败 %d 项" % (n, bad))
sys.exit(1 if bad else 0)
PY
then
    echo "   [✗] 抽验失败"; exit 1
fi

echo
echo "== staging 就绪 =="
echo "路径: $STAGE  ($(du -sh "$STAGE" | cut -f1))"
