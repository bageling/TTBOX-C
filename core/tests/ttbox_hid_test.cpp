// ttbox_hid_test.cpp — A9 独立测试程序：HID 设备信息 + 回报率 + 延迟 + 透传
//
// 功能：
//   - 枚举 /dev/hidraw*：显示 device / VID/PID / HID descriptor 摘要 / report size / 类型
//   - 测量 report rate、latency、drop、queue depth
//   - 键盘/鼠标事件解析（HidParser）
//   - 可选 --forward 启用 hidg 透传（HidForwarder）
//
// 用法：
//   ttbox-hid-test [--mouse] [--keyboard] [--rate] [--duration S] [--verbose] [--cpu N] [--forward]
// 高频测试默认不打印每个 Report（--verbose 才打印，仅建议低速短测）。
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dirent.h>
#include <fcntl.h>
#include <fstream>
#include <linux/hidraw.h>
#include <poll.h>
#include <string>
#include <sys/ioctl.h>
#include <thread>
#include <unistd.h>
#include <vector>

#include "hid/HidForwarder.hpp"
#include "hid/HidParser.hpp"
#include "hid/HidTypes.hpp"

using namespace ttbox::core;

namespace {

struct HidDeviceInfo {
    std::string path;
    uint16_t vid = 0;
    uint16_t pid = 0;
    HidKind kind = HidKind::kUnknown;
    int report_size = 0;
    std::vector<uint8_t> desc;  // report descriptor
};

std::vector<HidDeviceInfo> enumerate_hidraw() {
    std::vector<HidDeviceInfo> out;
    DIR* d = ::opendir("/dev");
    if (!d) return out;
    struct dirent* e;
    while ((e = ::readdir(d)) != nullptr) {
        if (std::strncmp(e->d_name, "hidraw", 6) != 0) continue;
        const std::string path = "/dev/" + std::string(e->d_name);
        const int fd = ::open(path.c_str(), O_RDONLY | O_NONBLOCK);
        if (fd < 0) continue;
        HidDeviceInfo info;
        info.path = path;
        struct hidraw_devinfo dev;
        std::memset(&dev, 0, sizeof(dev));
        if (::ioctl(fd, HIDIOCGRAWINFO, &dev) == 0) {
            info.vid = dev.vendor;
            info.pid = dev.product;
        }
        // report descriptor（HIDIOCGRDESC）
        struct hidraw_report_descriptor rd;
        std::memset(&rd, 0, sizeof(rd));
        rd.size = sizeof(rd.value);
        if (::ioctl(fd, HIDIOCGRDESC, &rd) == 0) {
            info.desc.assign(rd.value, rd.value + rd.size);
        }
        // 类型判断：优先扫描 descriptor 的 UsagePage（0x05 0x07=Keyboard 0x09=Mouse），
// 若 HIDIOCGRDESC 失败（如罗技复合设备），回退读 sysfs input 名称
        bool kb = false, ms = false;
        for (size_t i = 0; i + 1 < info.desc.size(); ++i) {
            if (info.desc[i] == 0x05) {
                if (info.desc[i + 1] == 0x07) kb = true;
                if (info.desc[i + 1] == 0x09) ms = true;
            }
        }
        if (!kb && !ms) {
            // sysfs 回退：/sys/class/hidraw/<name>/device/input/input*/name
            const std::string base = "/sys/class/hidraw/" + std::string(e->d_name) + "/device/input";
            DIR* id = ::opendir(base.c_str());
            if (id) {
                struct dirent* ie;
                while ((ie = ::readdir(id)) != nullptr) {
                    if (std::strncmp(ie->d_name, "input", 5) != 0) continue;
                    std::ifstream nf(base + "/" + ie->d_name + "/name");
                    std::string nm;
                    std::getline(nf, nm);
                    const auto has = [&nm](const char* key) {
                        return nm.find(key) != std::string::npos;
                    };
                    if (has("Mouse")) ms = true;
                    if (has("Keyboard")) kb = true;
                }
                ::closedir(id);
            }
            // HID_PHYS 兜底（罗技 Receiver 键盘名不含 Keyboard）：
            //   HID_PHYS 以 "/input0" 结尾 → 键盘接口（USB HID 惯例）
            if (!kb && !ms) {
                std::ifstream uf("/sys/class/hidraw/" + std::string(e->d_name) + "/device/uevent");
                std::string line;
                while (std::getline(uf, line)) {
                    if (line.rfind("HID_PHYS=", 0) == 0) {
                        const std::string phys = line.substr(9);
                        if (phys.size() >= 7 && phys.compare(phys.size() - 7, 7, "/input0") == 0) {
                            kb = true;
                        }
                    }
                }
            }
        }
        info.kind = kb ? HidKind::kKeyboard : (ms ? HidKind::kMouse : HidKind::kUnknown);
        // report size：取 report descriptor 中最大 INPUT 报告（简化：用 read 试探一次）
        char buf[256];
        const ssize_t n = ::read(fd, buf, sizeof(buf));
        if (n > 0) info.report_size = static_cast<int>(n);
        ::close(fd);
        out.push_back(std::move(info));
    }
    ::closedir(d);
    return out;
}

const char* kind_str(HidKind k) {
    switch (k) {
        case HidKind::kKeyboard: return "keyboard";
        case HidKind::kMouse: return "mouse";
        default: return "unknown";
    }
}

void dump_device(const HidDeviceInfo& d) {
    std::printf("  %s  VID=%04x PID=%04x  type=%s  report_size=%d  desc=%zuB\n",
                d.path.c_str(), d.vid, d.pid, kind_str(d.kind), d.report_size, d.desc.size());
    // descriptor 摘要（前 32B hex）
    std::printf("    desc[0..31]: ");
    for (size_t i = 0; i < d.desc.size() && i < 32; ++i) std::printf("%02x ", d.desc[i]);
    std::printf("\n");
}

}  // namespace

int main(int argc, char** argv) {
    bool want_mouse = false, want_kb = false, want_rate = false, verbose = false, forward = false;
    double duration = 3.0;
    int cpu = -1;
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        auto next = [&](const char* n) -> std::string {
            if (i + 1 >= argc) { std::fprintf(stderr, "缺少参数: %s\n", n); std::exit(1); }
            return argv[++i];
        };
        if (a == "--mouse") want_mouse = true;
        else if (a == "--keyboard") want_kb = true;
        else if (a == "--rate") want_rate = true;
        else if (a == "--verbose") verbose = true;
        else if (a == "--forward") forward = true;
        else if (a == "--duration") duration = std::atof(next("--duration").c_str());
        else if (a == "--cpu") cpu = std::atoi(next("--cpu").c_str());
        else { std::fprintf(stderr, "未知参数: %s\n", a.c_str()); return 1; }
    }

    std::printf("=== ttbox-hid-test (%.0fs%s%s) ===\n", duration,
                forward ? ", forward=on" : "",
                verbose ? ", verbose" : "");
    const auto devs = enumerate_hidraw();
    if (devs.empty()) {
        std::printf("[NOT AVAILABLE] 无 /dev/hidraw* 设备（请在 USB Host 口插入真实键鼠）\n");
        return 0;
    }

    // ---- 1. 枚举显示 ----
    std::printf("== HID 设备枚举 ==\n");
    int measured = 0;
    for (const auto& d : devs) {
        if (want_mouse && d.kind != HidKind::kMouse) continue;
        if (want_kb && d.kind != HidKind::kKeyboard) continue;
        dump_device(d);
        if (d.kind == HidKind::kMouse || d.kind == HidKind::kKeyboard) ++measured;
    }
    if (measured == 0) {
        std::printf("[NOT AVAILABLE] 指定类型无设备\n");
        return 0;
    }

    // ---- 2. 转发/测量 ----
    std::printf("== 测量（%s）==\n", forward ? "forward+measure" : "measure only");
    for (const auto& d : devs) {
        if (d.kind != HidKind::kMouse && d.kind != HidKind::kKeyboard) continue;
        if (want_mouse && d.kind != HidKind::kMouse) continue;
        if (want_kb && d.kind != HidKind::kKeyboard) continue;
        const std::string hidg = (d.kind == HidKind::kKeyboard) ? "/dev/hidg0" : "/dev/hidg1";

        HidForwarder fwd;
        HidForwarder::Params p;
        p.hidraw_path = d.path;
        p.hidg_path = hidg;   // forward=off 时 start 仍要求可打开；改为 forward 才转发
        p.kind = d.kind;
        p.cpu = cpu;
        p.raw_pass = false;   // 重编码为 gadget 固定格式（4B 鼠标 / 8B 键盘）+ 过滤非鼠标报告
        if (forward) {
            std::string err;
            if (!fwd.start(p, &err)) {
                std::printf("  [FAIL] %s: %s\n", d.path.c_str(), err.c_str());
                continue;
            }
        } else {
            // 仅测量：直接读 hidraw 并计时（不转发）
            const int fd = ::open(d.path.c_str(), O_RDONLY | O_NONBLOCK);
            if (fd < 0) continue;
            struct pollfd pf {fd, POLLIN, 0};
            const auto t0 = std::chrono::steady_clock::now();
            unsigned long long n_rep = 0;
            long long sum_us = 0;
            long long last_us = 0;
            long long min_int = 0, max_int = 0;
            std::vector<long long> intervals;
            char buf[256];
            while (true) {
                const auto now = std::chrono::steady_clock::now();
                if (std::chrono::duration<double>(now - t0).count() >= duration) break;
                const int pr = ::poll(&pf, 1, 100);
                if (pr <= 0) continue;
                const ssize_t n = ::read(fd, buf, sizeof(buf));
                if (n <= 0) continue;
                const long long now_us = std::chrono::duration_cast<std::chrono::microseconds>(
                                             std::chrono::steady_clock::now().time_since_epoch())
                                             .count();
                if (last_us != 0) {
                    const long long dt = now_us - last_us;
                    if (min_int == 0 || dt < min_int) min_int = dt;
                    if (dt > max_int) max_int = dt;
                    intervals.push_back(dt);
                    sum_us += dt;
                }
                last_us = now_us;
                ++n_rep;
                if (verbose && n_rep <= 50) {
                    std::printf("    [%s] rep#%llu len=%zd: ", d.path.c_str(),
                                (unsigned long long)n_rep, n);
                    for (ssize_t k = 0; k < n; ++k) std::printf("%02x ", (unsigned char)buf[k]);
                    std::printf("\n");
                }
            }
            ::close(fd);
            if (n_rep == 0) {
                std::printf("  %s [%s]: 无报告（设备静默？）\n", d.path.c_str(), kind_str(d.kind));
                continue;
            }
            const double avg_us = n_rep > 1 ? static_cast<double>(sum_us) / (n_rep - 1) : 0.0;
            std::sort(intervals.begin(), intervals.end());
            const size_t mid = intervals.size() / 2;
            const long long p50 = intervals.empty() ? 0 : intervals[mid];
            std::printf("  %s [%s]: reports=%llu rate=%.1f Hz  interval(us) avg=%.1f p50=%lld min=%lld max=%lld\n",
                        d.path.c_str(), kind_str(d.kind), (unsigned long long)n_rep,
                        n_rep / duration, avg_us, p50, min_int, max_int);
            continue;
        }
        // forward 模式统计
        std::this_thread::sleep_for(std::chrono::milliseconds(
            static_cast<int>(duration * 1000)));
        fwd.stop();
        const auto& s = fwd.stats();
        std::printf("  %s [%s] -> %s: rx=%llu tx=%llu drop=%llu\n",
                    d.path.c_str(), kind_str(d.kind), hidg.c_str(),
                    (unsigned long long)s.rx_reports.load(),
                    (unsigned long long)s.tx_reports.load(),
                    (unsigned long long)s.push_drops.load());
        if (s.latency_us.count() > 0) {
            std::printf("    latency(us) avg=%.1f p50=%llu p95=%llu p99=%llu max=%llu\n",
                        s.latency_us.avg(),
                        (unsigned long long)s.latency_us.percentile(50),
                        (unsigned long long)s.latency_us.percentile(95),
                        (unsigned long long)s.latency_us.percentile(99),
                        (unsigned long long)s.latency_us.max());
        }
        if (s.rx_interval_us.count() > 1) {
            const double a = s.rx_interval_us.avg();
            std::printf("    rate=%.1f Hz (interval avg=%.1f us) queue_max=%llu\n",
                        a > 0 ? 1e6 / a : 0.0, a,
                        (unsigned long long)s.max_queue_depth.load());
        }
    }
    std::printf("=== ttbox-hid-test done ===\n");
    return 0;
}
