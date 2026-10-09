// test_latestframe.cpp — LatestFrame 纯逻辑测试（无硬件）
#include <atomic>
#include <chrono>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "capture/V4L2Capture.hpp"
#include "test_util.hpp"

using namespace ttbox::core;

namespace {

std::shared_ptr<FrameBuffer> make_frame(uint32_t seq) {
    auto f = std::make_shared<FrameBuffer>();
    f->info.sequence = seq;
    f->info.dma_fd = static_cast<int>(seq);  // 任意非负值占位
    return f;
}

}  // namespace

TEST(latestframe_publish_overwrites_old) {
    LatestFrame lf;
    auto f1 = make_frame(1);
    auto f2 = make_frame(2);

    auto old1 = lf.publish(f1);
    CHECK(old1 == nullptr);  // 首次发布无旧帧

    auto old2 = lf.publish(f2);
    CHECK(old2 != nullptr);
    CHECK(old2->info.sequence == 1);  // 旧帧被替换返回

    auto cur = lf.get();
    CHECK(cur != nullptr);
    CHECK(cur->info.sequence == 2);  // 永远拿最新帧
}

TEST(latestframe_get_keeps_frame_alive) {
    LatestFrame lf;
    lf.publish(make_frame(10));

    auto held = lf.get();  // 消费者持有
    lf.clear();            // 清空容器
    CHECK(held != nullptr);
    CHECK(held->info.sequence == 10);  // 引用仍保活
}

TEST(latestframe_multithread_no_crash) {
    LatestFrame lf;
    std::atomic<bool> stop{false};

    // 生产者
    std::thread producer([&] {
        uint32_t seq = 0;
        while (!stop.load()) {
            lf.publish(make_frame(++seq));
            std::this_thread::yield();
        }
    });
    // 消费者
    std::thread consumer([&] {
        uint64_t got = 0;
        while (!stop.load()) {
            auto f = lf.get();
            if (f) {
                ++got;
                if (f->info.sequence > 1000000) break;  // 不可能
            }
            std::this_thread::yield();
        }
    });

    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    stop.store(true);
    producer.join();
    consumer.join();

    auto cur = lf.get();
    CHECK(cur != nullptr);
    CHECK(cur->info.sequence >= 1);  // 多线程下 latest 仍有效
}
