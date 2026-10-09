// ttbox_hid_forward.cpp — 常驻 HID 透传转发 daemon
//
// 职责：启动 HidRuntime（读取 hid_config.json，hidraw → hidg 转发），
//       常驻运行，支持 SIGINT/SIGTERM 优雅退出；定时刷新 metrics 日志。
//
// 用法：
//   ttbox-hid-forward [--root <hid_root>] [--log-interval S]
//
// 配套 systemd: ttbox-hid-forward.service (Restart=always)
#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <thread>

#include "hid/HidRuntime.hpp"

using namespace ttbox::core;

namespace {

std::atomic<bool> g_shutdown{false};

void signal_handler(int) { g_shutdown.store(true); }

}  // namespace

int main(int argc, char** argv) {
    std::string root;
    double log_interval = 10.0;
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "--root" && i + 1 < argc) root = argv[++i];
        else if (a == "--log-interval" && i + 1 < argc) log_interval = std::atof(argv[++i]);
        else { std::fprintf(stderr, "未知参数: %s\n", a.c_str()); return 1; }
    }

    std::signal(SIGINT, signal_handler);
    std::signal(SIGTERM, signal_handler);

    HidRuntime rt;
    if (!root.empty()) rt.set_root(root);

    std::string err;
    if (!rt.start(&err)) {
        std::fprintf(stderr, "HID runtime 启动失败: %s\n", err.c_str());
        return 1;
    }
    std::printf("ttbox-hid-forward running (root=%s)\n", root.empty() ? "<default>" : root.c_str());

    const auto interval = std::chrono::duration<double>(log_interval);
    while (!g_shutdown.load()) {
        std::this_thread::sleep_for(interval);
        const auto m = rt.get_metrics();
        std::printf("[metrics] rx=%llu tx=%llu drop=%llu backpressure=%llu "
                    "latency_avg=%.1fus p50=%llu rate=%.1fHz queue_max=%llu status=%s\n",
                    (unsigned long long)m.rx_reports, (unsigned long long)m.tx_reports,
                    (unsigned long long)m.drop, (unsigned long long)m.backpressure,
                    m.latency_avg_us, (unsigned long long)m.latency_p50_us,
                    m.report_rate_hz, (unsigned long long)m.max_queue_depth,
                    hid_runtime_status_name(m.status));
        std::fflush(stdout);
    }

    rt.stop();
    std::printf("ttbox-hid-forward stopped\n");
    return 0;
}
