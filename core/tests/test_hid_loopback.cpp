// test_hid_loopback.cpp — A9 转发器全链路回环测试（无真实键鼠也可验证）
//
// 链路：fifo(模拟 hidraw 输入) → HidForwarder(RX→SPSC→TX) → /dev/hidg1(鼠标)
// 往 fifo 注入合成鼠标报告（1000Hz），验证：
//   rx 计数 / tx 计数 / latency / drop / 队列深度
// 说明：真实 USB 枚举（真实键鼠→hidraw）仍需物理设备；本测试验证软件转发链路完整。
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <string>
#include <sys/stat.h>
#include <thread>
#include <unistd.h>
#include <vector>

#include "hid/HidForwarder.hpp"
#include "hid/HidTypes.hpp"

using namespace ttbox::core;

namespace {

// 夹具可重入化：FIFO 路径并入 PID，避免并发实例（或多轮）共用同一 /tmp FIFO 互踩。
const std::string kFifo = "/tmp/ttbox_hid_sim_fifo_" + std::to_string(static_cast<long>(::getpid()));

bool mk_fifo(std::string* err) {
    ::unlink(kFifo.c_str());
    if (::mkfifo(kFifo.c_str(), 0666) != 0) {
        if (err) *err = "mkfifo 失败";
        return false;
    }
    return true;
}

// 注入线程：向 fifo 写合成鼠标报告（目标 rate）
void injector(double rate_hz, double duration_s, std::atomic<uint64_t>* written) {
    const int fd = ::open(kFifo.c_str(), O_WRONLY);
    if (fd < 0) return;
    const uint8_t rep[4] = {0x01, 0x01, 0x01, 0x00};  // 左键+dx1+dy1
    const auto period = std::chrono::duration<double>(1.0 / rate_hz);
    auto next = std::chrono::steady_clock::now();
    const auto t0 = std::chrono::steady_clock::now();
    while (std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count() < duration_s) {
        const ssize_t w = ::write(fd, rep, sizeof(rep));
        if (w == (ssize_t)sizeof(rep)) written->fetch_add(1);
        next = next + std::chrono::duration_cast<std::chrono::steady_clock::duration>(period);
        std::this_thread::sleep_until(next);
    }
    ::close(fd);
}

}  // namespace

int main(int argc, char** argv) {
    double rate = 1000.0;
    double duration = 3.0;
    int cpu = -1;
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        auto next = [&](const char* n) -> std::string {
            if (i + 1 >= argc) { std::fprintf(stderr, "缺少参数: %s\n", n); std::exit(1); }
            return argv[++i];
        };
        if (a == "--rate") rate = std::atof(next("--rate").c_str());
        else if (a == "--duration") duration = std::atof(next("--duration").c_str());
        else if (a == "--cpu") cpu = std::atoi(next("--cpu").c_str());
    }

    std::string err;
    if (!mk_fifo(&err)) {
        std::printf("[FAIL] %s\n", err.c_str());
        return 1;
    }

    HidForwarder fwd;
    HidForwarder::Params p;
    p.hidraw_path = kFifo;   // 模拟输入
    p.hidg_path = "/dev/hidg1";  // 鼠标输出
    p.kind = HidKind::kMouse;
    p.cpu = cpu;
    if (!fwd.start(p, &err)) {
        std::printf("[FAIL] forwarder 启动: %s（需 root + gadget 已启用）\n", err.c_str());
        ::unlink(kFifo.c_str());
        return 1;
    }

    std::atomic<uint64_t> written{0};
    std::thread t(injector, rate, duration, &written);
    t.join();
    fwd.stop();
    ::unlink(kFifo.c_str());

    const auto& s = fwd.stats();
    std::printf("=== HID 转发器回环测试 ===\n");
    std::printf("  注入=%llu rx=%llu tx=%llu backpressure=%llu drop=%llu rx_err=%llu tx_err=%llu\n",
                (unsigned long long)written.load(),
                (unsigned long long)s.rx_reports.load(),
                (unsigned long long)s.tx_reports.load(),
                (unsigned long long)s.tx_backpressure.load(),
                (unsigned long long)s.push_drops.load(),
                (unsigned long long)s.rx_errors.load(),
                (unsigned long long)s.tx_errors.load());
    if (s.latency_us.count() > 0) {
        std::printf("  latency(us): avg=%.1f p50=%llu p95=%llu p99=%llu max=%llu\n",
                    s.latency_us.avg(),
                    (unsigned long long)s.latency_us.percentile(50),
                    (unsigned long long)s.latency_us.percentile(95),
                    (unsigned long long)s.latency_us.percentile(99),
                    (unsigned long long)s.latency_us.max());
    }
    const uint64_t rx = s.rx_reports.load();
    const bool pass = (rx > 0) && (rx >= written.load() * 0.95);  // ≥95% 注入被转发
    std::printf("=== 回环测试: %s ===\n", pass ? "PASS" : "FAIL");
    return pass ? 0 : 1;
}
