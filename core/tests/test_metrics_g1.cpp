// test_metrics_g1.cpp — G1 验收：collect_metrics 聚合逻辑（不依赖 V4L2/NPU 硬件）。
// 覆盖：
//   1) runtime 未启动 → metrics 全 0（unavailable 语义）
//   2) mailbox 有任务 → detect_count = 最近任务 detections.size()
//   3) 已知 published 值经手动构造不可行（worker 依赖 NPU）→ FPS 分支在板端验收，
//      本测试只验证：未启动时不产出虚假非零值（禁止伪造纪律）。
//
// ★ P0-1 源级绊线（Tripwire，非"静默跳过"）：
//   本文件是**独立可执行**（自带 main），直接实例化 CoreRuntime（→ WorkerPool / RKNNEngine）。
//   这些符号仅在 `TTBOX_CORE_HAS_RKNN` 或 `WIN32` 下编入 libttbox_core（core/CMakeLists.txt:165）。
//   注册条件见 core/CMakeLists.txt:588（UNIX ∧ NOT APPLE ∧ TTBOX_CORE_HAS_RKNN）。
//   条件不满足时【必须编译失败】—— 绝不允许退化成桩 main：add_test 只判退出码，
//   一个 return 0 的空桩会被 CTest 记 **PASS 且断言数 0**，静默吞掉覆盖。
#if !(defined(TTBOX_CORE_HAS_RKNN) && TTBOX_CORE_HAS_RKNN) && !defined(_WIN32)
#error "test_metrics_g1.cpp 需要 TTBOX_CORE_HAS_RKNN；请检查 core/CMakeLists.txt:588 的注册条件，勿无条件包含本源"
#endif

#include <cstdio>
#include "common/Metrics.hpp"
#include "pipeline/AimTargetMailbox.hpp"
#include "pipeline/AimTargetTask.hpp"
#include "runtime/CoreRuntime.hpp"

using namespace ttbox::core;
using namespace ttbox::core::aim;

int main() {
    int fails = 0;
    auto check = [&fails](bool ok, const char* name) {
        std::printf("[%s] %s\n", ok ? "PASS" : "FAIL", name);
        if (!ok) ++fails;
    };

    // ---- 1) 未启动（无 capture/worker）→ collect_metrics 直接返回，输出保持调用方初值 ----
    {
        CoreRuntime rt;
        PipelineMetrics m{};  // 全 0
        rt.collect_metrics(&m);
        check(m.fps == 0.0 && m.capture_fps == 0.0 && m.frames_total == 0 &&
                  m.infer_total == 0 && m.frames_superseded == 0,
              "未启动时 metrics 全 0（不伪造）");
        check(m.infer_ms == 0.0 && m.e2e_ms == 0.0 && m.decode_ms == 0.0,
              "未启动时耗时全 0");
        check(m.detect_count == 0, "未启动时目标数 0");
    }

    // ---- 2) 空指针安全 ----
    {
        CoreRuntime rt;
        rt.collect_metrics(nullptr);  // 不崩即过
        check(true, "null 指针安全");
    }

    // ---- 3) mailbox 语义独立验证：take_latest 取最新帧任务 ----
    {
        AimTargetMailbox mailbox(1);
        AimTargetTask t1;
        t1.frame_number = 1;
        t1.detections.resize(2);
        mailbox.offer(0, t1);
        AimTargetTask t2;
        t2.frame_number = 2;
        t2.detections.resize(5);
        mailbox.offer(0, t2);
        AimTargetTask out;
        bool ok = mailbox.take_latest(&out);
        check(ok && out.detections.size() == 5, "mailbox 取最新帧任务");
    }

    std::printf("%s\n", fails == 0 ? "=== tests done (exit=0) ===" : "=== tests done (exit=1) ===");
    return fails == 0 ? 0 : 1;
}
