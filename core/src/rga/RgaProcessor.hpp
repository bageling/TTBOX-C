// RgaProcessor.hpp — RGA 硬件缩放处理器（阶段 A-3）
//
// 链路边界（来自架构 module-boundaries）：
//   V4L2Capture ──FrameBuffer{dma_fd}──▶ RgaProcessor ──RgaOutput{dma_fd}──▶ (RKNN, A-4)
//
// 设计要点：
//   - 输入：A-2 FrameBuffer（info.dma_fd + width/height/stride），全程无 CPU memcpy
//   - 输出：固定模型输入尺寸（由调用方从模型/config 提供，禁止硬编码 640）
//   - 语义：center crop + resize（imcrop → imresize），或直接拉伸（center_crop=false）
//   - 生命周期：输出/中间 buffer 常驻复用（禁止每帧 malloc）；输入 fd 每帧
//     importbuffer_fd/releasebuffer_handle 成对（无 handle 泄漏）
//   - RGA 失败：明确错误 + 资源释放（不泄漏、不静默）
//   - 统计：import/crop/resize/total 独立耗时（us）
//   - 线程：process() 单线程调用（本阶段无 worker）；调用方负责保活输入引用，
//     即 RGA 完成前不得释放 FrameBuffer（V4L2 buffer 禁止提前归还）
#pragma once

#include <atomic>
#include <cstdint>
#include <memory>
#include <string>

#include "common/Types.hpp"

namespace ttbox::core {

// RGA 处理统计（us；多字段便于独立观察 import/crop/resize/total）
struct RgaMetrics {
    std::atomic<uint64_t> frames{0};      // process 调用次数
    std::atomic<uint64_t> ok_frames{0};   // 成功帧数
    std::atomic<uint64_t> error_frames{0};  // 失败帧数
    std::atomic<uint64_t> single_ok{0};   // 单段 improcess 成功帧数
    std::atomic<uint64_t> fallback_count{0}; // 单段失败回退两段次数
    // 累计耗时（us）
    std::atomic<uint64_t> import_sum_us{0};
    std::atomic<uint64_t> crop_sum_us{0};
    std::atomic<uint64_t> resize_sum_us{0};
    std::atomic<uint64_t> total_sum_us{0};
    std::atomic<uint64_t> single_sum_us{0};
    // 最近一次耗时（us）
    std::atomic<uint32_t> last_import_us{0};
    std::atomic<uint32_t> last_crop_us{0};
    std::atomic<uint32_t> last_resize_us{0};
    std::atomic<uint32_t> last_total_us{0};
};

// RGA 输出（指向 RgaProcessor 持有的复用 buffer，生命周期由 RgaProcessor 管理）
struct RgaOutput {
    bool ok = false;
    int dma_fd = -1;       // 输出 DMA-BUF fd（供 A-4 RKNN 消费）
    void* vir_addr = nullptr;  // mmap 虚拟地址（如需 CPU 读回/后续接入）
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t stride = 0;   // 物理行字节数（BGR888：stride = 物理 wstride*3）
};

// RgaProcessor：用 RGA（im2d）硬件完成 crop+resize 的预处理（DMA-BUF → DMA-BUF，零 CPU 拷贝）。
class RgaProcessor {
public:
    // RGA 处理参数（init() 传入）。
    struct Params {
        // 模型输入尺寸：必须由调用方从模型/config 提供（禁止在此硬编码）
        uint32_t output_width = 0;
        uint32_t output_height = 0;
        bool center_crop = true;  // true=center crop 后 resize；false=直接拉伸
        // 输出颜色顺序（模型输入要求，来自 config/model，不写死）：
        //   0 = BGR888（OpenCV 系模型，如黄瓦/yolo261n）；1 = RGB888（PIL/torch 系模型，如 v26m）
        int out_color = 0;
        // true=improcess 单段完成 crop+resize（失败自动回退 imcrop+imresize）
        bool single_pass = true;
        // A-8 ROI：屏幕截取区域（Capture ROI ≠ 模型输入尺寸）。
        //   roi_w/h == 0 = 未启用（保持 center_crop 原语义）；启用时裁剪该矩形后 resize 到模型输入。
        uint32_t roi_x = 0, roi_y = 0, roi_w = 0, roi_h = 0;
    };

    // 构造：仅创建内部 Impl。
    RgaProcessor();
    // 析构：destroy()。
    ~RgaProcessor();
    RgaProcessor(const RgaProcessor&) = delete;
    RgaProcessor& operator=(const RgaProcessor&) = delete;

    // 初始化：校验参数 + 分配/导入输出 buffer（BGR888）。失败给出明确错误。
    bool init(const Params& params, std::string* error = nullptr);
    void destroy();  // 释放全部 RGA buffer/handle/fd（幂等）

    // A-8：安全点热更新 ROI（worker 单线程调用；mid buffer 懒重建）。
    // 在 worker 处理帧之间调用（不并发 process）。
    void set_roi(uint32_t roi_x, uint32_t roi_y, uint32_t roi_w, uint32_t roi_h) {
        params_.roi_x = roi_x;
        params_.roi_y = roi_y;
        params_.roi_w = roi_w;
        params_.roi_h = roi_h;
    }

    // 是否已初始化。
    bool initialized() const { return inited_; }

    // 处理一帧。input 需含有效 dma_fd；调用方须在 process 返回前保活 input。
    // 成功返回 true，output 指向内部复用 buffer（下次 process 会被覆盖）。
    bool process(const FrameBuffer& input, RgaOutput* output,
                 std::string* error = nullptr);

    // 处理指标引用。
    const RgaMetrics& metrics() const { return metrics_; }

private:
    // PIMPL 实现（常驻输出 buffer + 懒分配中间 buffer）。
    struct Impl;
    std::unique_ptr<Impl> impl_;
    // 已保存参数。
    Params params_;
    // 处理指标。
    RgaMetrics metrics_;
    // 是否已初始化。
    bool inited_ = false;
};

}  // namespace ttbox::core
