// test_hid.cpp — A9 单元测试：SPSC 队列 / HID 解析 / 坐标转换
#include "test_util.hpp"

#include <thread>
#include <vector>

#include "hid/HidForwarder.hpp"
#include "hid/HidParser.hpp"
#include "hid/HidTypes.hpp"
#include "hid/SpscQueue.hpp"

using namespace ttbox::core;

TEST(spsc_queue_single_producer_consumer) {
    SpscQueue<int, 16> q;
    CHECK(q.empty());
    CHECK(q.try_push(1));
    CHECK(q.try_push(2));
    CHECK_EQ(q.size(), 2u);
    int v = 0;
    CHECK(q.try_pop(&v));
    CHECK_EQ(v, 1);
    CHECK(q.try_pop(&v));
    CHECK_EQ(v, 2);
    CHECK(q.empty());
    CHECK(!q.try_pop(&v));  // 空
}

TEST(spsc_queue_wraparound_full) {
    SpscQueue<int, 4> q;
    for (int i = 0; i < 4; ++i) CHECK(q.try_push(i));
    CHECK(!q.try_push(99));  // 满
    // 全部弹出后复用（wraparound）
    int v = 0;
    for (int i = 0; i < 4; ++i) {
        CHECK(q.try_pop(&v));
        CHECK_EQ(v, i);
    }
    CHECK(q.try_push(42));
    CHECK(q.try_pop(&v));
    CHECK_EQ(v, 42);
}

TEST(spsc_queue_multithread) {
    SpscQueue<int, 1024> q;
    std::atomic<bool> done{false};
    std::atomic<long long> sum{0};
    std::atomic<int> got{0};

    std::thread producer([&] {
        for (int i = 0; i < 100000; ++i) {
            while (!q.try_push(i)) std::this_thread::yield();
        }
        done = true;
    });
    std::thread consumer([&] {
        int v = 0;
        while (!done || !q.empty()) {
            if (q.try_pop(&v)) {
                sum += v;
                got.fetch_add(1);
            } else {
                std::this_thread::yield();
            }
        }
    });
    producer.join();
    consumer.join();
    CHECK_EQ(got.load(), 100000);
    CHECK_EQ(sum.load(), 100000LL * 99999LL / 2LL);  // 0..99999 和
}

TEST(hid_parse_mouse) {
    HidReport rep;
    rep.size = 4;
    rep.timestamp_us = 123456;
    rep.data[0] = 0x05;  // 左+中
    rep.data[1] = 0xFE;  // dx = -2
    rep.data[2] = 0x0A;  // dy = 10
    rep.data[3] = 0x01;  // wheel = 1
    MouseState m = parse_mouse_report(rep);
    CHECK_EQ(m.buttons, 0x05u);
    CHECK_EQ(static_cast<int>(m.dx), -2);
    CHECK_EQ(static_cast<int>(m.dy), 10);
    CHECK_EQ(static_cast<int>(m.wheel), 1);
    CHECK_EQ(m.timestamp_us, 123456u);
}

TEST(hid_parse_keyboard) {
    HidReport rep;
    rep.size = 8;
    rep.data[0] = 0x02;  // LShift
    rep.data[2] = 0x04;  // key 'a'
    rep.data[3] = 0x16;  // key 's'
    KeyboardState k = parse_keyboard_report(rep);
    CHECK_EQ(k.modifiers, 0x02u);
    CHECK_EQ(k.keys[0], 0x04u);
    CHECK_EQ(k.keys[1], 0x16u);
    CHECK_EQ(k.keys[2], 0x00u);
}

TEST(coord_screen_to_detection) {
    // Detection 坐标系 = 原图（屏幕）坐标系：screen → detection 恒等
    float ox = 0, oy = 0;
    CHECK(CoordinateTransform::screen_to_detection(800.0f, 600.0f, 100, 50, 640, 360,
                                                   640, 640, &ox, &oy));
    CHECK_EQ(ox, 800.0f);
    CHECK_EQ(oy, 600.0f);
    // 任意屏幕点均返回原图坐标（检测框可出现在全屏任意处）
    CHECK(CoordinateTransform::screen_to_detection(10.0f, 10.0f, 100, 50, 640, 360,
                                                   640, 640, &ox, &oy));
    CHECK_EQ(ox, 10.0f);
    CHECK_EQ(oy, 10.0f);
}

TEST(coord_model_to_detection) {
    // 模型坐标(320,320) → 模型 640 → ROI(100,50,320,320) → 检测 = 160+100, 160+50
    float ox = 0, oy = 0;
    CHECK(CoordinateTransform::model_to_detection(320.0f, 320.0f, 640, 640,
                                                  100, 50, 320, 320, &ox, &oy));
    CHECK_EQ(ox, 260.0f);
    CHECK_EQ(oy, 210.0f);
    // 无 ROI（全帧）：直接返回
    CHECK(CoordinateTransform::model_to_detection(100.0f, 200.0f, 640, 640,
                                                  0, 0, 0, 0, &ox, &oy));
    CHECK_EQ(ox, 100.0f);
    CHECK_EQ(oy, 200.0f);
}
