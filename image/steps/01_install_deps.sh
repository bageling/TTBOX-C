#!/usr/bin/env bash
# 步骤 01 · 补装 TTBOX 运行依赖（清单来自 ldd 闭包实测，非照抄文档）
#
# 依据（2026-09-20 板端实测）：
#   ldd /opt/ttbox/current/bin/ttbox_core_main 在板端解析出：
#     librga.so.2, librknnrt.so, libjpeg.so.8,
#     libopencv_imgproc.so.4.5d, libopencv_core.so.4.5d, libtbb.so.2 …
#   逐条 dpkg -S 归属后得出本清单。镜像内 libc6/libstdc++6/libgcc-s1/zlib1g/
#   libjpeg-turbo8 已具备，故不列入。
#
# 明确【不装】：
#   numpy  —— 板端实测 import numpy 直接 ModuleNotFoundError，而 TTBOX 正常运行；
#             plugins/web 的 import 清单里也无 numpy。DEPENDENCIES.md 那条是虚的。
#   *-dev  —— 板上不编译，只需运行库。
set -u

export DEBIAN_FRONTEND=noninteractive
export LC_ALL=C

echo "===== 1. 阻止安装期自动起服务（chroot 内无 PID1）====="
cat > /usr/sbin/policy-rc.d <<'EOF'
#!/bin/sh
exit 101
EOF
chmod 0755 /usr/sbin/policy-rc.d
echo "  已写 /usr/sbin/policy-rc.d (exit 101)"

echo
echo "===== 2. 检查 jjriek/rockchip PPA 的 GPG 公钥 ====="
PPA_OK=1
if ls /etc/apt/trusted.gpg.d/ 2>/dev/null | grep -qi 'jjriek\|rockchip'; then
    echo "  [✓] trusted.gpg.d 有 jjriek/rockchip 密钥"
    ls /etc/apt/trusted.gpg.d/ | grep -i 'jjriek\|rockchip' | sed 's/^/      /'
else
    echo "  [!] trusted.gpg.d 未见 jjriek/rockchip 密钥，查 keyrings.d："
    ls /etc/apt/keyrings/ /usr/share/keyrings/ 2>/dev/null | grep -i 'jjriek\|rockchip' || echo "      (也没有)"
    PPA_OK=0
fi

echo
echo "===== 3. apt-get update ====="
apt-get update 2>&1 | tail -15
UPD_RC=$?
echo "  update 退出码=$UPD_RC"

echo
echo "===== 4. 候选版本确认（装之前先看是谁提供）====="
for p in librga2 libopencv-core4.5d libopencv-imgproc4.5d python3-flask python3-waitress v4l-utils; do
    printf '%-24s ' "$p"
    if apt-cache policy "$p" 2>/dev/null | grep -q 'Candidate: [0-9]'; then
        apt-cache policy "$p" 2>/dev/null | sed -n 's/^  Candidate: /候选 /p' | head -1
    else
        echo "** 无候选（源不可用或包名不对）**"
    fi
done

echo
echo "===== 5. 安装 ====="
PKGS="librga2 libopencv-core4.5d libopencv-imgproc4.5d python3-flask python3-waitress v4l-utils"
apt-get install -y --no-install-recommends $PKGS 2>&1 | tail -40
INST_RC=$?
echo "  install 退出码=$INST_RC"

echo
echo "===== 6. 落地核实：文件真的在吗 ====="
for f in /usr/lib/aarch64-linux-gnu/librga.so.2 \
         /usr/lib/aarch64-linux-gnu/libopencv_core.so.4.5d \
         /usr/lib/aarch64-linux-gnu/libopencv_imgproc.so.4.5d \
         /usr/lib/aarch64-linux-gnu/libtbb.so.2 \
         /usr/lib/aarch64-linux-gnu/libjpeg.so.8 \
         /usr/bin/v4l2-ctl; do
    if [ -e "$f" ]; then printf '  [✓] %s\n' "$f"; else printf '  [✗] %s 缺失\n' "$f"; fi
done
printf '  python3 -c "import flask, waitress" : '
python3 -c "import flask, waitress; print('OK  flask', flask.__version__)" 2>&1 | tail -1

echo
echo "===== 7. 清理 apt 缓存（回写镜像时省空间）====="
apt-get clean
rm -rf /var/lib/apt/lists/*
echo "  已清理 /var/cache/apt/archives 与 /var/lib/apt/lists"

echo
echo "===== 8. 收尾：删掉 policy-rc.d（不能留在成品镜像里）====="
rm -f /usr/sbin/policy-rc.d
echo "  已删除 /usr/sbin/policy-rc.d"

echo
echo "===== 9. 磁盘余量 ====="
df -h /
