// RtSched.hpp — core 线程实时调度 + 锁页工具
//
// 为什么存在（2026-09-22 板端实测）：
//   ttbox-core 全部线程都是普通 TS 调度，且 systemd 单元 LimitRTPRIO=0。
//   采集线程在 1080p60 下每 16.7ms 要完成 poll→DQBUF→发布，被普通调度抢占
//   一次就体现为 buffer_age_ms 抖动 → 端到端延迟尾部长。
//
// 纪律：
//   - 权限自己放开（root 进程 setrlimit），**不依赖 systemd 单元的
//     LimitRTPRIO/LimitMEMLOCK** —— 客户机上跑的可能是镜像里那份旧单元，
//     OTA 不保证覆盖单元文件，指望外部配置 = 把生效条件放在自己控制之外。
//   - 申请失败只告警、降级为普通调度，绝不让服务起不来。
//   - 设完要读回来确认（存在性 ≠ 生效）。
//   - apply_fifo 必须在**目标线程内部**调用；在 start() 里调只会作用于调用
//     线程（WorkerPool.cpp 里已有同一处教训）。
//   - Linux 专用（_WIN32 下全部空操作）。
#pragma once

#include <cstdint>
#include <string>

namespace ttbox::core {

// 实时调度与锁页工具：进程级准备（提升 RLIMIT） + 线程 SCHED_FIFO（Linux 专用）。
class RtSched {
public:
    // RT 优先级上限：必须低于 usb-proxy 的鼠标通路（98），
    // 否则采集线程会反过来抢掉 HID 下发，端到端反而变差。
    static constexpr int kMaxPriority = 70;

    // 进程级准备：放开 RLIMIT_RTPRIO / RLIMIT_MEMLOCK，然后 mlockall。
    // 应在启动时调用一次（在任何采集/推理线程起来之前）。
    // detail 返回人类可读结果（无论成败），返回 true 表示至少 RT 权限可用。
    static bool prepare_process(std::string* detail = nullptr);

    // 给【当前线程】上 SCHED_FIFO（+ 可选单核绑定）。
    // role 决定环境变量名：TTBOX_CORE_<ROLE>_RT_PRIORITY / TTBOX_CORE_<ROLE>_CPU。
    // prio<=0 → 不启用；cpu<0 → 不绑（沿用 CpuAffinity 的大核掩码）。
    // 返回 true 表示**读回确认**已是 SCHED_FIFO。
    static bool apply_fifo(const std::string& role, int prio_default, int cpu = -1,
                           std::string* detail = nullptr);

    // 读环境变量整数（空/非法 → def）。
    static int env_int(const char* name, int def);
};

}  // namespace ttbox::core
