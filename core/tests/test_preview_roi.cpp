// test_preview_roi.cpp — 预览裁剪矩形纯函数测试。
//
// 覆盖现场关心的问题：「主页改了截取尺寸，低帧预览到底裁哪一块」。
// 判错就是"预览看到的和 AI 看到的不是同一块"——业主报的正是这个。
// 期望值直接照 rknn/WorkerPool::apply_runtime_profile 的 ROI 公式手算，
// 两边一旦分叉这里就红。
#include <cstdint>

#include "preview/PreviewRoi.hpp"
#include "test_util.hpp"

using ttbox::core::compute_preview_roi;
using ttbox::core::fit_preview_output;
using ttbox::core::PreviewRoi;

namespace {

constexpr uint32_t kFrameW = 2560;  // 采集帧（HDMI 2560x1440）
constexpr uint32_t kFrameH = 1440;

}  // namespace

// 无偏移：ROI = 屏幕正中的 截取尺寸 方块。
TEST(center_crop_without_offset) {
    const PreviewRoi roi = compute_preview_roi(kFrameW, kFrameH, 256, 256);
    CHECK_EQ(roi.x, 1152u);  // (2560-256)/2
    CHECK_EQ(roi.y, 592u);   // (1440-256)/2
    CHECK_EQ(roi.w, 256u);
    CHECK_EQ(roi.h, 256u);
}

// 截取尺寸跟着面板档位走：320 / 640 都要对。
TEST(crop_size_follows_capture_profile) {
    const PreviewRoi r320 = compute_preview_roi(kFrameW, kFrameH, 320, 320);
    CHECK_EQ(r320.w, 320u);
    CHECK_EQ(r320.x, 1120u);  // (2560-320)/2
    CHECK_EQ(r320.y, 560u);   // (1440-320)/2

    const PreviewRoi r640 = compute_preview_roi(kFrameW, kFrameH, 640, 640);
    CHECK_EQ(r640.w, 640u);
    CHECK_EQ(r640.x, 960u);
    CHECK_EQ(r640.y, 400u);
}

// ★ V1.0.13（2026-09-30）：capture.offset_x/y 已删 —— 裁剪区恒以屏幕中心为心。
//   原先三条「偏移位移 / 偏移 clamp 到全帧」的用例随之作废（函数签名也去掉了两个偏移参）。
//   反过来锁一条：任何非零偏移（贴边 / 中间）都不可能再出现。

// 截取尺寸 == 全帧 ⇒ 起点 0，就是整帧。
TEST(capture_equals_frame_is_full_frame) {
    const PreviewRoi roi = compute_preview_roi(kFrameW, kFrameH, kFrameW, kFrameH);
    CHECK_EQ(roi.x, 0u);
    CHECK_EQ(roi.y, 0u);
    CHECK_EQ(roi.w, kFrameW);
    CHECK_EQ(roi.h, kFrameH);
}

// 0×0（全帧/自动）⇒ 中心正方形，与 WorkerPool 的「自动中心区域」一致。
TEST(zero_capture_falls_back_to_center_square) {
    const PreviewRoi roi = compute_preview_roi(kFrameW, kFrameH, 0, 0);
    CHECK_EQ(roi.w, 1440u);  // min(2560,1440)
    CHECK_EQ(roi.h, 1440u);
    CHECK_EQ(roi.x, 560u);   // (2560-1440)/2
    CHECK_EQ(roi.y, 0u);
}

// 截取尺寸超过全帧（坏配置）⇒ 不硬裁，退回中心正方形。
TEST(capture_larger_than_frame_falls_back) {
    const PreviewRoi roi = compute_preview_roi(kFrameW, kFrameH, 4096, 4096);
    CHECK_EQ(roi.w, 1440u);
    CHECK_EQ(roi.h, 1440u);
    CHECK_EQ(roi.x, 560u);
    CHECK_EQ(roi.y, 0u);
}

// 奇数帧宽高：整数除法不能错位（1111x999）。
TEST(odd_frame_dimensions_use_integer_center) {
    const PreviewRoi roi = compute_preview_roi(1111, 999, 101, 101);
    CHECK_EQ(roi.x, 505u);  // 1111/2=555, 555-50
    CHECK_EQ(roi.y, 449u);  // 999/2=499, 499-50
    CHECK_EQ(roi.w, 101u);
    CHECK_EQ(roi.h, 101u);
}

// 空帧（0 尺寸）⇒ 全零，调用方据此拒绝编码，不 panic。
TEST(zero_sized_frame_yields_empty_roi) {
    const PreviewRoi roi = compute_preview_roi(0, 0, 256, 256);
    CHECK_EQ(roi.w, 0u);
    CHECK_EQ(roi.h, 0u);
}

// 输出尺寸：ROI 在上限内 ⇒ 原样输出，**不放大**。
TEST(output_size_never_upscales) {
    uint32_t w = 0;
    uint32_t h = 0;
    fit_preview_output(256, 256, 640, 640, &w, &h);
    CHECK_EQ(w, 256u);
    CHECK_EQ(h, 256u);

    fit_preview_output(640, 640, 640, 640, &w, &h);
    CHECK_EQ(w, 640u);
    CHECK_EQ(h, 640u);
}

// 输出尺寸：ROI 超出上限 ⇒ 等比缩小到上限内（方框）。
TEST(output_size_downscales_square_to_fit) {
    uint32_t w = 0;
    uint32_t h = 0;
    fit_preview_output(1440, 1440, 640, 640, &w, &h);
    CHECK_EQ(w, 640u);
    CHECK_EQ(h, 640u);
}

// 输出尺寸：非方 ROI ⇒ 等比缩小，长边贴上限，短边按比例。
TEST(output_size_preserves_aspect_when_downscaling) {
    uint32_t w = 0;
    uint32_t h = 0;
    fit_preview_output(2560, 1440, 640, 640, &w, &h);
    CHECK_EQ(w, 640u);
    CHECK_EQ(h, 360u);  // 1440 * (640/2560)
}

// 上限传 0 = 该轴不设限 ⇒ 不缩放。
TEST(output_size_zero_limit_means_no_cap) {
    uint32_t w = 0;
    uint32_t h = 0;
    fit_preview_output(256, 256, 0, 0, &w, &h);
    CHECK_EQ(w, 256u);
    CHECK_EQ(h, 256u);
}

int main() {
    std::printf("=== test_preview_roi ===\n");
    const int failed = ::ttbox_test::run_all();
    return failed == 0 ? 0 : 1;
}
