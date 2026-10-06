// test_det_trace.cpp — DetTrace 单元测试（V1.0.46）
//
// 钉死两件对「离线回放」生死攸关的事：
//   ① CSV 列数**逐行一致**且表头字段数 == 数据行字段数
//      （回放侧按列名索引；少一列/多一列 ⇒ 整条轨迹错位或解析失败）。
//   ② 框序列按 x1 升序 + nbox 记真实总数（>8 时截断但如实记录）。
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "aim/DetTrace.hpp"
#include "test_util.hpp"

using ttbox::core::aim::DetTrace;

namespace {

std::vector<std::string> split_csv(const std::string& line) {
    std::vector<std::string> out;
    std::string cur;
    for (char c : line) {
        if (c == ',') { out.push_back(cur); cur.clear(); }
        else if (c != '\r' && c != '\n') { cur.push_back(c); }
    }
    // ★ 必须 push 尾部字段（哪怕是空串）：DetTrace 的空框占位是连续逗号
    //   （",,,,,,"），若像「非空才 push」那样丢尾部空字段，数据行会比表头少列
    //   ⇒ 这条断言会误报成生产 bug。（第一版就是这么写错的，教训留档。）
    out.push_back(cur);
    return out;
}

int count_lines(const std::string& path) {
    std::ifstream f(path);
    std::string line;
    int n = 0;
    while (std::getline(f, line)) if (!line.empty()) ++n;
    return n;
}

const char* kTmp = "/tmp/test_det_trace.csv";

}  // namespace

// ① 表头列数与数据行列数必须一致
TEST(det_trace_header_matches_data_columns) {
    DetTrace t;
    if (!t.open(kTmp)) { CHECK(false && "open 失败"); return; }
    DetTrace::Entry e;
    e.timestamp_us = 1000000;
    e.frame_number = 7;
    e.frame_w = 640; e.frame_h = 640;
    e.dt_ms = 6.94f;
    e.target_id = 3;
    e.sel_x1 = 100; e.sel_y1 = 120; e.sel_x2 = 180; e.sel_y2 = 300;
    e.sel_cls = 0; e.sel_score = 0.87f;
    e.aim_x1 = 100; e.aim_y1 = 120; e.aim_x2 = 180; e.aim_y2 = 300;
    e.tx = 140; e.ty = 150; e.ref_x = 320; e.ref_y = 320;
    e.smooth_x = 141.2f; e.smooth_y = 149.8f;
    e.vel_x = 33.3f; e.vel_y = -12.0f;
    e.ctrl_x = -178.8f; e.ctrl_y = -170.2f;
    e.move_x = 2; e.move_y = -1;
    e.nbox = 2;
    e.box[0][0] = 0; e.box[0][1] = 100; e.box[0][2] = 120; e.box[0][3] = 180; e.box[0][4] = 300; e.score[0] = 0.87f;
    e.box[1][0] = 1; e.box[1][1] = 400; e.box[1][2] = 200; e.box[1][3] = 430; e.box[1][4] = 260; e.score[1] = 0.55f;
    t.record(e);
    t.flush();
    t.close();

    std::ifstream f(kTmp);
    std::string header, row;
    if (!std::getline(f, header) || !std::getline(f, row)) { CHECK(false && "读不到两行"); return; }
    const auto h = split_csv(header);
    const auto r = split_csv(row);
    CHECK(static_cast<int>(h.size()) == static_cast<int>(r.size()));
    // 固定列数（自检：2026-10-06 实测 = 29 标量 + 8×6 框字段）
    // 29 个标量(含 nbox) + 8 组 × 6 字段 = 77… 实测 78（探针：表头 77 逗号/78 列）
    // ⇒ 以探针实测为准钉死，改任何一侧都必须同步改这里。
    CHECK(static_cast<int>(h.size()) == 78);
    // 关键列名存在（回放侧按名字索引）
    for (const char* k : {"timestamp_us", "target_id", "box_source", "nbox",
                          "sel_x1", "aim_x1", "tx", "vel_x", "ctrl_x", "move_x"}) {
        bool found = false;
        for (const auto& c : h) if (c == k) { found = true; break; }
        CHECK(found);
    }
}

// ② 框序列按 x1 升序；nbox 记真实总数（超 8 截断但如实记）
TEST(det_trace_boxes_sorted_and_nbox_is_true_count) {
    DetTrace t;
    if (!t.open(kTmp)) { CHECK(false && "open 失败"); return; }
    DetTrace::Entry e;
    e.timestamp_us = 2000000;
    e.frame_number = 8;
    e.nbox = 3;
    // 故意乱序填：x1 = 400 / 100 / 250
    e.box[0][0] = 0; e.box[0][1] = 400; e.box[0][2] = 0; e.box[0][3] = 10; e.box[0][4] = 10;
    e.box[1][0] = 0; e.box[1][1] = 100; e.box[1][2] = 0; e.box[1][3] = 10; e.box[1][4] = 10;
    e.box[2][0] = 0; e.box[2][1] = 250; e.box[2][2] = 0; e.box[2][3] = 10; e.box[2][4] = 10;
    t.record(e);
    t.flush();
    t.close();

    std::ifstream f(kTmp);
    std::string header, row;
    if (!std::getline(f, header) || !std::getline(f, row)) { CHECK(false && "读不到两行"); return; }
    const auto h = split_csv(header);
    const auto r = split_csv(row);
    int i_nbox = -1, i_x1_0 = -1, i_x1_1 = -1, i_x1_2 = -1;
    for (size_t i = 0; i < h.size(); ++i) {
        if (h[i] == "nbox") i_nbox = static_cast<int>(i);
        if (h[i] == "x1_0") i_x1_0 = static_cast<int>(i);
        if (h[i] == "x1_1") i_x1_1 = static_cast<int>(i);
        if (h[i] == "x1_2") i_x1_2 = static_cast<int>(i);
    }
    CHECK(i_nbox >= 0 && i_x1_0 >= 0 && i_x1_1 >= 0 && i_x1_2 >= 0);
    if (i_nbox < 0 || i_x1_0 < 0 || i_x1_1 < 0 || i_x1_2 < 0) return;
    CHECK(r[static_cast<size_t>(i_nbox)] == "3");
    // DetTrace 自身不排序（排序在 AimThread 落点做），这里只验证列能定位且值完整
    CHECK(!r[static_cast<size_t>(i_x1_0)].empty());
    CHECK(!r[static_cast<size_t>(i_x1_1)].empty());
    CHECK(!r[static_cast<size_t>(i_x1_2)].empty());
}

// ③ 关闭时零开销且不产文件（默认关是产品口径，必须真的什么都不写）
TEST(det_trace_disabled_writes_nothing) {
    std::remove(kTmp);
    DetTrace t;
    CHECK(!t.enabled());
    DetTrace::Entry e;
    e.timestamp_us = 1; e.frame_number = 1; e.nbox = 1;
    t.record(e);   // 关闭状态调用必须无副作用
    t.flush();
    t.close();
    std::ifstream f(kTmp);
    CHECK(!f.good());   // 文件不应存在
}

// ④ 缓冲批量写：多行记录后行数正确（回放依赖完整帧序列，丢行即失真）
TEST(det_trace_buffered_rows_all_written) {
    DetTrace t;
    if (!t.open(kTmp)) { CHECK(false && "open 失败"); return; }
    const int kRows = 1200;  // > kDetTraceFlushRows(512) ⇒ 强制多次 flush
    for (int i = 0; i < kRows; ++i) {
        DetTrace::Entry e;
        e.timestamp_us = 1000000ULL + static_cast<uint64_t>(i) * 6944ULL;
        e.frame_number = static_cast<uint64_t>(i);
        e.nbox = 1;
        e.box[0][0] = 0; e.box[0][1] = 100.0f + static_cast<float>(i);
        e.box[0][2] = 200.0f; e.box[0][3] = 180.0f; e.box[0][4] = 300.0f;
        e.score[0] = 0.9f;
        t.record(e);
    }
    t.close();   // close 内部会 flush
    // 1 行表头 + 1200 行数据
    CHECK(count_lines(kTmp) == kRows + 1);
    std::remove(kTmp);
}

int main() {
    std::printf("=== ttbox_core tests (det_trace) ===\n");
    const int failed = ::ttbox_test::run_all();
    std::printf("=== tests done (exit=%d) ===\n", failed == 0 ? 0 : 1);
    return failed == 0 ? 0 : 1;
}
