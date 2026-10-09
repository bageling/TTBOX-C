// Types.hpp — C++ Core 基础数据结构（本阶段只定义结构，不实现视觉逻辑）
#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace ttbox::core {

// 像素格式枚举（占位，后续视觉链路使用）
enum class PixelFormat : uint32_t {
    kUnknown = 0,
    kBGR888 = 1,    // V4L2 BGR3 / BGR24
    kRGB888 = 2,
    kRGBA8888 = 3,
    kNV12 = 4,
};

// 单帧描述信息（元数据，不含像素数据）
struct FrameInfo {
    uint32_t width = 0;         // 像素宽
    uint32_t height = 0;        // 像素高
    uint32_t stride = 0;        // 行字节数（bytes per line）
    PixelFormat format = PixelFormat::kUnknown;
    uint32_t sequence = 0;      // 兼容旧采集实现的帧序号
    double timestamp_ms = 0.0;  // 兼容旧实现的采集时间戳（ms）
    // Core 稳定契约字段：使用单调时钟，微秒精度。
    uint64_t frame_number = 0;
    uint64_t timestamp_us = 0;

    // --- DMA-BUF 信息（阶段 A-2 采集模块填充；-1 表示无 fd）---
    int dma_fd = -1;            // 主 plane 的 DMA-BUF fd（future RgaProcessor 消费入口）
    void* cpu_va = nullptr;     // 采集侧 mmap 虚拟地址（CPU 直拷用；nullptr=不可用）
    uint32_t buffer_index = 0;  // V4L2 mmap buffer index
    uint32_t num_planes = 1;    // plane 数量
};

// 帧缓冲（数据 + 元数据；data 可为空表示"仅元数据"占位）
struct FrameBuffer {
    std::shared_ptr<uint8_t[]> data;  // 像素数据（或 nullptr）
    size_t size = 0;                  // 有效字节数
    FrameInfo info;
};

// 单个检测框（原图坐标像素）
struct DetectionBox {
    float x1 = 0.0f;
    float y1 = 0.0f;
    float x2 = 0.0f;
    float y2 = 0.0f;
    float score = 0.0f;
    int class_id = 0;
};

// 单帧推理结果
struct InferenceResult {
    std::vector<DetectionBox> detections;
    double infer_ms = 0.0;   // 推理耗时（不含后处理）
    double decode_ms = 0.0;  // 后处理耗时
    double timestamp_ms = 0.0;
    bool has_result = false;  // 本帧是否完成一次完整推理
};

}  // namespace ttbox::core
