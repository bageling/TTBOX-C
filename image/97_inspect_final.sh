#!/usr/bin/env bash
# 宿主侧（WSL）· 成品镜像只读抽查：激活所需凭据到底在不在
# 只读挂载（mount -o ro），不写盘。
set -u
IMG="${IMG:-${1:-}}"
[ -n "$IMG" ] && [ -f "$IMG" ] || { echo "需指定镜像：IMG=<xxx.img> 或第一个参数" >&2; exit 1; }
M="${M:-/mnt/final}"
mkdir -p "$M"

echo "== 只读挂载 =="
LOOP="$(losetup --find --show --partscan "$IMG")"
echo "  loop=$LOOP"
sleep 1
mount -o ro "${LOOP}p2" "$M" && echo "  已只读挂载 ${LOOP}p2 -> $M"

echo
echo "== 1. /opt/ttbox/config/ 清单 =="
ls -l "$M/opt/ttbox/config/" 2>&1

echo
echo "== 2. /opt/ttbox/config/default.json（**只放云端凭据**，运行期配置真源是 core 的 config.d）=="
python3 - "$M/opt/ttbox/config/default.json" <<'PY'
import json, sys, os
p = sys.argv[1]
if not os.path.isfile(p):
    print("  [✗] 文件不存在:", p); sys.exit(0)
d = json.load(open(p, encoding='utf-8'))
print("  文件大小:", os.path.getsize(p), "字节")
print("  顶层键数:", len(d))
c = d.get('cloud')
if c:
    mask = lambda s: s[:4] + '…' + s[-4:] + '(%d字符)' % len(s)
    print("  cloud 段:")
    for k in ('license_base_url','app_key','app_secret'):
        print("    %-18s = %s" % (k, mask(c[k]) if k == 'app_secret' else c.get(k)))
else:
    print("  cloud 段: 【不存在】 ⇒ 客户激活必失败")
# 运行期配置**不在**这个文件里：core 读 /etc/ttbox/config.d，web 只从这里取 cloud.*
print("  注：运行期参数（conf/nms/capture…）在 /etc/ttbox/config.d/00-factory.json，"
      "本文件按设计只有 cloud 段")
PY

echo
echo "== 3. /etc/ttbox 与 config.d =="
ls -l "$M/etc/ttbox/" "$M/etc/ttbox/config.d/" 2>&1

echo
echo "== 4. 10-device.json 的 model_id 与 capture =="
python3 - "$M/etc/ttbox/config.d/10-device.json" <<'PY'
import json, sys, os
p = sys.argv[1]
if not os.path.isfile(p):
    print("  [✗] 不存在:", p); sys.exit(0)
d = json.load(open(p, encoding='utf-8'))
rp = d.get('runtime_profile', {})
print("  model_id   =", repr(rp.get('model_id')))
print("  capture    =", rp.get('capture'))
print("  model_label=", repr(d.get('model_label')))
print("  model_registry_root =", d.get('model_registry_root'))
PY

echo
echo "== 5. 模型目录现状（按'干净出厂'应为空） =="
if [ -d "$M/var/lib/ttbox/models" ]; then
    ( cd "$M/var/lib/ttbox/models" && find . -maxdepth 2 -printf '  %y %M %p\n' )
else
    echo "  [✗] /var/lib/ttbox/models 不存在"
fi

echo
echo "== 6. 其它出厂应存在的路径 =="
for p in /opt/ttbox/presets /opt/ttbox/config/motion-profiles /var/lib/ttbox/license \
         /var/lib/ttbox/ota/jobs /var/lib/ttbox/ota/processed /opt/ttbox/config/default.json; do
    if [ -e "$M$p" ]; then printf '  %s  %s\n' "$(stat -c '%U:%G:%a' "$M$p")" "$p"
    else printf '  【缺】%s\n' "$p"; fi
done

echo
echo "== 卸载 =="
umount "$M" && rmdir "$M"
losetup -d "$LOOP" && echo "  已释放 $LOOP"
