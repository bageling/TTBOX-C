// PreviewRoi.hpp — 预览裁剪矩形（纯函数，host 可单测）
//
// 预览必须显示「模型实际看到的区域」：由 RuntimeProfile::capture 决定
// （截取尺寸 capture.width/height）。
// ★ V1.0.13（2026-09-30）：capture.offset_x/y 已删 —— 裁剪区恒以屏幕中心为心。
//
// 2026-09-20 业主定案：主页「截取尺寸」改多少，低帧预览就裁多少。
// 此前 PreviewModule 用 preview.width/height（部署值 640）自己裁一块固定中心方块，
// 于是「模型看 256、预览看 640」——预览与 AI 视野不是同一块，
// 而且预览窗口大小一变就以为改的是「尺寸」。把矩形计算收敛到这里：
//
//   · compute_preview_roi 的公式与 rknn/WorkerPool::apply_runtime_profile 的 ROI
//     **逐字一致**（中心、clamp 到全帧内），保证预览框 == 推理 ROI；
//   · capture 为 0×0（全帧/自动）或超出全帧 ⇒ 中心正方形 side=min(fw,fh)，
//     与 WorkerPool 里 0×0 分支的 decoder_->set_roi(中心正方形) 语义一致。
//
// 两个函数都不碰文件、不碰硬件，所以进得了 host 单测（tests/test_preview_roi.cpp）。
#pragma once

#include <algorithm>
#include <cstdint>

namespace ttbox::core {

// 预览裁剪矩形（采集帧像素坐标系；左上角起点 + 宽高）。
struct PreviewRoi {
    uint32_t x = 0;  // 左上角起点（采集帧像素系）
    uint32_t y = 0;
    uint32_t w = 0;
    uint32_t h = 0;
};

// 与 WorkerPool 同式：ROI 中心 = 屏幕中心，转左上角起点并 clamp 到全帧内。
inline PreviewRoi compute_preview_roi(uint32_t frame_w, uint32_t frame_h,
                                      uint32_t cap_w, uint32_t cap_h) {
    PreviewRoi roi;
    if (frame_w == 0 || frame_h == 0) return roi;

    if (cap_w > 0 && cap_h > 0 && cap_w <= frame_w && cap_h <= frame_h) {
        const int32_t cx = static_cast<int32_t>(frame_w / 2);
        const int32_t cy = static_cast<int32_t>(frame_h / 2);
        const int32_t rx = std::max<int32_t>(
            0, std::min<int32_t>(cx - static_cast<int32_t>(cap_w / 2),
                                 static_cast<int32_t>(frame_w - cap_w)));
        const int32_t ry = std::max<int32_t>(
            0, std::min<int32_t>(cy - static_cast<int32_t>(cap_h / 2),
                                 static_cast<int32_t>(frame_h - cap_h)));
        roi.x = static_cast<uint32_t>(rx);
        roi.y = static_cast<uint32_t>(ry);
        roi.w = cap_w;
        roi.h = cap_h;
        return roi;
    }

    // 0×0（全帧/自动）或越界 ⇒ 中心正方形。
    const uint32_t side = std::min(frame_w, frame_h);
    roi.x = (frame_w - side) / 2;
    roi.y = (frame_h - side) / 2;
    roi.w = side;
    roi.h = side;
    return roi;
}

// 预览输出尺寸：ROI 不超过上限就按原尺寸输出（**不放大**——放大只是白烧 CPU，
// 浏览器本来就要把画面缩放到面板），超过才等比缩小以限制 JPEG 体量与编码耗时。
// max_w/max_h 传 0 表示该轴不设限。
inline void fit_preview_output(uint32_t roi_w, uint32_t roi_h, uint32_t max_w,
                               uint32_t max_h, uint32_t* out_w, uint32_t* out_h) {
    if (out_w == nullptr || out_h == nullptr) return;
    uint32_t w = std::max<uint32_t>(1, roi_w);
    uint32_t h = std::max<uint32_t>(1, roi_h);
    if (max_w == 0) max_w = w;
    if (max_h == 0) max_h = h;
    if (w > max_w || h > max_h) {
        const double f = std::min(static_cast<double>(max_w) / static_cast<double>(w),
                                  static_cast<double>(max_h) / static_cast<double>(h));
        w = std::max<uint32_t>(1, static_cast<uint32_t>(static_cast<double>(w) * f));
        h = std::max<uint32_t>(1, static_cast<uint32_t>(static_cast<double>(h) * f));
    }
    *out_w = w;
    *out_h = h;
}

}  // namespace ttbox::core
