// test_stats_window.cpp — S6 内存泄漏修复回归：StatsCollector 滑动窗口语义
// 背景：samples_ 曾无界 push_back（worker 每帧多路 add 且永不清空），
// 板上 2h 长稳定位 73.6 MB/h 泄漏，根因即此。本测试钉死窗口行为。
#include "common/Stats.hpp"

#include <cstdio>
#include <thread>
#include <vector>

using ttbox::core::StatsCollector;

// ★ 本文件原本全用 assert，而本项目构建是 Release（`-DNDEBUG`）⇒ assert 被**整段编译掉**，
//   整个文件实际什么都没检查、永远退出 0。2026-09-23 反向验证时暴露：把聚合器退回默认构造后，
//   文件仍打印 "ALL ... PASSED" 且退出码 0，而实际输出的是 count=4096/min=20001（缺陷已复现）。
//   改为不受 NDEBUG 影响的显式检查，并由 main 按失败数返回非零。
static int g_failures = 0;

#define CHECK_TRUE(cond)                                                       \
    do {                                                                       \
        if (!(cond)) {                                                         \
            ++g_failures;                                                      \
            std::printf("  [FAIL] %s:%d  %s\n", __FILE__, __LINE__, #cond);    \
        }                                                                      \
    } while (0)

static void test_unbounded_before_window_fills() {
    StatsCollector s;
    for (uint64_t i = 1; i <= 100; ++i) s.add(i);
    CHECK_TRUE(s.count() == 100);
    CHECK_TRUE(s.min() == 1);
    CHECK_TRUE(s.max() == 100);
    CHECK_TRUE(s.avg() == 50.5);
    CHECK_TRUE(s.percentile(50) == 50 || s.percentile(50) == 51);  // 插值落在这两者
    std::printf("  [ok] 未满窗口顺序语义不变\n");
}

static void test_window_caps_and_overwrites_oldest() {
    StatsCollector s;
    for (uint64_t i = 1; i <= StatsCollector::kMaxSamples + 1000; ++i) {
        s.add(i);
        CHECK_TRUE(s.count() <= StatsCollector::kMaxSamples);  // 任何时刻都有界
    }
    // 末窗口 = 最后 kMaxSamples 个样本 [1001+... ]，逐段核对：
    CHECK_TRUE(s.count() == StatsCollector::kMaxSamples);
    CHECK_TRUE(s.max() == StatsCollector::kMaxSamples + 1000);
    // 最旧样本 1001 已被覆盖：min 应 > 1000
    CHECK_TRUE(s.min() > 1000);
    std::printf("  [ok] 窗口封顶 %zu，最旧样本被环形覆盖\n", StatsCollector::kMaxSamples);
}

static void test_window_keeps_recent_only() {
    StatsCollector s;
    // 先灌 1..kMax，再灌一个大值：大值必须保留，旧值被挤出
    for (uint64_t i = 1; i <= StatsCollector::kMaxSamples; ++i) s.add(i);
    s.add(9'999'999);
    CHECK_TRUE(s.count() == StatsCollector::kMaxSamples);
    CHECK_TRUE(s.max() == 9'999'999);
    // 挤出的是 1（head 指向的位置即最旧者）
    CHECK_TRUE(s.min() == 2);
    std::printf("  [ok] 覆盖的是最旧样本（min 2..kMax + 新值）\n");
}

static void test_absorb_respects_window() {
    StatsCollector a, b;
    for (uint64_t i = 1; i <= StatsCollector::kMaxSamples; ++i) a.add(i);
    // b 的值域刻意与 a 不重叠，才能判断"b 的样本到底进没进窗口"。
    // （原断言 a.max() == 100 隐含"吸收后只剩 b"——那正是窗口被挤穿的错误语义，
    //   它此前被 -DNDEBUG 编译掉所以从未暴露；2026-09-23 启用检查后发现并改正。）
    constexpr uint64_t kBFar = 50000;
    constexpr uint64_t kBCount = 100;
    for (uint64_t i = 0; i < kBCount; ++i) b.add(kBFar + i);
    a.absorb(b);
    CHECK_TRUE(a.count() == StatsCollector::kMaxSamples);   // absorb 后仍有界
    CHECK_TRUE(a.max() == kBFar + kBCount - 1);            // b 的样本确实在窗口里
    CHECK_TRUE(a.min() == kBCount + 1);                    // 被环形覆盖掉的正是最旧的 1..kBCount
    // 自吸收无害
    a.absorb(a);
    CHECK_TRUE(a.count() == StatsCollector::kMaxSamples);
    std::printf("  [ok] absorb 有界 + b 样本入窗 + 覆盖最旧 + 自吸收无害\n");
}

// ★ 合并容器（多路聚合）必须装下所有源的样本 —— 2026-09-23 全仓审查复核 #11 的回归。
//   修复前 CoreRuntime 用默认 4096 窗口做聚合，3 路各 4096 样本吸进去后窗口里只剩最后一路的
//   样本（后吸的整段挤掉先吸的），于是 e2e_p95/p99「名义全局、实际单路」。
//   反向验证：把 merged 改回默认构造（StatsCollector merged;）后，下面 min()/count() 必红。
static void test_merged_collector_keeps_all_sources() {
    constexpr uint64_t kSeg = 10000;  // 三路值域互不重叠，便于判断"谁还在窗口里"
    StatsCollector merged(StatsCollector::kMaxSamples * 3);
    StatsCollector w0, w1, w2;
    for (uint64_t j = 1; j <= StatsCollector::kMaxSamples; ++j) {
        w0.add(j);
        w1.add(kSeg + j);
        w2.add(kSeg * 2 + j);
    }
    merged.absorb(w0);
    merged.absorb(w1);
    merged.absorb(w2);

    CHECK_TRUE(merged.count() == StatsCollector::kMaxSamples * 3);  // 三路样本全在
    CHECK_TRUE(merged.min() == 1);                                  // 第 1 路的最小值没被挤掉
    CHECK_TRUE(merged.max() == kSeg * 2 + StatsCollector::kMaxSamples);  // 第 3 路的最大值也在
    // 修复前这里最多只剩第 3 路：count 会被截到 kMaxSamples、min 会是 kSeg*2+1
    std::printf("  [ok] 合并容器保留全部三路样本（count=%zu min=%llu max=%llu）\n", merged.count(),
                static_cast<unsigned long long>(merged.min()),
                static_cast<unsigned long long>(merged.max()));
}

static void test_clear_resets_head() {
    StatsCollector s;
    for (uint64_t i = 0; i < StatsCollector::kMaxSamples * 2; ++i) s.add(i);
    s.clear();
    CHECK_TRUE(s.count() == 0);
    CHECK_TRUE(s.avg() == 0.0);
    CHECK_TRUE(s.percentile(95) == 0);
    s.add(42);
    CHECK_TRUE(s.count() == 1);
    CHECK_TRUE(s.avg() == 42.0);
    std::printf("  [ok] clear 重置窗口与游标\n");
}

static void test_percentile_bounded_memory() {
    // percentile 内部拷贝受窗口约束——间接验证：大样本量下调用不炸、语义对
    StatsCollector s;
    for (uint64_t i = 1; i <= StatsCollector::kMaxSamples; ++i) s.add(i);
    CHECK_TRUE(s.percentile(0) == 1);
    CHECK_TRUE(s.percentile(100) == StatsCollector::kMaxSamples);
    CHECK_TRUE(s.percentile(50) == StatsCollector::kMaxSamples / 2 ||
           s.percentile(50) == StatsCollector::kMaxSamples / 2 + 1);
    std::printf("  [ok] percentile 边界正确\n");
}

static void test_concurrent_add_safe() {
    StatsCollector s;
    constexpr int kThreads = 4;
    constexpr int kPerThread = 5000;
    std::vector<std::thread> ts;
    for (int t = 0; t < kThreads; ++t) {
        ts.emplace_back([&s] {
            for (int i = 0; i < kPerThread; ++i) s.add(static_cast<uint64_t>(i));
        });
    }
    for (auto& th : ts) th.join();
    CHECK_TRUE(s.count() == StatsCollector::kMaxSamples);  // 全部 add 都不丢锁、总量有界
    std::printf("  [ok] 多线程 add 安全且有界\n");
}

int main() {
    std::printf("== StatsCollector 滑动窗口（S6 修复回归）==\n");
    test_unbounded_before_window_fills();
    test_window_caps_and_overwrites_oldest();
    test_window_keeps_recent_only();
    test_absorb_respects_window();
    test_merged_collector_keeps_all_sources();
    test_clear_resets_head();
    test_percentile_bounded_memory();
    test_concurrent_add_safe();
    if (g_failures != 0) {
        std::printf("FAILED: %d 项检查未通过（%zu 样本窗口）\n", g_failures,
                    StatsCollector::kMaxSamples);
        return 1;
    }
    std::printf("ALL %zu-window TESTS PASSED\n", StatsCollector::kMaxSamples);
    return 0;
}
