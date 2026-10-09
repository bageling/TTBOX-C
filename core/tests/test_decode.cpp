// test_decode.cpp — DecodeNMS 单元测试（合成数据，host 可跑，零硬件依赖）
//
// 覆盖：正常输出、空检测、多目标、边界框解码、classwise NMS、坐标映射、
//       FP16 输出解码、objectness 变体。
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <vector>

#include "common/Types.hpp"
#include "rknn/DecodeNMS.hpp"
#include "rknn/RKNNEngine.hpp"
#include "test_util.hpp"

using namespace ttbox::core;

namespace {

constexpr uint32_t kChannels = 84;   // 4 reg + 80 cls
constexpr uint32_t kAnchors = 8400;  // yolo261n

// 构造单输出 FLOAT32 模型信息（(1,84,8400) NCHW）
RknnModelInfo make_info(int channels = kChannels, int anchors = kAnchors,
                        int type = 0, float scale = 1.0f, int zp = 0) {
    RknnModelInfo info;
    info.n_inputs = 1;
    info.n_outputs = 1;
    RknnOutputInfo oi;
    oi.n_elems = static_cast<uint32_t>(channels) * static_cast<uint32_t>(anchors);
    oi.size = oi.n_elems * 4;  // 先按 4 字节（FP16 覆盖处自行覆盖）
    oi.type = type;
    oi.fmt = 1;  // NCHW
    oi.dims = {1, static_cast<uint32_t>(channels), static_cast<uint32_t>(anchors)};
    oi.scale = scale;
    oi.zp = zp;
    info.output_n_elems.push_back(oi.n_elems);
    info.output_sizes.push_back(oi.size);
    info.outputs.push_back(std::move(oi));
    return info;
}

// 在 (C,M) NCHW 布局中设置 anchor a 的第 c 通道值
void set_elem(std::vector<float>& buf, int anchors, int c, int a, float v) {
    buf[static_cast<size_t>(c) * anchors + a] = v;
}

// 就地配置 DecodeNMS（默认 conf=0.25 iou=0.45，640 输入，可选 frame 映射/输入尺寸）
void make_decode(DecodeNMS* d, uint32_t frame_w = 0, uint32_t frame_h = 0,
                 uint32_t in_w = 640, uint32_t in_h = 640) {
    DecodeParams dp;
    dp.conf_thres = 0.25f;
    dp.iou_thres = 0.45f;
    dp.classwise = true;
    dp.input_w = in_w;
    dp.input_h = in_h;
    dp.frame_w = frame_w;
    dp.frame_h = frame_h;
    std::string err;
    if (!d->configure(dp, &err)) {
        std::printf("configure 失败: %s\n", err.c_str());
    }
}

bool near(float a, float b) { return std::fabs(a - b) < 0.01f; }

}  // namespace

// 空检测：全零输出 → 0 个检测
TEST(decode_empty_output) {
    auto info = make_info();
    std::vector<float> raw(info.outputs[0].n_elems, 0.0f);
    void* ptrs[] = {raw.data()};
    DecodeNMS d;
    make_decode(&d);
    std::vector<DetectionBox> dets;
    std::string err;
    CHECK(d.process(info, ptrs, &dets, &err));
    CHECK_EQ(dets.size(), 0u);
    CHECK_EQ(d.stats().candidates.load(), 0u);
}

// 单目标：xywh + 类别解码，框坐标正确
TEST(decode_single_object) {
    auto info = make_info();
    std::vector<float> raw(info.outputs[0].n_elems, 0.0f);
    // anchor 123：中心 (100,200) wh (80,40)，类别 3 分数 0.9
    const int a = 123;
    set_elem(raw, kAnchors, 0, a, 100.0f);
    set_elem(raw, kAnchors, 1, a, 200.0f);
    set_elem(raw, kAnchors, 2, a, 80.0f);
    set_elem(raw, kAnchors, 3, a, 40.0f);
    set_elem(raw, kAnchors, 4 + 3, a, 0.9f);
    void* ptrs[] = {raw.data()};
    DecodeNMS d;
    make_decode(&d);
    std::vector<DetectionBox> dets;
    std::string err;
    CHECK(d.process(info, ptrs, &dets, &err));
    CHECK_EQ(dets.size(), 1u);
    CHECK(near(dets[0].x1, 60.0f));
    CHECK(near(dets[0].y1, 180.0f));
    CHECK(near(dets[0].x2, 140.0f));
    CHECK(near(dets[0].y2, 220.0f));
    CHECK(near(dets[0].score, 0.9f));
    CHECK_EQ(dets[0].class_id, 3);
    CHECK_EQ(d.stats().candidates.load(), 1u);
    CHECK_EQ(d.stats().detections.load(), 1u);
}

// 多目标：两个不同类别，互不抑制
TEST(decode_multi_object_distinct_classes) {
    auto info = make_info();
    std::vector<float> raw(info.outputs[0].n_elems, 0.0f);
    // a=10：类 0 高分；a=20：类 1 高分，不同位置
    set_elem(raw, kAnchors, 0, 10, 100.0f);
    set_elem(raw, kAnchors, 1, 10, 100.0f);
    set_elem(raw, kAnchors, 2, 10, 50.0f);
    set_elem(raw, kAnchors, 3, 10, 50.0f);
    set_elem(raw, kAnchors, 4 + 0, 10, 0.8f);
    set_elem(raw, kAnchors, 0, 20, 500.0f);
    set_elem(raw, kAnchors, 1, 20, 500.0f);
    set_elem(raw, kAnchors, 2, 20, 60.0f);
    set_elem(raw, kAnchors, 3, 20, 60.0f);
    set_elem(raw, kAnchors, 4 + 1, 20, 0.7f);
    void* ptrs[] = {raw.data()};
    DecodeNMS d;
    make_decode(&d);
    std::vector<DetectionBox> dets;
    std::string err;
    CHECK(d.process(info, ptrs, &dets, &err));
    CHECK_EQ(dets.size(), 2u);
    CHECK_EQ(d.stats().candidates.load(), 2u);
}

// NMS：同类别重叠框，低分被抑制；不同类别重叠不抑制
TEST(decode_nms_suppression) {
    auto info = make_info();
    std::vector<float> raw(info.outputs[0].n_elems, 0.0f);
    // 同一位置 (300,300) wh 100 的两个同类框：0.9 与 0.5 → 只留 0.9
    set_elem(raw, kAnchors, 0, 1, 300.0f);
    set_elem(raw, kAnchors, 1, 1, 300.0f);
    set_elem(raw, kAnchors, 2, 1, 100.0f);
    set_elem(raw, kAnchors, 3, 1, 100.0f);
    set_elem(raw, kAnchors, 4 + 5, 1, 0.9f);
    set_elem(raw, kAnchors, 0, 2, 302.0f);
    set_elem(raw, kAnchors, 1, 2, 302.0f);
    set_elem(raw, kAnchors, 2, 2, 100.0f);
    set_elem(raw, kAnchors, 3, 2, 100.0f);
    set_elem(raw, kAnchors, 4 + 5, 2, 0.5f);
    // 不同类别 (400,400) 同类框 0.7 → 不被抑制
    set_elem(raw, kAnchors, 0, 3, 400.0f);
    set_elem(raw, kAnchors, 1, 3, 400.0f);
    set_elem(raw, kAnchors, 2, 3, 100.0f);
    set_elem(raw, kAnchors, 3, 3, 100.0f);
    set_elem(raw, kAnchors, 4 + 6, 3, 0.7f);
    set_elem(raw, kAnchors, 0, 4, 402.0f);
    set_elem(raw, kAnchors, 1, 4, 402.0f);
    set_elem(raw, kAnchors, 2, 4, 100.0f);
    set_elem(raw, kAnchors, 3, 4, 100.0f);
    set_elem(raw, kAnchors, 4 + 6, 4, 0.4f);
    void* ptrs[] = {raw.data()};
    DecodeNMS d;
    make_decode(&d);
    std::vector<DetectionBox> dets;
    std::string err;
    CHECK(d.process(info, ptrs, &dets, &err));
    CHECK_EQ(dets.size(), 2u);  // 类5 留 0.9；类6 留 0.7
    std::vector<float> scores;
    for (const auto& b : dets) scores.push_back(b.score);
    std::sort(scores.begin(), scores.end());
    CHECK(near(scores[0], 0.7f));
    CHECK(near(scores[1], 0.9f));
}

// 坐标映射：640 空间 → 1920x1080 原图
TEST(decode_coordinate_mapping) {
    auto info = make_info();
    std::vector<float> raw(info.outputs[0].n_elems, 0.0f);
    const int a = 55;
    set_elem(raw, kAnchors, 0, a, 320.0f);
    set_elem(raw, kAnchors, 1, a, 320.0f);
    set_elem(raw, kAnchors, 2, a, 64.0f);
    set_elem(raw, kAnchors, 3, a, 64.0f);
    set_elem(raw, kAnchors, 4 + 2, a, 0.85f);
    void* ptrs[] = {raw.data()};
    DecodeNMS d;
    make_decode(&d, 1920, 1080);
    std::vector<DetectionBox> dets;
    std::string err;
    CHECK(d.process(info, ptrs, &dets, &err));
    CHECK_EQ(dets.size(), 1u);
    // 模型空间 x1=288,y1=288,x2=352,y2=352；sx=3.0, sy=1.6875
    CHECK(near(dets[0].x1, 288.0f * 3.0f));       // 864
    CHECK(near(dets[0].y1, 288.0f * 1.6875f));    // 486
    CHECK(near(dets[0].x2, 352.0f * 3.0f));       // 1056
    CHECK(near(dets[0].y2, 352.0f * 1.6875f));    // 594
}

// FP16 输出类型：半精度解码
TEST(decode_fp16_output) {
    auto info = make_info(kChannels, kAnchors, 1);  // type=1: FLOAT16
    std::vector<uint16_t> raw16(info.outputs[0].n_elems, 0);
    // float -> IEEE half（与生产 float_to_half 一致，就近舍入）
    auto to_half = [](float f) -> uint16_t {
        uint32_t b;
        std::memcpy(&b, &f, sizeof(b));
        const uint32_t sign = (b >> 16) & 0x8000u;
        int32_t exp = static_cast<int32_t>((b >> 23) & 0xFF) - 127 + 15;
        uint32_t man = b & 0x7FFFFFu;
        if (exp >= 31) return static_cast<uint16_t>(sign | 0x7C00u);
        if (exp <= 0) return static_cast<uint16_t>(sign);
        const uint32_t rem = man & 0x1FFFu;
        man >>= 13;
        if (rem > 0x1000u || (rem == 0x1000u && (man & 1u))) ++man;
        if (man > 0x3FFu) { man = 0; ++exp; }
        if (exp >= 31) return static_cast<uint16_t>(sign | 0x7C00u);
        return static_cast<uint16_t>(sign | (static_cast<uint32_t>(exp) << 10) | man);
    };
    const int a = 7;
    const auto idx = [&](int c) { return static_cast<size_t>(c) * kAnchors + a; };
    raw16[idx(0)] = to_half(100.0f);
    raw16[idx(1)] = to_half(200.0f);
    raw16[idx(2)] = to_half(80.0f);
    raw16[idx(3)] = to_half(40.0f);
    raw16[idx(4 + 3)] = to_half(0.9f);
    info.outputs[0].size = info.outputs[0].n_elems * 2;
    void* ptrs[] = {raw16.data()};
    DecodeNMS d;
    make_decode(&d);
    std::vector<DetectionBox> dets;
    std::string err;
    CHECK(d.process(info, ptrs, &dets, &err));
    CHECK_EQ(dets.size(), 1u);
    CHECK(near(dets[0].x1, 60.0f));
    CHECK(near(dets[0].score, 0.9f));
}

// objectness 变体：C=85（4 reg + 1 obj + 80 cls），score = obj * cls
TEST(decode_objectness_variant) {
    constexpr int C = 85;
    auto info = make_info(C, kAnchors, 0);
    std::vector<float> raw(info.outputs[0].n_elems, 0.0f);
    const int a = 9;
    set_elem(raw, kAnchors, 0, a, 200.0f);
    set_elem(raw, kAnchors, 1, a, 200.0f);
    set_elem(raw, kAnchors, 2, a, 50.0f);
    set_elem(raw, kAnchors, 3, a, 50.0f);
    set_elem(raw, kAnchors, 4, a, 0.5f);      // objectness（logit，decode 内做 sigmoid）
    set_elem(raw, kAnchors, 5 + 0, a, 0.9f);  // 类 0（已是 sigmoid 概率）
    void* ptrs[] = {raw.data()};
    DecodeNMS d;
    make_decode(&d);
    std::vector<DetectionBox> dets;
    std::string err;
    CHECK(d.process(info, ptrs, &dets, &err));
    CHECK_EQ(dets.size(), 1u);
    // 2026-09 真机修复（fc2d4e8）：objectness 为 logit 需 sigmoid，类别通道已是概率。
    // 期望 score = sigmoid(0.5) * 0.9 ≈ 0.560
    CHECK(near(dets[0].score, 0.5602f));
    CHECK_EQ(dets[0].class_id, 0);
}

// 奇数输出数量 → 明确报错
TEST(decode_odd_outputs_rejected) {
    RknnModelInfo info;
    info.n_outputs = 3;
    std::vector<float> raw(100, 0.0f);
    void* ptrs[] = {raw.data(), raw.data(), raw.data()};
    DecodeNMS d;
    make_decode(&d);
    std::vector<DetectionBox> dets;
    std::string err;
    CHECK(!d.process(info, ptrs, &dets, &err));
    CHECK(!err.empty());
}

// 多输出 DFL（黄瓦模型格式）：3 尺度 × [box(1,1,4,M), cls(1,C,H,W)]
TEST(decode_dfl_multiscale) {
    // 尺度：grid 40/20/10 → 锚点 1600/400/100；stride 8/16/32（输入 320）
    const uint32_t grids[3] = {40, 20, 10};
    const uint32_t anchors[3] = {1600, 400, 100};
    RknnModelInfo info;
    info.n_inputs = 1;
    info.n_outputs = 6;
    std::vector<std::vector<float>> bufs;  // [box0, cls0, box1, cls1, box2, cls2]
    std::vector<void*> ptrs;

    for (int si = 0; si < 3; ++si) {
        // box (1,1,4,M)
        RknnOutputInfo bo;
        bo.n_elems = 4 * anchors[si];
        bo.size = bo.n_elems * 4;
        bo.type = 0;  // FLOAT32
        bo.dims = {1, 1, 4, anchors[si]};
        info.outputs.push_back(bo);
        bufs.emplace_back(bo.n_elems, 0.0f);
        // cls (1,2,grid,grid)
        RknnOutputInfo co;
        co.n_elems = 2 * grids[si] * grids[si];
        co.size = co.n_elems * 4;
        co.type = 0;
        co.dims = {1, 2, grids[si], grids[si]};
        info.outputs.push_back(co);
        bufs.emplace_back(co.n_elems, -10.0f);  // 背景 logit -10（sigmoid≈4.5e-5）
    }
    for (auto& b : bufs) ptrs.push_back(b.data());
    for (size_t i = 0; i < bufs.size(); ++i) {
        info.output_n_elems.push_back(static_cast<uint32_t>(bufs[i].size()));
        info.output_sizes.push_back(static_cast<uint32_t>(bufs[i].size() * sizeof(float)));
    }

    // 尺度0（stride 8）目标：锚点 row=20,col=30 → a=20*40+30=830
    const uint32_t a = 20 * grids[0] + 30;
    auto& box0 = bufs[0];
    auto& cls0 = bufs[1];
    const uint32_t M0 = anchors[0];
    box0[0 * M0 + a] = 1.0f;  // dl
    box0[1 * M0 + a] = 1.0f;  // dt
    box0[2 * M0 + a] = 1.0f;  // dr
    box0[3 * M0 + a] = 1.0f;  // db
    const uint32_t G0 = grids[0];
    cls0[0 * G0 * G0 + a] = 5.0f;   // 类0 sigmoid=0.993
    cls0[1 * G0 * G0 + a] = -10.0f;  // 类1

    // 尺度1（stride 16）目标：锚点 row=5,col=10 → a=5*20+10=110
    const uint32_t a1 = 5 * grids[1] + 10;
    auto& box1 = bufs[2];
    auto& cls1 = bufs[3];
    const uint32_t M1 = anchors[1];
    box1[0 * M1 + a1] = 0.5f;
    box1[1 * M1 + a1] = 0.5f;
    box1[2 * M1 + a1] = 0.5f;
    box1[3 * M1 + a1] = 0.5f;
    const uint32_t G1 = grids[1];
    cls1[0 * G1 * G1 + a1] = 4.0f;
    cls1[1 * G1 * G1 + a1] = -10.0f;

    DecodeNMS d;
    make_decode(&d, 0, 0, 320, 320);  // DFL：输入 320，stride 8/16/32
    std::vector<DetectionBox> dets;
    std::string err;
    CHECK(d.process(info, ptrs.data(), &dets, &err));
    // 期望 2 个检测（两个尺度各 1）
    CHECK_EQ(dets.size(), 2u);
    // 尺度0 框：cx=(30+0.5)*8=244, cy=(20+0.5)*8=164；dl/dt/dr/db=1 → x1=236,y1=156,x2=252,y2=172
    CHECK(near(dets[0].x1, 236.0f));
    CHECK(near(dets[0].y1, 156.0f));
    CHECK(near(dets[0].x2, 252.0f));
    CHECK(near(dets[0].y2, 172.0f));
    CHECK(near(dets[0].score, 0.9933f));
    CHECK_EQ(dets[0].class_id, 0);
    CHECK_EQ(d.stats().candidates.load(), 2u);
    CHECK_EQ(d.stats().detections.load(), 2u);
}
