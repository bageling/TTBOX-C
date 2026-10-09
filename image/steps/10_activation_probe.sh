#!/usr/bin/env bash
# 在镜像 chroot 内**真跑一次激活请求**：拿镜像自带的云端凭据，打真实服务器。
# 目的：把"盒子能不能连上授权服务器"这件事，在烧录包里就定死，不用等现场。
#
# 为什么能测：chroot 共享宿主的网络命名空间（apt 当年就是这么装的），
#   只要 /etc/resolv.conf 能用，出网能力与真机一致（TLS/路由/DNS 全走同一条）。
# 不能测的部分：真机的网卡名、客户防火墙、真机时钟（时钟只在真机才有 RTC 问题）。
set -uo pipefail

echo "########## 0. 环境 ##########"
echo "  date      : $(date)"
echo "  resolv.conf: $(cat /etc/resolv.conf 2>/dev/null | tr '\n' ' ')"

echo
echo "########## 1. DNS 能不能解析授权服务器域名 ##########"
getent hosts cctv2.top || echo "  [✗] 解析失败"

echo
echo "########## 2. TLS 握手通不通 ##########"
curl -s -o /dev/null -w "  HTTP=%{http_code}  tls=%{time_appconnect}s  total=%{time_total}s\n" \
     -m 15 -X POST https://cctv2.top:10086/ttbox/api/client/card-login \
     -H 'Content-Type: application/json' -d '{}' 2>&1
echo "  （000 = 连不出去；401 = 服务在，只是没签名头 —— 这是期望值）"

echo
echo "########## 3. 镜像自带的云端凭据 ##########"
python3 - <<'PY'
import json
d = json.load(open('/opt/ttbox/config/default.json', encoding='utf-8'))
c = d.get('cloud') or {}
print("  license_base_url =", c.get('license_base_url'))
print("  app_key          =", c.get('app_key'))
s = str(c.get('app_secret', ''))
print("  app_secret       = %s…%s（%d 字符）" % (s[:4], s[-4:], len(s)) if s else "  app_secret       = 【空】")
PY

echo
echo "########## 4. 用它自己签名打真实 card-login（root 身份）##########"
cd /opt/ttbox/current/plugins/web
python3 - <<'PY'
import json, sys
sys.path.insert(0, '.')
from lib.cloud_client import CloudLicenseClient, CloudLicenseError
cfg = json.load(open('/opt/ttbox/config/default.json', encoding='utf-8'))
cli = CloudLicenseClient(lambda: cfg)
try:
    r = cli.card_login('PROBE-0000-0000-0000', '9ecf266dc8154491')
    print("  [i] 返回:", json.dumps(r, ensure_ascii=False)[:300])
except CloudLicenseError as e:
    print("  status=%s code=%s" % (getattr(e, 'status', '?'), getattr(e, 'code', '?')))
    print("  message=%s" % getattr(e, 'message', e))
    print("  → 服务器**收到了并回了话**（说明网络/TLS/HMAC 全通），只是这张卡无效")
except Exception as e:
    print("  [✗] 其它异常: %r" % (e,))
PY

echo
echo "########## 5. 换成 web 的真实身份 ttbox 再打一次（抓权限问题）##########"
su -s /bin/sh ttbox -c "cd /opt/ttbox/current/plugins/web && python3 - <<'PY'
import json, sys
sys.path.insert(0, '.')
from lib.cloud_client import CloudLicenseClient, CloudLicenseError
cfg = json.load(open('/opt/ttbox/config/default.json', encoding='utf-8'))
cli = CloudLicenseClient(lambda: cfg)
try:
    r = cli.card_login('PROBE-0000-0000-0000', '9ecf266dc8154491')
    print('  [i] 返回:', json.dumps(r, ensure_ascii=False)[:200])
except CloudLicenseError as e:
    print('  status=%s code=%s message=%s' % (getattr(e,'status','?'), getattr(e,'code','?'), getattr(e,'message',e)))
except Exception as e:
    print('  [✗] 其它异常: %r' % (e,))
PY"

echo
echo "########## 6. core 的 IPC 能不能通（激活成功后要写回 core）##########"
ls -l /run/ttbox 2>/dev/null || echo "  /run/ttbox 不存在（开机由 systemd RuntimeDirectory 建，chroot 内本就没有）"
