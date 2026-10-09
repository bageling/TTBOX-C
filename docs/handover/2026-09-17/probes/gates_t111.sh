#!/bin/bash
cd /mnt/c/Users/Administrator/Desktop/TTBOX-Module-Edition-main || exit 1
export PYTHONDONTWRITEBYTECODE=1

echo "===== 1) 语法 ====="
for f in tools/ota/ttbox_ota_sign.py tools/ota/test_verify.py scripts/ttbox_ota_updater.py plugins/web/bin/ttbox-web.py; do
  python3 -m py_compile "$f" && echo "  OK  $f" || echo "  FAIL $f"
done
rm -rf tools/ota/__pycache__ scripts/__pycache__ plugins/web/bin/__pycache__ 2>/dev/null

echo
echo "===== 2) §4.4 私钥门禁（硬 FAIL）====="
echo "-- 产物/scripts/plugins --"
grep -rn 'BEGIN.*PRIVATE KEY' scripts/ plugins/ 2>/dev/null | grep -v '\.test' ; echo "  命中=$(grep -rn 'BEGIN.*PRIVATE KEY' scripts/ plugins/ 2>/dev/null | grep -cv '\.test')  (期望 0)"
echo "-- tools/ota/keys（公钥目录，生产私钥不得在此）--"
grep -rn 'BEGIN.*PRIVATE KEY' tools/ota/keys/ 2>/dev/null | grep -v '\.test' ; echo "  命中=$(grep -rn 'BEGIN.*PRIVATE KEY' tools/ota/keys/ 2>/dev/null | grep -cv '\.test')  (期望 0)"
echo "-- 入库公钥 --"
ls -l tools/ota/keys/ 2>/dev/null

echo
echo "===== 3) 私钥目录必须被忽略（不得入库）====="
git check-ignore -v tools/ota/.testkeys/ttbox-ota-2026a.priv.pem && echo "  已忽略 OK" || echo "  ⚠ 未忽略"
git status --porcelain | grep -c 'priv.pem' | xargs echo "  未跟踪私钥数(期望 0) ="

echo
echo "===== 4) Web 测试回归（不得因新增端点回退）====="
python3 /mnt/c/Users/Administrator/Desktop/run_pytest_repo.py "$PWD" plugins/web/tests/test_web_auth.py 2>&1 | tail -3

echo
echo "===== 5) OTA 单测 ====="
python3 tools/ota/test_verify.py 2>&1 | tail -3

echo
echo "===== 6) git status ====="
git status --porcelain
