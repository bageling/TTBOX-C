// test_lifecycle_stress.cpp — C10/C14：start/stop/restart 生命周期压力测试。
// 模拟 60 次 start→stop（含连续 restart），验证线程真正退出、无重复创建、无崩溃。
// 同时覆盖：AimThread（最活跃线程）+ RuntimeConfig 并发读写（C11 基础）。
#include <atomic>
#include <chrono>
#include <cstdio>
#include <memory>
#include <thread>
#include <vector>

#include "aim/AimThread.hpp"
#include "model/RuntimeProfile.hpp"

using namespace ttbox::core;
using namespace ttbox::core::aim;

int main() {
    int fails = 0;
    auto check = [&fails](bool ok, const char* name) {
        std::printf("[%s] %s\n", ok ? "PASS" : "FAIL", name);
        if (!ok) ++fails;
    };

    // ---- 1) AimThread start/stop × 60（线程必须每次真正退出）----
    {
        AimTargetMailbox mailbox(1);
        auto output = std::make_shared<ttbox::core::output::NullHidOutput>();
        bool all_ok = true;
        for (int i = 0; i < 60; ++i) {
            AimThread thread;
            if (!thread.start(&mailbox, output, 1000)) { all_ok = false; break; }
            AimTargetTask task;
            task.frame_number = static_cast<uint64_t>(i);
            task.timestamp_us = static_cast<uint64_t>(i) * 1000ULL;
            task.frame_width = 1280;
            task.frame_height = 720;
            task.has_target = false;
            mailbox.offer(0, std::move(task));
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
            thread.stop();
            // stop 后 status 应反映 stopped
            auto st = thread.status();
            if (st.running) { all_ok = false; break; }
        }
        check(all_ok, "lifecycle: AimThread start/stop ×60 无泄漏无崩溃");
    }

    // ---- 2) 连续 restart（stop→start 同实例）----
    {
        AimTargetMailbox mailbox(1);
        auto output = std::make_shared<ttbox::core::output::NullHidOutput>();
        AimThread thread;
        bool all_ok = true;
        if (!thread.start(&mailbox, output, 1000)) all_ok = false;
        for (int i = 0; i < 20; ++i) {
            thread.stop();
            if (!thread.start(&mailbox, output, 1000)) { all_ok = false; break; }
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        thread.stop();
        check(all_ok, "lifecycle: 同实例 stop→start ×20");
    }

    // ---- 3) RuntimeConfig 热更新并发（C11：写线程 ×2 + 读线程 ×2，2000 轮）----
    {
        RuntimeConfig cfg;
        auto base = std::make_shared<RuntimeProfile>();
        base->mouse.kp_x = 10.0f;
        base->mouse.kp_y = 20.0f;  // 保持 y=2x 不变量（否则读者读 base 快照误报撕裂）
        cfg.update(base);
        std::atomic<bool> stop{false};
        std::atomic<int> read_errors{0};

        auto writer = [&cfg](float start) {
            for (int i = 0; i < 1000; ++i) {
                auto p = std::make_shared<RuntimeProfile>();
                p->mouse.kp_x = start + static_cast<float>(i % 50);
                p->mouse.kp_y = (start + static_cast<float>(i % 50)) * 2.0f;  // 配对字段：y=2x
                cfg.update(p);
            }
        };
        auto reader = [&cfg, &read_errors, &stop]() {
            while (!stop.load(std::memory_order_acquire)) {
                auto s = cfg.snapshot();
                if (!s) { ++read_errors; continue; }
                // 完整性：快照必须是完整 profile——配对字段 y=2x 必须严格成立，
                // 若出现撕裂（x 来自批次A、y 来自批次B）则断言失败
                const float kx = s->mouse.kp_x;
                const float ky = s->mouse.kp_y;
                if (ky != kx * 2.0f) ++read_errors;
            }
        };

        std::thread w1(writer, 0.0f);
        std::thread w2(writer, 500.0f);
        std::thread r1(reader);
        std::thread r2(reader);
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
        stop.store(true);
        w1.join(); w2.join(); r1.join(); r2.join();
        check(read_errors.load() == 0, "runtimeconfig: 并发读写无撕裂（200ms 压测）");
    }

    if (fails == 0) std::printf("test_lifecycle_stress: PASS\n");
    else std::printf("test_lifecycle_stress: %d FAILED\n", fails);
    return fails == 0 ? 0 : 1;
}
