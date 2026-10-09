// test_hid_forward_hw.cpp — A9 硬件测试：hidraw → hidg 透传 + 回报率/latency
//
// 验证：
//   1. USB HID Gadget（/dev/hidg0 键盘 /dev/hidg1 鼠标）存在且可写
//   2. 真实键鼠插入时（/dev/hidraw*）：HidForwarder 透传 + 统计回报率/latency/drop
//   3. 无真实键鼠：明确报告 NOT AVAILABLE（不伪造）
//
// 用法：test_hid_forward_hw [seconds] [--cpu N]
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dirent.h>
#include <fcntl.h>
#include <linux/hidraw.h>
#include <string>
#include <sys/ioctl.h>
#include <thread>
#include <unistd.h>
#include <vector>

#include "hid/HidForwarder.hpp"
#include "hid/HidTypes.hpp"

using namespace ttbox::core;

namespace {

std::vector<std::string> list_hidraw() {
    std::vector<std::string> out;
    DIR* d = ::opendir("/dev");
    if (!d) return out;
    struct dirent* e;
    while ((e = ::readdir(d)) != nullptr) {
        if (std::strncmp(e->d_name, "hidraw", 6) == 0) {
            out.push_back("/dev/" + std::string(e->d_name));
        }
    }
    ::closedir(d);
    return out;
}

bool exists(const std::string& p) { return ::access(p.c_str(), F_OK) == 0; }

// 读 HID report descriptor 判断键盘/鼠标（键盘含 UsagePage=Keyboard 0x07）
HidKind kind_from_report_desc(const std::string& path) {
    const int fd = ::open(path.c_str(), O_RDONLY | O_NONBLOCK);
    if (fd < 0) return HidKind::kUnknown;
    char buf[4096];
    const int n = ::read(fd, buf, sizeof(buf));
    ::close(fd);
    if (n <= 0) return HidKind::kUnknown;
    // 简单启发：扫描 descriptor，Keyboard UsagePage (0x05 0x07) → keyboard；
    // Mouse UsagePage (0x09) 且无 0x07 → mouse
    bool has_kb = false, has_ms = false;
    for (int i = 0; i + 1 < n; ++i) {
        if ((unsigned char)buf[i] == 0x05) {
            if ((unsigned char)buf[i + 1] == 0x07) has_kb = true;
            if ((unsigned char)buf[i + 1] == 0x09) has_ms = true;
        }
    }
    if (has_kb) return HidKind::kKeyboard;
    if (has_ms) return HidKind::kMouse;
    return HidKind::kUnknown;
}

void print_stats(const char* tag, const HidForwardStats& s) {
    std::printf("  [%s] rx=%llu tx=%llu push_drop=%llu rx_err=%llu tx_err=%llu bad=%llu\n",
                tag, (unsigned long long)s.rx_reports.load(),
                (unsigned long long)s.tx_reports.load(),
                (unsigned long long)s.push_drops.load(),
                (unsigned long long)s.rx_errors.load(),
                (unsigned long long)s.tx_errors.load(),
                (unsigned long long)s.bad_size.load());
    if (s.latency_us.count() > 0) {
        std::printf("  [%s] latency(us) N=%zu min=%llu avg=%.1f p50=%llu p95=%llu p99=%llu max=%llu\n",
                    tag, s.latency_us.count(),
                    (unsigned long long)s.latency_us.min(), s.latency_us.avg(),
                    (unsigned long long)s.latency_us.percentile(50),
                    (unsigned long long)s.latency_us.percentile(95),
                    (unsigned long long)s.latency_us.percentile(99),
                    (unsigned long long)s.latency_us.max());
    }
    if (s.rx_interval_us.count() > 1) {
        const double avg_us = s.rx_interval_us.avg();
        std::printf("  [%s] rx interval avg=%.1f us (≈%.1f Hz), queue_max=%llu\n",
                    tag, avg_us, avg_us > 0 ? 1e6 / avg_us : 0.0,
                    (unsigned long long)s.max_queue_depth.load());
    }
}

}  // namespace

int main(int argc, char** argv) {
    double duration_s = 3.0;
    int cpu = -1;
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "--cpu" && i + 1 < argc) cpu = std::atoi(argv[++i]);
        else duration_s = std::atof(a.c_str());
    }
    std::printf("=== A9 HID 透传硬件测试（%.0fs, cpu=%d）===\n", duration_s, cpu);

    // ---- 1. HID Gadget 检查 ----
    const bool has_hidg0 = exists("/dev/hidg0");
    const bool has_hidg1 = exists("/dev/hidg1");
    std::printf("[gadget] hidg0(键盘)=%s hidg1(鼠标)=%s\n",
                has_hidg0 ? "OK" : "MISSING", has_hidg1 ? "OK" : "MISSING");
    if (!has_hidg0 || !has_hidg1) {
        std::printf("[FAIL] HID Gadget 未启用（先运行 a9_setup_hid_gadget.sh enable）\n");
        return 1;
    }
    // gadget 可写检查（无主机枚举时 write 会 ENODEV；只验证 open+nonblock）
    int g = ::open("/dev/hidg0", O_WRONLY | O_NONBLOCK);
    if (g < 0) {
        std::printf("[WARN] hidg0 open 失败（需 root）: %s\n", std::strerror(errno));
    } else {
        ::close(g);
        std::printf("[gadget] hidg0 open OK\n");
    }

    // ---- 2. 真实键鼠检测 ----
    const auto hidraws = list_hidraw();
    std::printf("[input] hidraw 设备数=%zu\n", hidraws.size());
    if (hidraws.empty()) {
        std::printf("[NOT AVAILABLE] 未检测到 USB HID 输入设备（请在板子 USB Host 口插入真实键鼠后重测）\n");
        std::printf("=== test_hid_forward_hw: INPUT NOT AVAILABLE（Gadget 部分 OK）===\n");
        return 0;
    }

    // ---- 3. 启动 Forwarder 短跑 ----
    int fail = 0;
    for (const auto& h : hidraws) {
        const HidKind kind = kind_from_report_desc(h);
        const std::string hidg = (kind == HidKind::kKeyboard) ? "/dev/hidg0"
                                 : (kind == HidKind::kMouse) ? "/dev/hidg1" : "";
        if (hidg.empty()) {
            std::printf("  [skip] %s 类型未知（非键盘/鼠标）\n", h.c_str());
            continue;
        }
        std::printf("  === %s -> %s (%s) ===\n",
                    h.c_str(), hidg.c_str(),
                    kind == HidKind::kKeyboard ? "keyboard" : "mouse");

        HidForwarder fwd;
        HidForwarder::Params p;
        p.hidraw_path = h;
        p.hidg_path = hidg;
        p.kind = kind;
        p.cpu = cpu;
        std::string err;
        if (!fwd.start(p, &err)) {
            std::printf("  [FAIL] %s\n", err.c_str());
            fail = 1;
            continue;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(
            static_cast<int>(duration_s * 1000)));
        fwd.stop();
        print_stats(kind == HidKind::kKeyboard ? "kb" : "ms", fwd.stats());
        if (fwd.stats().rx_reports.load() == 0) {
            std::printf("  [NOT AVAILABLE] %s 无报告（设备静默？）\n", h.c_str());
        }
    }
    std::printf("=== test_hid_forward_hw %s ===\n", fail ? "FAIL" : "PASS");
    return fail;
}
