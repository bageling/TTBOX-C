// ModelMetadata.hpp — 模型统一元数据（阶段 A-7 ModelAdapter）
//
// 目标：用结构化的模型描述替代任何"按模型名/版本号写死"的逻辑。
// 三种已验证模型必须全部可由本 Metadata 描述：
//   - 黄瓦 320×320 INT8（6 输出 DFL，2 类，BGR）
//   - yolo261n 640×640 FP16（单输出 (1,84,8400)，80 类，BGR）
//   - v26m 640×640 INT8/FP16（单输出 [1,300,6] E2E，RGB）
//
// 字段说明：dtype/layout 与 rknn_api.h 枚举值对齐（由 RKNN runtime 提供，
// 不从文件名/模型名猜测）；decode 相关字段由 ModelAdapter 从输出结构自动推断。
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace ttbox::core {

// 解码类型（统一抽象；由输出结构自动推断，禁止文件名判断）
enum class DecodeType : int {
    kUnknown = 0,
    kSingle = 1,      // 单输出 (1,C,M)：前 4 通道 xywh + 类别（yolo261n）
    kDfl = 2,         // 多输出成对 box/cls：DFL 距离已解码（黄瓦）
    kE2e = 3,         // 单输出 [1,N,F] F<=16：端到端已解码（v26m）
    kDflDist = 4,     // 多输出 reg-bins DFL：reg(1,64,H,W)+cls(1,C,H,W)+aux(1,1,H,W)（大腕256）
    kDflPairDist = 5, // 多输出成对 reg/cls bins（无 aux）：reg(1,64,H,W)+cls(1,C,H,W)（sjz-XCSH）
};

// 量化类型（输入侧）
enum class QuantType : int {
    kNone = 0,   // 浮点输入（FP16/FLOAT32）
    kInt8 = 1,
    kUint8 = 2,
};

// 颜色顺序（模型输入要求，由 ModelAdapter/config 提供，禁止模型特判）
enum class ColorOrder : int {
    kBgr = 0,
    kRgb = 1,
};

// NMS 类型
enum class NmsType : int {
    kClasswise = 0,
    kGlobal = 1,
};

// 坐标格式（模型输出坐标语义）
enum class CoordFormat : int {
    kXywh = 0,   // (cx,cy,w,h)，DFL/Single 解码后转 xyxy
    kXyxy = 1,   // 直接 [x1,y1,x2,y2]（v26m E2E）
    kLtrb = 2,   // DFL 距离 (l,t,r,b) 已解码为 dl,dt,dr,db
};

// ---------------------------------------------------------------------------
// ModelMetadata：单模型完整描述（加载时解析一次，禁止逐帧解析）
// ---------------------------------------------------------------------------
struct ModelMetadata {
    // ---- 输入 ----
    uint32_t input_width = 0;
    uint32_t input_height = 0;
    uint32_t input_channels = 0;
    int input_dtype = 0;          // rknn_tensor_type（0=F32,1=FP16,2=INT8,3=UINT8）
    int input_layout = 0;         // rknn_tensor_format（0=NCHW,1=NHWC）
    ColorOrder color_order = ColorOrder::kBgr;
    QuantType quantization_type = QuantType::kNone;
    size_t input_size = 0;        // 输入 tensor 字节（含对齐，来自 rknn）

    // ---- 输出 ----
    uint32_t output_count = 0;
    std::vector<int> output_dtypes;              // 每输出 dtype
    std::vector<int> output_layouts;             // 每输出 layout
    std::vector<std::vector<uint32_t>> output_shapes;  // 每输出完整 dims

    // ---- 解码（ModelAdapter 推断）----
    DecodeType decode_type = DecodeType::kUnknown;
    std::vector<uint32_t> strides;    // DFL 各尺度 stride（黄瓦 {32,16,8}）
    uint32_t class_count = 0;
    bool objectness = false;          // 输出含 objectness 通道（C-4==81）
    bool dfl = false;                 // 是否 DFL 解码
    NmsType nms_type = NmsType::kClasswise;
    CoordFormat coordinate_format = CoordFormat::kXywh;

    // ---- 运行默认（优先级：Model Default < Runtime Default < User Config）----
    float default_conf = 0.25f;
    float default_iou = 0.45f;

    // ---- 模型标识（仅诊断/日志，禁止用于逻辑分发）----
    std::string label;
};

}  // namespace ttbox::core
