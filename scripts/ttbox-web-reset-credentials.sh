#!/bin/sh
# ttbox-web-reset-credentials.sh — T1.12（SEC-02）M1 物理恢复
#
# 作用（design.md §A10 / t1.12-impl-spec.md §2.8）：
#   清除 Web 管理员凭据 → 回"首次设置"页。这是 M1 唯一的忘记密码恢复路径
#   （无邮箱/无短信/无后门口令 —— DECISIONS Q8「无默认口令」的必然推论）。
#
# 调用方：
#   · M1：管理员在设备上手工以 root 执行；
#   · DEP-05：长按按钮 10s 的 GPIO 服务届时调用本脚本（**本任务不实现按钮服务**）。
#
# 边界（诚实记录）：
#   · 本脚本**不删**审计/授权数据，只删 Web 凭据；
#   · 会话仅存内存（§2.2），故"重启即踢光所有会话"；
#   · 以 root 运行 ⇒ 非 root 一律 exit 0 且不做任何修改（不制造半套恢复，
#     对齐 scripts/ttbox_ensure_services.sh 的既有契约）。
set -eu

CRED_PATH='/etc/ttbox/web_credentials.json'
UNIT='ttbox-web.service'

if [ "$(id -u)" != '0' ]; then
    echo "ttbox-web-reset-credentials: 需要 root（当前 uid=$(id -u)），未做任何修改" >&2
    exit 0
fi

rm -f "$CRED_PATH"

# 重启即清空内存会话表（会话不落盘，见 §2.2）
if systemctl restart "$UNIT" 2>/dev/null; then
    echo "web credentials cleared; $UNIT restarted; re-open setup page"
else
    echo "web credentials cleared; $UNIT restart skipped (systemd 不可用?); re-open setup page"
fi

exit 0
