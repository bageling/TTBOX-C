// CpuAffinity.cpp — RK3588 大小核亲和性 + 频率锁定实现（Linux sysfs）
#include "common/CpuAffinity.hpp"

#if !defined(_WIN32)

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <dirent.h>
#include <fstream>
#include <sched.h>
#include <sstream>
#include <unistd.h>

#include "common/Logger.hpp"

namespace ttbox::core {

namespace {

// 读 /sys 下某 policy 的整数值；失败返回 -1
long read_sys_long(const std::string& path) {
    std::ifstream f(path);
    long v = -1;
    if (f) {
        f >> v;
    }
    return v;
}

// 写 sysfs（root 权限）；返回 false 带错误说明
bool write_sys(const std::string& path, const std::string& value, std::string* error) {
    std::ofstream f(path);
    if (!f) {
        if (error) *error = "open " + path + " 失败: " + std::strerror(errno);
        return false;
    }
    f << value;
    f.flush();
    if (!f.good()) {
        if (error) *error = "write " + path + " 失败: " + std::strerror(errno);
        return false;
    }
    return true;
}

// 枚举 /sys/devices/system/cpu/cpufreq/ 下全部 policy 目录名
std::vector<std::string> list_policies() {
    std::vector<std::string> policies;
    const char* base = "/sys/devices/system/cpu/cpufreq";
    DIR* d = ::opendir(base);
    if (!d) return policies;
    while (auto* e = ::readdir(d)) {
        const std::string name = e->d_name;
        if (name.rfind("policy", 0) == 0 && name.size() > 6) {
            const bool digits = std::all_of(name.begin() + 6, name.end(), ::isdigit);
            if (digits) policies.push_back(name);
        }
    }
    ::closedir(d);
    std::sort(policies.begin(), policies.end());
    return policies;
}

}  // namespace

// 按位掩码设置指定线程（tid=0 表示当前线程）的 CPU 亲和性；mask=0 直接成功返回。
bool CpuAffinity::set_tid_affinity(uint64_t mask, unsigned tid, std::string* error) {
    if (mask == 0) return true;
    cpu_set_t set;
    CPU_ZERO(&set);
    for (int c = 0; c < 64; ++c) {
        if (mask & (1ULL << c)) CPU_SET(c, &set);
    }
    const int rc = ::sched_setaffinity(tid ? static_cast<pid_t>(tid) : 0, sizeof(set), &set);
    if (rc != 0) {
        if (error) *error = "sched_setaffinity(tid=" + std::to_string(tid) +
                            ", mask=0x" + [&] {
                                std::ostringstream os;
                                os << std::hex << mask;
                                return os.str();
                            }() + ") 失败: " + std::strerror(errno);
        return false;
    }
    return true;
}

// 设置当前线程的 CPU 亲和性（转发给 set_tid_affinity）。
bool CpuAffinity::set_thread_affinity(uint64_t mask, std::string* error) {
    return set_tid_affinity(mask, 0, error);
}

// 把每个 cpufreq policy 的 scaling_min_freq 锁到 cpuinfo_max_freq × percent%。
CpuAffinity::Result CpuAffinity::lock_min_freq_percent(int percent) {
    Result res;
    if (percent <= 0 || percent > 100) {
        res.detail = "percent=" + std::to_string(percent) + " 越界，跳过锁频";
        res.freq_ok = true;  // 未配置 = 不锁，视为成功（不阻塞启动）
        return res;
    }

    bool all_ok = true;
    std::ostringstream log;
    for (const auto& pol : list_policies()) {
        const std::string base = "/sys/devices/system/cpu/cpufreq/" + pol;
        const long max_freq = read_sys_long(base + "/cpuinfo_max_freq");
        if (max_freq <= 0) {
            log << pol << ": 读 cpuinfo_max_freq 失败; ";
            all_ok = false;
            continue;
        }
        // 目标下限 = max × percent，向下取整到 kHz
        const long target = max_freq * static_cast<long>(percent) / 100L;
        std::string werr;
        if (!write_sys(base + "/scaling_min_freq", std::to_string(target), &werr)) {
            log << pol << ": 锁 " << target / 1000 << "MHz 失败(" << werr << "); ";
            all_ok = false;
        } else {
            log << pol << ": min=" << target / 1000 << "MHz"
                << "/" << max_freq / 1000 << "MHz(" << percent << "%); ";
        }
    }
    res.freq_ok = all_ok;
    res.detail = log.str();
    return res;
}

}  // namespace ttbox::core

#else  // _WIN32

namespace ttbox::core {
// Windows 平台：亲和性与锁频均为空操作（恒成功 / 返回默认结果）。
bool CpuAffinity::set_tid_affinity(uint64_t, unsigned, std::string*) { return true; }
bool CpuAffinity::set_thread_affinity(uint64_t, std::string*) { return true; }
CpuAffinity::Result CpuAffinity::lock_min_freq_percent(int) { return {}; }
}  // namespace ttbox::core

#endif  // !_WIN32
