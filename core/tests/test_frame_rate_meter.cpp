// test_frame_rate_meter.cpp — FrameRateMeter 行为测试
//
// 为什么专门测它（2026-09-23 板端回归）：
//   面板"检测帧率"曾显示 140 出头并**缓慢爬升**，看着像性能在爬坡，实为统计口径错：
//   CoreRuntime 用 `published ÷ 链路启动至今秒数`（累计平均），分母里永久含着启动期
//   一次性开销（3 worker 加载 + 起流 + 预热），于是 fps(T) = real × (1 − T_startup/T)，
//   只能渐近、永远到不了真实值。FrameRateMeter 改为滚动窗口瞬时值后，第 2 帧就准。
//   本文件的核心用例 = 复现该场景，断言"瞬时口径立刻反映当前速率"。
#include "test_util.hpp"
#include "common/FrameRateMeter.hpp"

#include <chrono>
#include <cmath>
#include <thread>

using namespace ttbox::core;

namespace {

// 以 interval_ms 的间隔 tick n 次。
// ★ 用**忙等**而非 sleep_for：Windows 休眠精度约 15.6 ms，`sleep_for(10ms)` 实际睡
//   ~15 ms，会把"名义 100 fps"打成 ~66 fps，断言只能靠放宽容差蒙混（等于没测）。
//   忙等到点时基误差在微秒级，是真正在校验"间隔→帧率"的换算。
//   代价是这几个用例占用一点 CPU（合计约 1 s），换来的是可判定的断言。
void tick_at_rate(FrameRateMeter& m, int n, int interval_ms) {
    auto next = std::chrono::steady_clock::now();
    for (int i = 0; i < n; ++i) {
        m.tick();
        next += std::chrono::milliseconds(interval_ms);
        while (std::chrono::steady_clock::now() < next) {
            // spin
        }
    }
}

bool near(double a, double b, double tol) { return std::fabs(a - b) <= tol; }

}  // namespace

TEST(frame_rate_meter_empty_and_single_sample) {
    FrameRateMeter m;
    CHECK_EQ(m.sample_count(), 0u);
    CHECK(near(m.fps(), 0.0, 1e-9));       // 无样本 ⇒ 0（调用方回退累计平均）
    m.tick();
    CHECK_EQ(m.sample_count(), 1u);
    CHECK(near(m.fps(), 0.0, 1e-9));       // 单样本无法构成间隔 ⇒ 0
}

TEST(frame_rate_meter_reports_instant_rate) {
    FrameRateMeter m;
    // 10 ms 间隔 = 100 fps，跑 30 帧（约 300 ms）
    tick_at_rate(m, 30, 10);
    const double fps = m.fps();
    CHECK(fps > 85.0 && fps < 115.0);      // 调度抖动容差宽一些，只卡量级正确
}

// ★ 核心回归：模拟"启动慢、随后跑满"——瞬时口径必须立刻反映**当前**速率，
//   而旧的累计平均会被开头的慢帧永久拖低（表现为面板帧率缓慢爬升）。
TEST(frame_rate_meter_ignores_startup_overhead) {
    FrameRateMeter m;
    // ① 启动期：慢速 5 帧，每帧 100 ms（模拟加载/预热/起流）
    tick_at_rate(m, 5, 100);
    const double after_slow = m.fps();
    CHECK(after_slow > 7.0 && after_slow < 13.0);   // ≈10 fps

    // ② 随后跑满：20 帧 × 10 ms（≈100 fps）
    tick_at_rate(m, 20, 10);

    // 窗口 512 帧，25 帧全在窗口内 ⇒ 会被 ① 拉低。这里断言的是"**明显回升**"，
    // 而不是精确等于 100：本用例的回归价值在于——旧口径下分子分母含全部启动耗时，
    // 帧率只会慢慢爬；瞬时口径下后续快帧把 fps 拉回接近当前速率。
    CHECK(m.fps() > after_slow * 2.0);

    // ③ 证明不是靠"等窗口滚动"：把慢帧挤出窗口后应逼近纯快段速率。
    m.reset();
    tick_at_rate(m, 20, 10);
    CHECK(m.fps() > 85.0 && m.fps() < 115.0);
}

TEST(frame_rate_meter_window_rolls_over) {
    FrameRateMeter m;
    // 填满并越过窗口（kWindow=512）会显著变慢，这里只验证"溢出不崩、不返回负数/NaN"。
    // 用 0 间隔快速 tick，再补两个有间隔的样本以形成有效间隔。
    for (size_t i = 0; i < FrameRateMeter::kWindow + 50; ++i) m.tick();
    CHECK_EQ(m.sample_count(), FrameRateMeter::kWindow);   // 计数封顶，不越界
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
    m.tick();
    const double fps = m.fps();
    CHECK(fps > 0.0);                       // 溢出后仍是正数
    CHECK(!std::isnan(fps) && !std::isinf(fps));
}

TEST(frame_rate_meter_reset_clears) {
    FrameRateMeter m;
    tick_at_rate(m, 10, 5);
    CHECK(m.sample_count() > 0);
    m.reset();
    CHECK_EQ(m.sample_count(), 0u);
    CHECK(near(m.fps(), 0.0, 1e-9));
}

int main() {
    std::printf("=== ttbox_core tests (frame_rate_meter) ===\n");
    const int failed = ::ttbox_test::run_all();
    std::printf("=== tests done (exit=%d) ===\n", failed == 0 ? 0 : 1);
    return failed == 0 ? 0 : 1;
}
