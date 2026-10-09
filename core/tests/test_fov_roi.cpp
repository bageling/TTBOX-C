// test_fov_roi.cpp — A-8 单元测试：FOV Filter + ROI 坐标映射（合成数据，零硬件）
#include <cmath>
#include <cstdint>
#include <vector>

#include "common/Types.hpp"
#include "model/RuntimeProfile.hpp"
#include "rknn/DecodeNMS.hpp"
#include "rknn/RKNNEngine.hpp"
#include "test_util.hpp"

using namespace ttbox::core;

namespace {

// 单输出 FLOAT32 (1,C,M)
RknnModelInfo make_info(int channels = 84, int anchors = 8400) {
    RknnModelInfo info;
    info.n_inputs = 1;
    info.n_outputs = 1;
    RknnOutputInfo oi;
    oi.n_elems = static_cast<uint32_t>(channels) * static_cast<uint32_t>(anchors);
    oi.size = oi.n_elems * 4;
    oi.type = 0;
    oi.fmt = 1;  // NCHW
    oi.dims = {1, static_cast<uint32_t>(channels), static_cast<uint32_t>(anchors)};
    info.output_n_elems.push_back(oi.n_elems);
    info.output_sizes.push_back(oi.size);
    info.outputs.push_back(std::move(oi));
    return info;
}

void set_elem(std::vector<float>& buf, int anchors, int c, int a, float v) {
    buf[static_cast<size_t>(c) * anchors + a] = v;
}

bool near(float a, float b) { return std::fabs(a - b) < 0.01f; }

}  // namespace

// 目标在屏幕右上角（模型空间 x=200,y=50），全帧映射到 1920x1080
TEST(fov_map_no_roi_full_frame) {
    auto info = make_info();
    std::vector<float> raw(info.outputs[0].n_elems, 0.0f);
    const int a = 5;
    set_elem(raw, 8400, 0, a, 200.0f);
    set_elem(raw, 8400, 1, a, 50.0f);
    set_elem(raw, 8400, 2, a, 40.0f);
    set_elem(raw, 8400, 3, a, 40.0f);
    set_elem(raw, 8400, 4 + 0, a, 0.9f);
    void* ptrs[] = {raw.data()};

    DecodeNMS d;
    DecodeParams dp;
    dp.conf_thres = 0.25f;
    dp.iou_thres = 0.45f;
    dp.input_w = 640;
    dp.input_h = 640;
    dp.frame_w = 1920;
    dp.frame_h = 1080;
    std::string err;
    CHECK(d.configure(dp, &err));

    std::vector<DetectionBox> dets;
    CHECK(d.process(info, ptrs, &dets, &err));
    CHECK_EQ(dets.size(), 1u);
    if (dets.empty()) return;
    // 全帧缩放：x*3, y*1080/640
    CHECK(near(dets[0].x1, 180.0f * 3.0f));
    CHECK(near(dets[0].y1, 30.0f * (1080.0f / 640.0f)));
}

// ROI 裁剪：模型输入 640x640，ROI = 屏幕(100,50,320,320)，缩放后坐标应加偏移
TEST(fov_map_with_roi) {
    auto info = make_info();
    std::vector<float> raw(info.outputs[0].n_elems, 0.0f);
    const int a = 7;
    set_elem(raw, 8400, 0, a, 160.0f);  // ROI 空间中心 x=160（ROI 宽 320，640 输入→×0.5→80）
    set_elem(raw, 8400, 1, a, 160.0f);
    set_elem(raw, 8400, 2, a, 80.0f);
    set_elem(raw, 8400, 3, a, 80.0f);
    set_elem(raw, 8400, 4 + 0, a, 0.8f);
    void* ptrs[] = {raw.data()};

    DecodeNMS d;
    DecodeParams dp;
    dp.conf_thres = 0.25f;
    dp.iou_thres = 0.45f;
    dp.input_w = 640;
    dp.input_h = 640;
    dp.frame_w = 1920;
    dp.frame_h = 1080;
    dp.roi_x = 100;
    dp.roi_y = 50;
    dp.roi_w = 320;
    dp.roi_h = 320;
    std::string err;
    CHECK(d.configure(dp, &err));

    std::vector<DetectionBox> dets;
    CHECK(d.process(info, ptrs, &dets, &err));
    CHECK_EQ(dets.size(), 1u);
    if (dets.empty()) return;
    // 模型坐标 160 → ROI 空间 160*(320/640)=80 → 原图 80+100=180
    // 框中心 (160,160)：x=160*0.5+100=180；y=160*0.5+50=130
    const float cx = (dets[0].x1 + dets[0].x2) * 0.5f;
    const float cy = (dets[0].y1 + dets[0].y2) * 0.5f;
    CHECK(near(cx, 180.0f));
    CHECK(near(cy, 130.0f));
    // 框宽高：模型 80 → ROI 80*0.5=40
    CHECK(near(dets[0].x2 - dets[0].x1, 40.0f));
    CHECK(near(dets[0].y2 - dets[0].y1, 40.0f));
}

// FOV circle：中心 (0.5,0.5)，radius=0.2*min(1920,1080)=216px
// 目标在画面边缘（x≈1900 → 中心 1900 > 中心 960+216=1176）→ 被过滤
TEST(fov_filter_circle_removes_outside) {
    auto info = make_info();
    std::vector<float> raw(info.outputs[0].n_elems, 0.0f);
    const int a = 9;
    set_elem(raw, 8400, 0, a, 630.0f);  // 模型空间 630/640 → 全帧 ≈1890
    set_elem(raw, 8400, 1, a, 320.0f);
    set_elem(raw, 8400, 2, a, 20.0f);
    set_elem(raw, 8400, 3, a, 20.0f);
    set_elem(raw, 8400, 4 + 0, a, 0.9f);
    void* ptrs[] = {raw.data()};

    DecodeNMS d;
    DecodeParams dp;
    dp.conf_thres = 0.25f;
    dp.iou_thres = 0.45f;
    dp.input_w = 640;
    dp.input_h = 640;
    dp.frame_w = 1920;
    dp.frame_h = 1080;
    dp.fov_enabled = true;
    dp.fov_shape = FovShape::kCircle;
    dp.fov_radius = 0.2f;  // 216px
    dp.fov_center_x = 0.5f;
    dp.fov_center_y = 0.5f;
    std::string err;
    CHECK(d.configure(dp, &err));

    std::vector<DetectionBox> dets;
    CHECK(d.process(info, ptrs, &dets, &err));
    CHECK_EQ(dets.size(), 0u);  // 边缘目标被 FOV 过滤
}

// FOV circle：目标在中心附近 → 保留
TEST(fov_filter_circle_keeps_center) {
    auto info = make_info();
    std::vector<float> raw(info.outputs[0].n_elems, 0.0f);
    const int a = 3;
    set_elem(raw, 8400, 0, a, 320.0f);  // 中心
    set_elem(raw, 8400, 1, a, 320.0f);
    set_elem(raw, 8400, 2, a, 30.0f);
    set_elem(raw, 8400, 3, a, 30.0f);
    set_elem(raw, 8400, 4 + 1, a, 0.8f);
    void* ptrs[] = {raw.data()};

    DecodeNMS d;
    DecodeParams dp;
    dp.conf_thres = 0.25f;
    dp.iou_thres = 0.45f;
    dp.input_w = 640;
    dp.input_h = 640;
    dp.frame_w = 1920;
    dp.frame_h = 1080;
    dp.fov_enabled = true;
    dp.fov_shape = FovShape::kCircle;
    dp.fov_radius = 0.2f;
    dp.fov_center_x = 0.5f;
    dp.fov_center_y = 0.5f;
    std::string err;
    CHECK(d.configure(dp, &err));

    std::vector<DetectionBox> dets;
    CHECK(d.process(info, ptrs, &dets, &err));
    CHECK_EQ(dets.size(), 1u);
}

// FOV disabled → 不过滤（默认行为回归）
TEST(fov_disabled_keeps_all) {
    auto info = make_info();
    std::vector<float> raw(info.outputs[0].n_elems, 0.0f);
    const int a = 2;
    set_elem(raw, 8400, 0, a, 630.0f);  // 边缘
    set_elem(raw, 8400, 1, a, 320.0f);
    set_elem(raw, 8400, 2, a, 20.0f);
    set_elem(raw, 8400, 3, a, 20.0f);
    set_elem(raw, 8400, 4 + 0, a, 0.9f);
    void* ptrs[] = {raw.data()};

    DecodeNMS d;
    DecodeParams dp;
    dp.conf_thres = 0.25f;
    dp.iou_thres = 0.45f;
    dp.input_w = 640;
    dp.input_h = 640;
    dp.frame_w = 1920;
    dp.frame_h = 1080;
    std::string err;
    CHECK(d.configure(dp, &err));

    std::vector<DetectionBox> dets;
    CHECK(d.process(info, ptrs, &dets, &err));
    CHECK_EQ(dets.size(), 1u);
}

// A-8：E2E 模型内部已 TopK/NMS → 禁止无条件二次 NMS。
// 构造 [1,N,F] 两行高度重叠同类别框，验证 process_e2e 不 NMS 抑制（保留模型语义）。
TEST(e2e_skip_secondary_nms) {
    RknnModelInfo info;
    info.n_inputs = 1;
    info.n_outputs = 1;
    RknnOutputInfo oi;
    oi.n_elems = 300 * 6;
    oi.size = oi.n_elems * 4;
    oi.type = 0;  // FLOAT32
    oi.fmt = 1;
    oi.dims = {1, 300, 6};
    info.output_n_elems.push_back(oi.n_elems);
    info.output_sizes.push_back(oi.size);
    info.outputs.push_back(std::move(oi));

    // 行 0 与行 1：几乎完全重叠，同 class 0，高分 → 若跑 NMS 会被抑制一个
    std::vector<float> raw(300 * 6, 0.0f);
    raw[0] = 100; raw[1] = 100; raw[2] = 200; raw[3] = 200; raw[4] = 0.9f; raw[5] = 0;
    raw[6] = 101; raw[7] = 101; raw[8] = 201; raw[9] = 201; raw[10] = 0.8f; raw[11] = 0;
    void* ptrs[] = {raw.data()};

    DecodeNMS d;
    DecodeParams dp;
    dp.conf_thres = 0.25f;
    dp.iou_thres = 0.45f;
    dp.input_w = 640;
    dp.input_h = 640;
    dp.frame_w = 640;
    dp.frame_h = 640;
    dp.e2e_skip_nms = true;  // 默认：E2E 跳过二次 NMS
    std::string err;
    CHECK(d.configure(dp, &err));

    std::vector<DetectionBox> dets;
    CHECK(d.process(info, ptrs, &dets, &err));
    CHECK_EQ(dets.size(), 2u);  // 模型输出保留（不二次 NMS）

    // 关闭 skip 时（兼容路径）仍不抑制 —— 保持模型语义（当前实现两者都不 NMS）
    DecodeNMS d2;
    DecodeParams dp2 = dp;
    dp2.e2e_skip_nms = false;
    CHECK(d2.configure(dp2, &err));
    std::vector<DetectionBox> dets2;
    CHECK(d2.process(info, ptrs, &dets2, &err));
    CHECK_EQ(dets2.size(), 2u);
}
