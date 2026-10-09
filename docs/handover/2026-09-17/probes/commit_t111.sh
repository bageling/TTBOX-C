#!/bin/bash
set -u
cd /mnt/c/Users/Administrator/Desktop/TTBOX-Module-Edition-main || exit 1

echo "===== 可执行位 ====="
chmod 0755 scripts/ttbox_ota_updater.py tools/ota/ttbox_ota_sign.py tools/ota/test_verify.py
ls -l scripts/ttbox_ota_updater.py tools/ota/ttbox_ota_sign.py tools/ota/test_verify.py | awk '{print $1, $NF}'

echo "===== 暂存（逐路径，不用 -A）====="
git add .gitignore
git add plugins/web/bin/ttbox-web.py
git add scripts/ttbox_ota_updater.py
git add tools/ota/ttbox_ota_sign.py
git add tools/ota/test_verify.py
git add tools/ota/keys/ttbox-ota-2026a.pub

echo "===== 暂存集（必须恰 6 项且无私钥）====="
git diff --cached --name-only
echo "count=$(git diff --cached --name-only | wc -l)"
echo "私钥泄漏检查: $(git diff --cached --name-only | grep -c 'priv.pem') (必须 0)"

echo "===== 提交 ====="
git commit -F - <<'MSG'
feat(ota): T1.11 OTA 验签设备端（SEC-01）+ OTA-01 调度 + OTA-02 健康检查回滚

M1 必达 28 行里唯一"代码为零"的一项。按 t1.11-impl-spec.md 落地：

- tools/ota/ttbox_ota_sign.py（新）：离线签名工具（Ed25519，key_id=ttbox-ota-2026a）
- tools/ota/keys/ttbox-ota-2026a.pub（新）：**只入库公钥**；私钥落 .testkeys/ 已 gitignore
- scripts/ttbox_ota_updater.py（新）：root 独立进程，结构必经七步
  ① https scheme 白名单 → ② 下载到临时目录 → ③ sha256 → ④ Ed25519 →
  ⑤ 展开 staging → ⑥ RELEASE_MANIFEST 全量复验 → ⑦ T1.01 原子发布 →
  健康检查（业务能力）→ 失败自动 rollback；finally 清临时目录
  · 双因子是 and（sha256 与签名缺一不可）
  · tar 展开做成员名/越界校验（root 解不可信包的 RCE 面）
  · 依赖全部可注入（fetch/install/rollback/health/pubkey），测试替身不开生产后门
- tools/ota/test_verify.py（新，REG-01，Python 侧不并入 ctest）：7 用例全绿
  篡改 6 个偏移拒绝率 100% / http 零下载 / key_id 不符拒 / 双因子 and /
  失败零残留 / 健康检查失败必 rollback
- plugins/web/bin/ttbox-web.py：新建 /api/ota/install —— systemd-run --collect
  --on-active=2s 拉起 updater 后立即返回（OTA-01：更新器脱离 Web，避免"自己杀自己"）；
  非白名单 ⇒ 未登录由 before_request 401（401 先于动作）
- .gitignore：排除 tools/ota/.testkeys/ 与 *.priv.pem（私钥永不入库）

★ 契约裁定（须架构师复核）：规格 §3.1 要求 UPDATE_SIGN 在 tgz 内、且 sha256 = 整个 tgz，
  二者同时成立会自指（§6 陷阱 3）。取"保 sha256=整个 tgz"⇒ 签名改为**旁车
  <pkg>.tgz.sign.json**（包外）。理由与推演写在 ttbox_ota_sign.py 文件头。

验证：单测 0 failures；私钥门禁 0；Web 回归 18 passed/1 failed（失败项与改动前
同为该既存 werkzeug 版本问题，无新增回归）。
MSG
echo "commit_rc=$?"
echo "===== 提交后 ====="
git log --oneline -3
git status --porcelain
