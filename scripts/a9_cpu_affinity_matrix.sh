#!/bin/bash
# a9_cpu_affinity_matrix.sh — HID 线程 CPU affinity 实验（合成 1000Hz 负载）
# 测试每个 CPU 绑定的实际率/开销；随后与 AI Pipeline 并发验证隔离。
cd /home/ubuntu/ttbox2/ttbox/core/build

echo "===== HID 负载 CPU affinity 矩阵（1000Hz, 3s）====="
echo "注：NPU IRQ 45/46/47 集中在 CPU0（/proc/interrupts 实测）"
for c in -1 0 1 2 3 4 5 6 7; do
  lab=$c
  [ "$c" = "-1" ] && lab="default"
  out=$(./test_hid_load_sim --rate 1000 --cpu $c --duration 3 2>&1 | grep '目标 rate')
  echo "  cpu=$lab : $out"
done

echo ""
echo "===== 8000Hz 高回报率 × CPU affinity ====="
for c in -1 0 4 6; do
  lab=$c
  [ "$c" = "-1" ] && lab="default"
  out=$(./test_hid_load_sim --rate 8000 --cpu $c --duration 3 2>&1 | grep '目标 rate')
  echo "  cpu=$lab : $out"
done
