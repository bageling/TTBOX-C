#!/bin/bash
# a9_ai_isolation.sh — HID 高回报率负载 与 AI Pipeline 并发隔离测试
# 对比：无 HID / HID 8000Hz 绑 CPU0（NPU IRQ 所在核，最坏情况）/ CPU7 / 默认
#
# P8（2026-10-01）：本脚本此前写死板端开发树
#   /home/ubuntu/ttbox2/ttbox/core/build 与 /home/ubuntu/ttbox2/models/huangwa.rknn
# （那是早期 DTB/交叉编译联调期的目录，现板端布局早已是 /opt/ttbox）—— 改为显式变量。
#
# ★ HW 测试二进制目录**刻意不给默认值**。板端发布树 /opt/ttbox 只有
#   releases/V1.0.xx + current 软链，不落构建产物；本脚本要的
#   test_worker_hw / test_hid_load_sim 出自板端 `cmake -DTTBOX_CORE_BUILD_HW_TESTS=ON`
#   的构建目录，那是**临时**目录（位置随人定），没有"正确默认值"可猜。
#   猜一个 /opt/ttbox/build 只会把"确定错"换成"大概率也不存在"，跑起来还是一句
#   cd 失败 —— 不如直接讲清楚要什么。
#   ★ 变量名用 TTBOX_HW_TEST_DIR 而非 TTBOX_BUILD_DIR：后者在登记表 §3.3 里是
#     BUILD 域常量（交叉编译产物目录，ttbox_build_release.sh / fhs_init 消费），
#     语义不同。同名异义正是登记表要禁的事。
HW_TEST_DIR="${TTBOX_HW_TEST_DIR:-}"
if [ -z "$HW_TEST_DIR" ]; then
  echo "需指定板端 HW 测试二进制目录：TTBOX_HW_TEST_DIR=<dir>" >&2
  echo "  该目录须含 test_worker_hw / test_hid_load_sim，由板端构建" >&2
  echo "  cmake -DTTBOX_CORE_BUILD_HW_TESTS=ON 产出（发布树 /opt/ttbox 不含）。" >&2
  exit 1
fi
[ -d "$HW_TEST_DIR" ] || { echo "目录不存在: $HW_TEST_DIR" >&2; exit 1; }
cd "$HW_TEST_DIR" || exit 1
# 模型根默认 /opt/ttbox/models：FHS 化之前的旧布局落点（见 scripts/ttbox_fhs_init.sh
# 的「首次部署：把既有模型复制到 /var/lib/ttbox/models」段），现板端仍可能保留。
MODELS_ROOT="${TTBOX_MODELS_ROOT:-/opt/ttbox/models}"
MODEL="$MODELS_ROOT/huangwa.rknn"

run_ai() {
  ./test_worker_hw --model $MODEL --adapter --workers 3 --cores 4,5,6 \
    --buffers 8 --frames 1000 --inw 320 --inh 320 2>&1 | \
    grep -E 'capture FPS|总吞吐|错误|poll_timeout' | tr '\n' ' | '
  echo ""
}

echo "===== [基线] 无 HID 负载 ====="
run_ai

echo "===== [HID 8000Hz @ CPU0=NPU IRQ] 与 AI 并发 ====="
./test_hid_load_sim --rate 8000 --cpu 0 --duration 9 >/tmp/hid_cpu0.log 2>&1 &
HID_PID=$!
sleep 1
run_ai
wait $HID_PID
grep '目标 rate' /tmp/hid_cpu0.log

echo "===== [HID 8000Hz @ CPU7] 与 AI 并发 ====="
./test_hid_load_sim --rate 8000 --cpu 7 --duration 9 >/tmp/hid_cpu7.log 2>&1 &
HID_PID=$!
sleep 1
run_ai
wait $HID_PID
grep '目标 rate' /tmp/hid_cpu7.log

echo "===== [HID 8000Hz @ 默认调度] 与 AI 并发 ====="
./test_hid_load_sim --rate 8000 --duration 9 >/tmp/hid_def.log 2>&1 &
HID_PID=$!
sleep 1
run_ai
wait $HID_PID
grep '目标 rate' /tmp/hid_def.log

echo ""
echo "===== NPU IRQ 分布（AI+HID 并发后）====="
# IRQ 名不写死 SoC 地址（P8）：按名字匹配 npu/iommu，换 SoC 不用改脚本
grep -iE 'npu|iommu' /proc/interrupts
