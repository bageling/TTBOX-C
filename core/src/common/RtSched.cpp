// RtSched.cpp — core 线程实时调度 + 锁页实现（Linux）
#include "common/RtSched.hpp"

#if !defined(_WIN32)

#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <pthread.h>
#include <sched.h>
#include <sys/mman.h>
#include <sys/resource.h>
#include <unistd.h>

#include "common/Logger.hpp"

namespace ttbox::core {

namespace {

// 把某个 rlimit 的软/硬限制一起顶到 RLIM_INFINITY。
// 只动软限制不够：非 root 时硬限制是天花板，我们这里靠 root 直接抬硬限制。
bool raise_limit(int resource, const char* name, std::string* detail) {
    struct rlimit rl {};
    if (::getrlimit(resource, &rl) != 0) {
        if (detail) *detail += name + std::string(": getrlimit 失败: ") + std::strerror(errno) + "; ";
        return false;
    }
    if (rl.rlim_cur == RLIM_INFINITY) {
        return true;
    }
    struct rlimit want = rl;
    want.rlim_cur = RLIM_INFINITY;
    want.rlim_max = RLIM_INFINITY;
    if (::setrlimit(resource, &want) != 0) {
        // 抬硬限制失败（非 root / 容器）时退一步：只抬软限制到当前硬限制。
        want.rlim_max = rl.rlim_max;
        want.rlim_cur = rl.rlim_max;
        if (::setrlimit(resource, &want) != 0) {
            if (detail) *detail += name + std::string(": setrlimit 失败: ") + std::strerror(errno) + "; ";
            return false;
        }
        if (detail) *detail += name + std::string(": 仅抬到硬限制; ");
        return true;
    }
    return true;
}

}  // namespace

// 读环境变量为整数；未设置或为空 → def。
int RtSched::env_int(const char* name, int def) {
    const char* v = std::getenv(name);
    if (!v || !*v) {
        return def;
    }
    return std::atoi(v);
}

// 进程级准备：放开 RLIMIT_RTPRIO/MEMLOCK、mlockall，并读回确认真实生效。
bool RtSched::prepare_process(std::string* detail) {
    std::string d;
    bool rt_ok = raise_limit(RLIMIT_RTPRIO, "RLIMIT_RTPRIO", &d);

    if (env_int("TTBOX_CORE_MLOCK", 1) > 0) {
        raise_limit(RLIMIT_MEMLOCK, "RLIMIT_MEMLOCK", &d);
        if (::mlockall(MCL_CURRENT | MCL_FUTURE) != 0) {
            d += "mlockall 失败: " + std::string(std::strerror(errno)) + "; ";
        } else {
            d += "mlockall: locked; ";
        }
    } else {
        d += "mlockall: 由 TTBOX_CORE_MLOCK=0 关闭; ";
    }

    // 读回确认，别只信 setrlimit 的返回值。
    struct rlimit cur {};
    if (::getrlimit(RLIMIT_RTPRIO, &cur) == 0) {
        if (cur.rlim_cur == RLIM_INFINITY) {
            d += "RLIMIT_RTPRIO=infinity(已生效)";
            rt_ok = true;
        } else {
            d += "RLIMIT_RTPRIO 软限制=" + std::to_string(static_cast<long long>(cur.rlim_cur));
            rt_ok = (cur.rlim_cur > 0);
        }
    }

    if (detail) {
        *detail = d;
    }
    if (rt_ok) {
        TTBOX_LOG_INFO("RtSched 进程准备完成: " + d);
    } else {
        TTBOX_LOG_WARN("RtSched 进程准备未完成（将降级为普通调度）: " + d);
    }
    return rt_ok;
}

// 给当前线程上 SCHED_FIFO（可选单核绑定），并读回确认策略已生效。
bool RtSched::apply_fifo(const std::string& role, int prio_default, int cpu, std::string* detail) {
    std::string d;
    const std::string prio_name = "TTBOX_CORE_" + role + "_RT_PRIORITY";
    const std::string cpu_name = "TTBOX_CORE_" + role + "_CPU";
    int prio = env_int(prio_name.c_str(), prio_default);
    const int cpu_pin = env_int(cpu_name.c_str(), cpu);

    if (prio <= 0) {
        d = role + ": RT 由 " + prio_name + "<=0 关闭";
        if (detail) *detail = d;
        TTBOX_LOG_INFO("RtSched " + d);
        return false;
    }
    if (prio > kMaxPriority) {
        d = role + ": 请求优先级 " + std::to_string(prio) + " 超过上限 " +
            std::to_string(kMaxPriority) + "，已夹到上限（不得盖过 usb-proxy 鼠标通路）";
        prio = kMaxPriority;
    }

    struct sched_param sp {};
    sp.sched_priority = prio;
    if (::pthread_setschedparam(::pthread_self(), SCHED_FIFO, &sp) != 0) {
        d += role + ": SCHED_FIFO(" + std::to_string(prio) + ") 失败: " +
             std::strerror(errno) + "（降级为普通调度）";
        if (detail) *detail = d;
        TTBOX_LOG_WARN("RtSched " + d);
        return false;
    }

    if (cpu_pin >= 0) {
        cpu_set_t set;
        CPU_ZERO(&set);
        CPU_SET(static_cast<size_t>(cpu_pin), &set);
        if (::sched_setaffinity(0, sizeof(set), &set) != 0) {
            d += role + ": 绑核 CPU" + std::to_string(cpu_pin) + " 失败: " +
                 std::strerror(errno) + "; ";
        } else {
            d += role + ": pinned CPU" + std::to_string(cpu_pin) + "; ";
        }
    }

    // 读回确认：设了不等于生效。
    int policy = 0;
    struct sched_param got {};
    const int rc = ::pthread_getschedparam(::pthread_self(), &policy, &got);
    const bool ok = (rc == 0 && policy == SCHED_FIFO);
    if (ok) {
        d += role + ": SCHED_FIFO " + std::to_string(got.sched_priority) + "（读回确认）";
        TTBOX_LOG_INFO("RtSched " + d);
    } else {
        d += role + ": 读回确认失败 policy=" + std::to_string(policy);
        TTBOX_LOG_WARN("RtSched " + d);
    }
    if (detail) {
        *detail = d;
    }
    return ok;
}

}  // namespace ttbox::core

#else  // _WIN32

#include <cstdlib>
#include <string>

namespace ttbox::core {

// Windows 平台：实时调度与锁页均为空操作（恒返回 false 并在 detail 说明）。
int RtSched::env_int(const char* name, int def) {
    const char* v = std::getenv(name);
    if (!v || !*v) {
        return def;
    }
    return std::atoi(v);
}

bool RtSched::prepare_process(std::string* detail) {
    if (detail) {
        *detail = "RtSched: Windows 下空操作";
    }
    return false;
}

bool RtSched::apply_fifo(const std::string& role, int prio_default, int cpu, std::string* detail) {
    (void)prio_default;
    (void)cpu;
    if (detail) {
        *detail = "RtSched: " + role + " Windows 下空操作";
    }
    return false;
}

}  // namespace ttbox::core

#endif  // !_WIN32
