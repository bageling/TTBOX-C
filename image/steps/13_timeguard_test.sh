#!/usr/bin/env bash
# 验证 ttbox-time-guard 真的会在"时钟停在 2024"时抬表
#
# ★ 为什么用桩：chroot 与宿主共享**时间命名空间**，真的跑 date -s 会把 WSL 宿主时钟改掉。
#   所以把 date 换成桩：+%s 恒返回 2024-10-22（= 镜像构建日，即无 RTC 板子的开机时钟），
#   -s 只记录不执行。测的是"判据 + 分支 + 取的时间值"，syscall 本身留给真机。
set -u
hr(){ echo "------------------------------------------------------------"; }
[ -x /usr/local/sbin/ttbox-time-guard.sh ] || { echo "!! 守卫脚本不存在"; exit 1; }

hr; echo "[1] 装桩"
STUB=/root/_tg_test/bin
mkdir -p "$STUB"
cat > "$STUB/date" <<'EOS'
#!/bin/sh
# 桩：模拟 RK3588 无 RTC，开机时钟停在镜像构建日 2024-10-22 20:30:27 UTC
for a in "$@"; do [ "$a" = "-s" ] && { echo "[STUB] date $*  (真实机在这里抬表)"; exit 0; }; done
if [ "$1" = "-u" ] && [ "$2" = "+%s" ] && [ "$#" -eq 2 ]; then echo 1729631427; exit 0; fi
exec /bin/date "$@"
EOS
chmod 0755 "$STUB/date"
echo "  桩已就位；模拟开机时钟 = $(/bin/date -u -d @1729631427 '+%Y-%m-%d %H:%M:%S UTC')"
echo "  自检 date -u +%s -> $(PATH="$STUB:$PATH" date -u +%s)  （应=1729631427）"

hr; echo "[2] 场景 A：时钟停在 2024（应当抬表）"
echo "  FLOOR=2026-06-21 → $(/bin/date -u -d '2026-06-21 00:00:00' +%s)"
echo "  模拟 now       → 1729631427"
echo "  --- 守卫输出 ---"
PATH="$STUB:$PATH" /usr/local/sbin/ttbox-time-guard.sh 2>&1 | sed 's/^/    /'
A_RC=$?

hr; echo "[3] 场景 B：时钟已正确（应当什么都不做）"
cat > "$STUB/date" <<'EOS'
#!/bin/sh
for a in "$@"; do [ "$a" = "-s" ] && { echo "[STUB] 不该被调用！"; exit 1; }; done
if [ "$1" = "-u" ] && [ "$2" = "+%s" ] && [ "$#" -eq 2 ]; then
    /bin/date -u +%s; exit 0
fi
exec /bin/date "$@"
EOS
chmod 0755 "$STUB/date"
echo "  当前真实时间: $(/bin/date -u '+%Y-%m-%d %H:%M:%S UTC')"
echo "  --- 守卫输出（应为空）---"
OUT="$(PATH="$STUB:$PATH" /usr/local/sbin/ttbox-time-guard.sh 2>&1)"
[ -z "$OUT" ] && echo "    (无输出 = 未干预，正确)" || echo "    $OUT"

hr; echo "[4] 单元文件检查"
echo "  --- ttbox-time-guard.service ---"
sed 's/^/    /' /etc/systemd/system/ttbox-time-guard.service
echo "  Before= 是否覆盖 timesyncd / ttbox-web："
grep -q 'systemd-timesyncd.service' /etc/systemd/system/ttbox-time-guard.service \
  && echo "    [OK] 早于 timesyncd" || echo "    !! 未排在 timesyncd 之前"
grep -q 'ttbox-web.service' /etc/systemd/system/ttbox-time-guard.service \
  && echo "    [OK] 早于 ttbox-web" || echo "    !! 未排在 web 之前"

hr; echo "[5] 清理"
rm -rf /root/_tg_test
echo "  已清理 /root/_tg_test"
exit 0
