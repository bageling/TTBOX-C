// test_hid_load_sim.cpp — A9 合成 HID 高频负载模拟（CPU affinity / AI 隔离实验）
//
// 目的：无真实键鼠时，用合成高频 HID Report 负载测量：
//   - 指定目标 report rate（如 1000/4000/8000 Hz）下 RX/TX 的实际吞吐
//   - 绑定不同 CPU 时的 CPU 开销
//   - 与 AI Pipeline 并发时对 AI FPS 的影响（配合 test_worker_hw 一起跑）
//
// 链路：producer(目标 rate, 绑定 cpu) → SPSC 队列 → consumer(丢弃, 绑定 cpu)
// 输出：实际 rate / 队列最大深度 / drop / 运行时间
//
// 用法：test_hid_load_sim --rate 1000 [--cpu RX --cpu-tx TX --duration S --verbose]
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <pthread.h>
#include <sched.h>
#include <string>
#include <thread>

#include "hid/HidTypes.hpp"
#include "hid/SpscQueue.hpp"

using namespace ttbox::core;

namespace {

bool bind_cpu(std::thread& t, int cpu) {
    if (cpu < 0) return true;
    cpu_set_t cs;
    CPU_ZERO(&cs);
    CPU_SET(cpu, &cs);
    return pthread_setaffinity_np(t.native_handle(), sizeof(cs), &cs) == 0;
}

// 按目标速率生成 HidReport（恒定间隔，模拟高频回报率）
void producer(SpscQueue<HidReport, 1024>* q, double rate_hz,
              std::atomic<bool>* run, std::atomic<uint64_t>* made,
              std::atomic<uint64_t>* drop, uint32_t seq_base) {
    HidReport rep;
    rep.size = 4;
    rep.data[0] = 0;  // buttons
    rep.data[1] = 1;  // dx
    rep.data[2] = 1;  // dy
    rep.data[3] = 0;  // wheel
    const auto period = std::chrono::duration<double>(1.0 / rate_hz);
    auto next = std::chrono::steady_clock::now();
    uint32_t seq = seq_base;
    while (run->load()) {
        rep.timestamp_us =
            static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(
                                      std::chrono::steady_clock::now().time_since_epoch())
                                      .count());
        rep.seq = seq++;
        if (!q->try_push(rep)) drop->fetch_add(1);
        made->fetch_add(1);
        next = next + std::chrono::duration_cast<std::chrono::steady_clock::duration>(period);
        std::this_thread::sleep_until(next);
    }
}

}  // namespace

int main(int argc, char** argv) {
    double rate = 1000.0;
    int cpu_rx = -1, cpu_tx = -1;
    double duration = 5.0;
    bool verbose = false;
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        auto next = [&](const char* n) -> std::string {
            if (i + 1 >= argc) { std::fprintf(stderr, "缺少参数: %s\n", n); std::exit(1); }
            return argv[++i];
        };
        if (a == "--rate") rate = std::atof(next("--rate").c_str());
        else if (a == "--cpu") cpu_rx = std::atoi(next("--cpu").c_str());
        else if (a == "--cpu-tx") cpu_tx = std::atoi(next("--cpu-tx").c_str());
        else if (a == "--duration") duration = std::atof(next("--duration").c_str());
        else if (a == "--verbose") verbose = true;
        else { std::fprintf(stderr, "未知参数: %s\n", a.c_str()); return 1; }
    }
    if (cpu_tx < 0) cpu_tx = cpu_rx;

    SpscQueue<HidReport, 1024> q;
    std::atomic<bool> run{true};
    std::atomic<uint64_t> made{0}, drop{0};

    std::thread t1(producer, &q, rate, &run, &made, &drop, 0);
    std::thread t2([&] {
        HidReport r;
        uint64_t n = 0;
        while (run.load() || !q.empty()) {
            if (q.try_pop(&r)) ++n;
            else std::this_thread::yield();
        }
        std::printf("[consumer] 消费 %llu reports\n", (unsigned long long)n);
    });
    bind_cpu(t1, cpu_rx);
    bind_cpu(t2, cpu_tx);

    const auto t0 = std::chrono::steady_clock::now();
    size_t qmax = 0;
    while (std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count() < duration) {
        const size_t s = q.size();
        if (s > qmax) qmax = s;
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    run = false;
    t1.join();
    t2.join();
    const double dt = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();

    const uint64_t m = made.load();
    const double actual = m / dt;
    std::printf("=== HID 负载模拟 ===\n");
    std::printf("  目标 rate=%.0f Hz 实际=%.1f Hz (%.1f%%)  reports=%llu drop=%llu queue_max=%zu\n",
                rate, actual, 100.0 * actual / rate, (unsigned long long)m,
                (unsigned long long)drop.load(), qmax);
    std::printf("  rx_cpu=%d tx_cpu=%d duration=%.1fs\n", cpu_rx, cpu_tx, dt);
    if (verbose) std::printf("  (HID 模拟负载运行完成)\n");
    return 0;
}
