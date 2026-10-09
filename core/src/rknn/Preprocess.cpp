// Preprocess.cpp — 统一预处理实现（RGA 默认，CPU 仅显式回退）
//
// ★ 条件源闭包（P0-1）：RGA 分支的条件统一为 TTBOX_CORE_HAS_RGA（原为 `__unix__ && !__APPLE__`）。
//   根因：RgaProcessor.cpp **仅在 RGA 可用时**编入 ttbox_core；消费端若用更宽的 `__unix__` 条件，
//   则无 RGA 的 UNIX host 上 init()/process()/set_crop() 会引用 RgaProcessor::* 却无定义
//   ⇒ 链接期 `undefined reference to RgaProcessor::init/process/~RgaProcessor`（P0-1 host 实测）。
//   现条件与 RgaProcessor.cpp 的编译条件**逐字一致**：无 RGA ⇒ 走 CPU fallback / 明确报错（优雅降级）。
#include "rknn/Preprocess.hpp"

#include <algorithm>
#include <cstring>

namespace ttbox::core {

namespace {
// float 转 IEEE half（尾数截断到 10 位）。
uint16_t float_to_half(float f) {
    uint32_t bits = 0;
    std::memcpy(&bits, &f, sizeof(bits));
    const uint32_t sign = (bits >> 16) & 0x8000u;
    int exp = static_cast<int>((bits >> 23) & 0xffu) - 127 + 15;
    uint32_t mantissa = bits & 0x7fffffu;
    if (exp >= 31) return static_cast<uint16_t>(sign | 0x7c00u);
    if (exp <= 0) return static_cast<uint16_t>(sign);
    mantissa >>= 13;
    return static_cast<uint16_t>(sign | (static_cast<uint32_t>(exp) << 10) | mantissa);
}

// 决定 CPU 预处理结果如何送模：量化/FP16 透传原始像素，其它类型转 half。
void finalize_tensor(PreprocessedFrame* output, int input_type) {
    // 2026-09-03 真实链路修复：对量化(INT8/UINT8)与 FLOAT16 模型统一透传原始 UINT8 像素。
    // 之前对 FLOAT16 型转 half /255 归一化，导致 yolo261n（已声明 FP16 输入）收到 0-1 half，
    // 模型类别通道全 ~0 而"失明"；改喂原始 0-255 像素由 RKNN runtime 自行量化（见 RKNNEngine
    // set_input 同注释），与 rknnlite 参考实现一致，INT8/UINT8 模型路径保持不变。
    if (input_type == 2 || input_type == 3 || !output->cpu_storage ||
        input_type == 1 /* FP16: 透传原始像素 */) {
        output->tensor_data = output->data;
        output->tensor_size = output->size;
        return;
    }
    const size_t count = output->cpu_storage->size();
    output->fp16_storage = std::make_shared<std::vector<uint16_t>>(count);
    for (size_t i = 0; i < count; ++i) {
        (*output->fp16_storage)[i] = float_to_half((*output->cpu_storage)[i] / 255.0f);
    }
    output->tensor_data = reinterpret_cast<const uint8_t*>(output->fp16_storage->data());
    output->tensor_size = output->fp16_storage->size() * sizeof(uint16_t);
}

// 决定 RGA 预处理结果如何送模：量化/FP16 透传原始像素，其它类型逐行转 half。
void finalize_rga_tensor(PreprocessedFrame* output, int input_type) {
    // 同上：FLOAT16 模型透传 RGA 原始 BGR888（0-255），不做 half /255 归一化。
    if (input_type == 2 || input_type == 3 || !output->data ||
        input_type == 1 /* FP16: 透传原始像素 */) {
        output->tensor_data = output->data;
        output->tensor_size = output->size;
        return;
    }
    const uint32_t width = output->detect_size.width;
    const uint32_t height = output->detect_size.height;
    const uint32_t stride = output->stride ? output->stride : width * 3;
    output->fp16_storage = std::make_shared<std::vector<uint16_t>>(static_cast<size_t>(width) * height * 3);
    for (uint32_t y = 0; y < height; ++y) {
        const uint8_t* row = output->data + static_cast<size_t>(y) * stride;
        for (uint32_t x = 0; x < width * 3; ++x) {
            (*output->fp16_storage)[static_cast<size_t>(y) * width * 3 + x] = float_to_half(row[x] / 255.0f);
        }
    }
    output->tensor_data = reinterpret_cast<const uint8_t*>(output->fp16_storage->data());
    output->tensor_size = output->fp16_storage->size() * sizeof(uint16_t);
}
}  // namespace

// 初始化预处理：校验尺寸，并按后端建立 RGA 或标记为 CPU 回退。
bool Preprocess::init(const PreprocessConfig& config, std::string* error) {
    if (config.detect_size.width == 0 || config.detect_size.height == 0) {
        if (error) *error = "detect_size 必须大于 0";
        return false;
    }
    if (config.detect_size.width > 8192 || config.detect_size.height > 8192) {
        if (error) *error = "detect_size 超出合理范围";
        return false;
    }
    config_ = config;
#if defined(TTBOX_CORE_HAS_RGA) && TTBOX_CORE_HAS_RGA
    if (config_.backend == PreprocessBackend::kRga) {
        rga_ = std::make_unique<RgaProcessor>();
        RgaProcessor::Params params;
        params.output_width = config_.detect_size.width;
        params.output_height = config_.detect_size.height;
        params.center_crop = config_.center_crop;
        params.out_color = config_.color_order;
        params.single_pass = config_.single_pass;
        params.roi_x = config_.crop_x;
        params.roi_y = config_.crop_y;
        params.roi_w = config_.crop_width;
        params.roi_h = config_.crop_height;
        if (!rga_->init(params, error)) {
            rga_.reset();
            return false;
        }
    }
#else
    if (config_.backend == PreprocessBackend::kRga) {
        if (error) *error = "当前平台没有 RGA 后端；请显式选择 CPU fallback";
        return false;
    }
#endif
    initialized_ = true;
    return true;
}

// 预处理入口：按后端走 RGA 硬件或 CPU 回退，产出送模张量。
bool Preprocess::process(const FrameBuffer& input, PreprocessedFrame* output,
                         std::string* error) {
    if (!initialized_) {
        if (error) *error = "Preprocess 未初始化";
        return false;
    }
    if (!output) {
        if (error) *error = "Preprocess 输出为空";
        return false;
    }
    *output = {};
    output->detect_size = config_.detect_size;
#if defined(TTBOX_CORE_HAS_RGA) && TTBOX_CORE_HAS_RGA
    if (config_.backend == PreprocessBackend::kRga) {
        if (!rga_ || !rga_->process(input, &output->rga_output, error)) return false;
        output->ok = true;
        output->format = config_.color_order == 0 ? PixelFormat::kBGR888 : PixelFormat::kRGB888;
        output->stride = output->rga_output.stride;
        output->dma_fd = output->rga_output.dma_fd;
        output->data = static_cast<const uint8_t*>(output->rga_output.vir_addr);
        output->size = static_cast<size_t>(output->stride) * output->detect_size.height;
        finalize_rga_tensor(output, config_.input_type);
        return true;
    }
#endif
    return process_cpu(input, output, error);
}

// CPU 回退实现：裁剪后最近邻缩放为 BGR/RGB888。
bool Preprocess::process_cpu(const FrameBuffer& input, PreprocessedFrame* output,
                             std::string* error) {
    if (!input.info.cpu_va || input.info.width == 0 || input.info.height == 0) {
        if (error) *error = "CPU fallback 需要有效 cpu_va 和输入尺寸";
        return false;
    }
    if (input.info.format != PixelFormat::kBGR888 && input.info.format != PixelFormat::kRGB888) {
        if (error) *error = "CPU fallback 当前仅支持 BGR888/RGB888";
        return false;
    }
    const uint32_t source_w = input.info.width;
    const uint32_t source_h = input.info.height;
    uint32_t crop_w = config_.crop_width ? config_.crop_width : source_w;
    uint32_t crop_h = config_.crop_height ? config_.crop_height : source_h;
    if (crop_w > source_w || crop_h > source_h) {
        if (error) *error = "CPU fallback crop 超出输入帧";
        return false;
    }
    const uint32_t crop_x = std::min(config_.crop_x, source_w - crop_w);
    const uint32_t crop_y = std::min(config_.crop_y, source_h - crop_h);
    const uint32_t dw = config_.detect_size.width;
    const uint32_t dh = config_.detect_size.height;
    output->cpu_storage = std::make_shared<std::vector<uint8_t>>(static_cast<size_t>(dw) * dh * 3);
    const auto* src = static_cast<const uint8_t*>(input.info.cpu_va);
    for (uint32_t y = 0; y < dh; ++y) {
        const uint32_t sy = crop_y + std::min(crop_h - 1, (y * crop_h) / dh);
        const uint8_t* row = src + static_cast<size_t>(sy) * input.info.stride;
        for (uint32_t x = 0; x < dw; ++x) {
            const uint32_t sx = crop_x + std::min(crop_w - 1, (x * crop_w) / dw);
            std::memcpy(output->cpu_storage->data() + (static_cast<size_t>(y) * dw + x) * 3,
                        row + static_cast<size_t>(sx) * 3, 3);
        }
    }
    output->ok = true;
    output->format = input.info.format;
    output->stride = dw * 3;
    output->data = output->cpu_storage->data();
    output->size = output->cpu_storage->size();
    finalize_tensor(output, config_.input_type);
    return true;
}

// 更新裁剪 ROI（同时下发给 RGA）。
void Preprocess::set_crop(uint32_t x, uint32_t y, uint32_t width, uint32_t height) {
    config_.crop_x = x;
    config_.crop_y = y;
    config_.crop_width = width;
    config_.crop_height = height;
#if defined(TTBOX_CORE_HAS_RGA) && TTBOX_CORE_HAS_RGA
    if (rga_) rga_->set_roi(x, y, width, height);
#endif
}

}  // namespace ttbox::core
