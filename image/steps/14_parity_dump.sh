#!/usr/bin/env bash
# 与"正在正常跑的开发板"逐项对表用的镜像侧快照（配合宿主侧脚本 diff）
# 输出刻意做成可 diff 的纯文本，方便与板端同名输出比对
set -u
OUT=/root/_bake/parity_image.txt
{
echo "########## 1. 配置层 /etc/ttbox/config.d ##########"
for f in $(ls /etc/ttbox/config.d/*.json 2>/dev/null | sort); do
  echo "--- $f ---"
  python3 - "$f" <<'PY'
import json,sys
d=json.load(open(sys.argv[1]))
def flat(p,v,out):
    if isinstance(v,dict):
        for k in sorted(v): flat(p+"."+k if p else k, v[k], out)
    elif isinstance(v,list): out.append("   %-40s = [list %d]"%(p,len(v)))
    else: out.append("   %-40s = %s"%(p,v))
out=[]; flat("",d,out)
print("\n".join(out))
PY
done
echo
echo "########## 2. web 云端凭据 ##########"
python3 -c "
import json
d=json.load(open('/opt/ttbox/config/default.json'))
print('  顶层键:', sorted(d))
c=d.get('cloud',{})
print('  base_url:',c.get('license_base_url'))
print('  app_key :',c.get('app_key'))
print('  secret_len:',len(c.get('app_secret') or ''))
"
echo
echo "########## 3. 运行目录与权限 ##########"
for d in /opt/ttbox/presets /opt/ttbox/config/motion-profiles /var/lib/ttbox/license \
         /var/lib/ttbox/ota/processed /var/lib/ttbox/models /var/lib/ttbox/models/installed \
         /var/lib/ttbox/models/registry /var/lib/ttbox/hid; do
  printf '  %-46s %s\n' "$d" "$(stat -c '%A %U:%G' "$d" 2>/dev/null || echo '不存在')"
done
echo
echo "########## 4. 已启用 unit（文件系统真身）##########"
for w in /etc/systemd/system/multi-user.target.wants/*.service \
         /etc/systemd/system/multi-user.target.wants/*.timer \
         /etc/systemd/system/multi-user.target.wants/*.path \
         /etc/systemd/system/sysinit.target.wants/*.service \
         /etc/systemd/system/timers.target.wants/*; do
  [ -L "$w" ] && echo "  $(basename "$w")"
done | sort -u
echo
echo "########## 5. 版本 ##########"
echo "  current -> $(readlink -f /opt/ttbox/current)"
echo
echo "########## 6. 版本自洽（RELEASE_BUILD.md 的 commit/md5）##########"
head -8 /opt/ttbox/current/RELEASE_BUILD.md 2>/dev/null | sed 's/^/  /'
echo
echo "########## 7. 时间守卫 ##########"
echo "  unit  存在: $([ -f /etc/systemd/system/ttbox-time-guard.service ] && echo yes || echo NO)"
echo "  脚本  存在: $([ -x /usr/local/sbin/ttbox-time-guard.sh ] && echo yes || echo NO)"
echo "  fake-hwclock.data: $(cat /etc/fake-hwclock.data 2>/dev/null)"
echo
echo "########## 8. 模型 ##########"
find /var/lib/ttbox/models -type f -printf '  %s\t%M %u:%g %p\n' 2>/dev/null | sort -k4
} > "$OUT" 2>&1
cat "$OUT"
