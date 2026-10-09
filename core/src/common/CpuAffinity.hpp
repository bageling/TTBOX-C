// CpuAffinity.hpp — RK3588 大小核亲和性 + 频率锁定工具
//
// 目标（用户需求）：
//   1. 采集（capture）与检测（worker/推理）线程绑定大核（CPU4~7：2×A76+2×A76），
//      避免被调度到小核（CPU0~3）造成采集/推理抖动；NPU 三核（core_mask 1/2/4）
//      并行由 WorkerPool 保证（每 worker 独立 RKNN context + core_mask）。
//   2. CPU 锁频：各 policy 的 scaling_min_freq 锁到最大频率的指定百分比
//      （默认 70%），governor=performance 下保证频率下限，防降频抖动。
//
// 纪律：
//   - Linux 专用（_WIN32 下全部空操作）
//   - 失败仅告警不致命（权限/内核差异容错）
//   - 全部走 sysfs，不依赖外部命令
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace ttbox::core {

// 大小核亲和性 + CPU 锁频工具（Linux 专用；线程绑大核并锁最小频率，失败仅告警）。
class CpuAffinity {
public:
    struct Result {
        bool affinity_ok = false;   // 线程亲和性是否设置成功
        bool freq_ok = false;       // 频率锁定是否全部成功
        std::string detail;         // 人类可读结果（日志用）
    };

    // 把当前线程绑定到指定 CPU 掩码（bit0=cpu0）。mask=0 → 不操作。
    static bool set_thread_affinity(uint64_t mask, std::string* error = nullptr);

    // 把指定 tid 绑定到掩码；tid=0 → 当前线程。
    static bool set_tid_affinity(uint64_t mask, unsigned tid = 0, std::string* error = nullptr);

    // 锁频：把每个 cpufreq policy 的 scaling_min_freq 锁到
    // cpuinfo_max_freq × percent（percent 0~100；<=0 或 >=100 → 跳过锁定）。
    // 需要 root（板端 systemd 服务即 root）。
    static Result lock_min_freq_percent(int percent);

    // RK3588 大核掩码（CPU4~7），供默认调用方使用。
    static constexpr uint64_t kBigCoreMask = 0xF0ULL;
};

}  // namespace ttbox::core
