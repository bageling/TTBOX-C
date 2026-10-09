// Preprocess.hpp — 统一采集帧到检测输入的预处理边界
#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "common/Types.hpp"
// ★ 条件源闭包（P0-1）：RgaProcessor.hpp 仍**无条件**包含（RgaOutput / RgaMetrics 是 POD，
//   不产生符号依赖）；但**持有 RgaProcessor 实例**（rga_）与**调用其方法**的两处 inline 路径
//   条件编译为 TTBOX_CORE_HAS_RGA（原为 `__unix__ && !__APPLE__` ⇒ 无 RGA 的 UNIX host 上
//   `~Preprocess` 仍销毁 unique_ptr<RgaProcessor> ⇒ 链接期 undefined reference to ~RgaProcessor）。
#include "rga/RgaProcessor.hpp"
#include "common/CoreContracts.hpp"

namespace ttbox::core {

// 统一检测尺寸：Detector 的工作输入尺寸。模型尺寸由 ActiveModel/RKNN 元数据决定。
struct DetectSize {
    uint32_t width = 0;
    uint32_t height = 0;
};

// 预处理后端选择：RGA 硬件或 CPU 回退。
enum class PreprocessBackend { kRga, kCpuFallback };

// 预处理配置：目标尺寸、输入类型/字节数、后端、裁剪裁区与颜色顺序。
struct PreprocessConfig {
    DetectSize detect_size;
    int input_type = 3;  // RKNN 输入类型：2=INT8，3=UINT8，其余按 FP16/FLOAT 转换
    size_t input_size = 0;
    PreprocessBackend backend = PreprocessBackend::kRga;
    bool center_crop = true;
    int color_order = 0;  // 0=BGR, 1=RGB
    bool single_pass = true;  // RGA 单段 crop+resize（失败自动回退两段）
    uint32_t crop_x = 0;
    uint32_t crop_y = 0;
    uint32_t crop_width = 0;
    uint32_t crop_height = 0;
};

// 预处理结果：送模张量指针/大小，以及 RGA 输出与 CPU/FP16 存储的保活句柄。
struct PreprocessedFrame {
    bool ok = false;
    DetectSize detect_size;
    PixelFormat format = PixelFormat::kUnknown;
    uint32_t stride = 0;
    int dma_fd = -1;
    const uint8_t* data = nullptr;
    size_t size = 0;
    const uint8_t* tensor_data = nullptr;
    size_t tensor_size = 0;
    RgaOutput rga_output{};
    std::shared_ptr<std::vector<uint8_t>> cpu_storage;
    std::shared_ptr<std::vector<uint16_t>> fp16_storage;
};

// 预处理：把采集帧统一裁剪/缩放到模型输入尺寸（默认 RGA，CPU 仅显式回退）。
class Preprocess {
public:
    Preprocess() = default;
    ~Preprocess() = default;
    Preprocess(const Preprocess&) = delete;
    Preprocess& operator=(const Preprocess&) = delete;

    // 初始化（校验尺寸并按后端建立 RGA 实例）。
    bool init(const PreprocessConfig& config, std::string* error = nullptr);
    // 处理一帧：产出送模张量。
    bool process(const FrameBuffer& input, PreprocessedFrame* output,
                 std::string* error = nullptr);
    // 更新裁剪 ROI。
    void set_crop(uint32_t x, uint32_t y, uint32_t width, uint32_t height);
    const PreprocessConfig& config() const { return config_; }
    bool using_rga() const {
#if defined(TTBOX_CORE_HAS_RGA) && TTBOX_CORE_HAS_RGA
        return rga_ != nullptr;
#else
        return false;
#endif
    }
    const RgaMetrics* rga_metrics() const {
#if defined(TTBOX_CORE_HAS_RGA) && TTBOX_CORE_HAS_RGA
        return rga_ ? &rga_->metrics() : nullptr;
#else
        return nullptr;
#endif
    }

private:
    bool process_cpu(const FrameBuffer& input, PreprocessedFrame* output,
                     std::string* error);

    PreprocessConfig config_{};
#if defined(TTBOX_CORE_HAS_RGA) && TTBOX_CORE_HAS_RGA
    std::unique_ptr<RgaProcessor> rga_;
#endif
    bool initialized_ = false;
};

}  // namespace ttbox::core
