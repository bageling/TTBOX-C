#!/bin/bash
# a9_hidg_write_test.sh — 向 hidg0/hidg1 写入测试报告（验证主机枚举状态）
# 主机未连接时 write 应返回 ENODEV/EAGAIN；连接后应成功。
set -e
echo "== 向 hidg0（键盘）写入 8 字节空报告 =="
python3 - <<'EOF'
import os, errno
for name, size in [("/dev/hidg0", 8), ("/dev/hidg1", 4)]:
    try:
        fd = os.open(name, os.O_WRONLY | os.O_NONBLOCK)
        try:
            n = os.write(fd, b"\x00" * size)
            print(f"{name}: write OK ({n} bytes) → 主机已枚举")
        except OSError as e:
            print(f"{name}: write {e.errno} ({os.strerror(e.errno)}) "
                  f"{'(主机未枚举/未连接 USB-C)' if e.errno in (errno.ENODEV, errno.EAGAIN) else ''}")
        finally:
            os.close(fd)
    except OSError as e:
        print(f"{name}: open {e.errno} ({os.strerror(e.errno)})")
EOF
echo "== 检查 USB-C 是否接主机（typec 连接状态）=="
cat /sys/class/typec/port0/data_role 2>/dev/null
ls /sys/class/typec/port0-partner/ 2>/dev/null | head
echo "== 写入压力测试（1000 次键盘报告，测 ENODEV 语义）=="
python3 - <<'EOF'
import os, time
fd = os.open("/dev/hidg0", os.O_WRONLY | os.O_NONBLOCK)
ok = 0; err = 0
t0 = time.time()
for i in range(1000):
    try:
        os.write(fd, b"\x00" * 8)
        ok += 1
    except OSError:
        err += 1
dt = time.time() - t0
os.close(fd)
print(f"write: ok={ok} err={err} in {dt*1000:.1f}ms")
EOF
