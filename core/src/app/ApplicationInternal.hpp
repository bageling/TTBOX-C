// ApplicationInternal.hpp — Application 四个编译单元**共享**的内部辅助
//
// ★ 为什么需要这个文件（2026-10-04 core 结构治理 S1 的硬前提）：
//   Application.cpp 拆成 4 个 TU 之前，下面这些函数/常量都住在同一个文件的
//   **匿名命名空间**里。匿名命名空间按标准对"每个 TU 各自一份副本"，跨 TU 不可见。
//   拆分后它们被三个不同 TU 使用 —— 若仍留在原 TU 的匿名命名空间，
//   其他 TU 引用时得到的是**未声明**（更糟：本地若恰好有无同名的静默用错），
//   链接期报 undefined reference。
//   ⇒ 提到这里、加 inline，让 4 个 TU 共享同一实体。
//
// ★ 为什么放在 app/ 而不是 common/：
//   这些辅助**只服务 Application**，不是全局通用工具。放 common/ 会诱使
//   别的模块来用，等于把"应用层的实现细节"升格成"公共 API" —— 与这次治理
//   "按职责收窄可见性"的方向相反。
//
// ★ 为什么要 inline（而不是 .cpp + 声明）：
//   这些函数都很短，且被多个 TU 使用；inline 让每个 TU 自持一份副本，
//   省掉一个 .o 与一次函数调用。**不要给本文件加 .cpp** —— 一旦有了 .cpp，
//   忘记把新函数写进 .cpp 就是 undefined reference，且这类错**编译期不报**。
#pragma once

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdlib>
#include <cstdint>
#include <filesystem>
#include <sstream>
#include <string>
#include <system_error>
#include <vector>

#include "common/ConfigDefaults.hpp"
#include "model/ModelRegistry.hpp"

namespace ttbox::core::app_internal {

// ---- 时间 ----
// 单调时钟毫秒。★ 不能用 gettimeofday/系统时钟：那是墙钟，NTP 校时会让
//   毫秒值倒退，主循环的 deadline 比较会算出巨大的负间隔。
inline double now_ms() {
    using clock = std::chrono::steady_clock;
    return static_cast<double>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            clock::now().time_since_epoch())
            .count());
}

// ---- 字符串 ----
// 去首尾空白。参数用 const& 而非临时对象，避免"接字符串字面量时创建临时 string"。
inline std::string strip(const std::string& s) {
    size_t a = 0, b = s.size();
    while (a < b && std::isspace(static_cast<unsigned char>(s[a]))) ++a;
    while (b > a && (std::isspace(static_cast<unsigned char>(s[b - 1])) ||
                     s[b - 1] == '\n' || s[b - 1] == '\r')) --b;
    return s.substr(a, b - a);
}

// ---- 配置字面量解析 ----
// 返回值取自 cfg::（common/ConfigDefaults.hpp，== deploy/config/00-factory.json），
// 不在这里另写一份字面量。
inline int parse_color_order(const std::string& s) {
    if (s == "rgb") return 1;
    return 0;
}

inline std::vector<int> parse_worker_cores(const std::string& s) {
    if (s.empty()) return cfg::default_worker_cores();

    std::vector<int> result;
    std::istringstream iss(s);
    std::string token;
    while (std::getline(iss, token, ',')) {
        // 去掉首尾空格和换行
        size_t start = token.find_first_not_of(" \t\r\n");
        size_t end = token.find_last_not_of(" \t\r\n");
        if (start == std::string::npos) continue;
        token = token.substr(start, end - start + 1);

        try {
            int core = std::stoi(token);
            // RKNN core_mask 是位掩码，不是 CPU 编号。RK3588 三个 NPU
            // 核心的独占掩码只有 1、2、4；3/5/6 会造成核心重叠调度。
            if (core == 1 || core == 2 || core == 4) {
                if (std::find(result.begin(), result.end(), core) == result.end()) {
                    result.push_back(core);
                }
            }
        } catch (...) {
            // 跳过非法值，不报错
        }
    }
    // 配置中出现非法/重叠 mask 时回退到稳定的独占组合，避免把错误
    // 参数直接传给 RKNN 后出现吞吐下降或不同版本驱动行为不一致。
    return result.empty() ? cfg::default_worker_cores() : result;
}

// ---- 模型仓库 ----
// 收件目录：<registry_root>/_incoming。Gateway 上传端点先把文件写到这里，
// 再发 MODEL_IMPORT 引用路径。import 只允许引用收件目录内的文件（防任意路径读取）。
inline std::string incoming_dir_of(const ModelRegistry& reg) {
    return reg.root_dir() + "/_incoming";
}

// ---- 更新冒烟自检（2026-09-20 方案B）----
// 自检最长等待（ms）：更新器异常/未写终态时的兜底停回时限，绝不无限运行。
// ★ 200s 的由来：旧更新器健康门禁最长等 180s，本值留 20s 余量。
inline constexpr double kPostUpdateSmokeMaxMs = 200000.0;

}  // namespace ttbox::core::app_internal
