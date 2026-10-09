// ModelAdapter.cpp — 模型统一适配器实现
/*
 * TTBOX 文件说明
 *
 * 文件：ModelAdapter.cpp
 *
 * 作用：
 *   AI 模型适配器，负责读取模型元数据并适配不同格式的模型。
 *
 * 小白理解：
 *   不同的 AI 模型有不同的输入输出格式。
 *   这个模块负责适配各种模型，让 TTBOX 不挑食。
 *
 * 注意：
 *   本注释仅用于说明代码，不改变程序逻辑。
 */

#include "model/ModelAdapter.hpp"

#include <cmath>

#include "common/Logger.hpp"

namespace ttbox::core {

// ---------------------------------------------------------------------------
// 输出结构推断（禁止按模型名/文件名/标签判断）
// ---------------------------------------------------------------------------

// 依据输出数量与各维尺寸判定解码类型；无法识别返回 kUnknown
DecodeType ModelAdapter::infer_decode_type(const RknnModelInfo& info) {
    if (info.n_outputs == 0 || info.outputs.empty()) {
        return DecodeType::kUnknown;
    }
    if (info.n_outputs == 1) {
        const auto& oi = info.outputs[0];
        // E2E：单输出 3D [1,N,F]，F 小（每行固定字段，如 v26m [1,300,6]）
        if (oi.dims.size() == 3 && oi.dims[2] > 0 && oi.dims[2] <= 16 && oi.dims[1] > 0) {
            return DecodeType::kE2e;
        }
        // Single：单输出 (1,C,M)，C>=5（前 4 通道 xywh + 类别，如 yolo261n (1,84,8400)）
        if (oi.dims.size() >= 3 && oi.dims[1] >= 5) {
            return DecodeType::kSingle;
        }
        return DecodeType::kUnknown;
    }
    // 多输出 reg-bins DFL（大腕256：3 尺度 × [reg(1,64,H,W), cls(1,C,H,W), aux(1,1,H,W)]）
    // 严格条件避免误入成对 DFL（黄瓦 box[1,1,4,M] 的 dims[1]=1 < 32）
    if (info.n_outputs >= 6 && info.n_outputs % 3 == 0) {
        bool dfl_dist = true;
        for (uint32_t i = 0; i + 2 < info.n_outputs; i += 3) {
            const auto& ro = info.outputs[i];      // reg
            const auto& co = info.outputs[i + 1];  // cls
            const auto& ao = info.outputs[i + 2];  // aux
            if (ro.dims.size() != 4 || co.dims.size() != 4 || ao.dims.size() != 4 ||
                ro.dims[1] < 32 || ro.dims[1] % 16 != 0 ||
                ao.dims[1] != 1 || co.dims[1] == 0) {
                dfl_dist = false;
                break;
            }
        }
        if (dfl_dist) return DecodeType::kDflDist;
    }
    // 多输出成对 reg/cls bins（无 aux，如 sjz-XCSH 6 输出：3 尺度 × [reg(1,64,H,W), cls(1,C,H,W)]）
    // 与 DecodeNMS::process 的 pair_reg_cls 分发条件对齐：reg dims[1]>=32 且 %4==0，
    // reg/cls 网格 H/W 一致；先于「成对 box/cls DFL」判断，避免误入 kDfl。
    if (info.n_outputs >= 6 && info.n_outputs % 2 == 0) {
        bool pair_reg_cls = true;
        for (uint32_t i = 0; i + 1 < info.n_outputs; i += 2) {
            const auto& ro = info.outputs[i];
            const auto& co = info.outputs[i + 1];
            if (ro.dims.size() != 4 || co.dims.size() != 4 ||
                ro.dims[1] < 32 || ro.dims[1] % 4 != 0 || co.dims[1] == 0 ||
                ro.dims[2] != co.dims[2] || ro.dims[3] != co.dims[3]) {
                pair_reg_cls = false;
                break;
            }
        }
        if (pair_reg_cls) return DecodeType::kDflPairDist;
    }
    // 多输出成对 box/cls（DFL，如黄瓦 6 输出）
    if (info.n_outputs >= 2 && info.n_outputs % 2 == 0) {
        return DecodeType::kDfl;
    }
    return DecodeType::kUnknown;
}

// 按解码类型从对应输出张量推导类别数；E2E / 未知返回 0（由用户配置明确）
uint32_t ModelAdapter::infer_class_count(const RknnModelInfo& info, DecodeType t) {
    switch (t) {
        case DecodeType::kSingle: {
            const uint32_t c = info.outputs[0].dims[1];
            // 带 objectness：C-4==81（4 xywh + 1 objectness）→ 减 5；否则减 4
            return (c > 5 && c - 4 == 81) ? c - 5 : (c > 4 ? c - 4 : 0);
        }
        case DecodeType::kDfl:
        case DecodeType::kDflPairDist: {
            // cls 输出 [1,C,H,W]，类别数 = dims[1]（成对：cls 在 i+1）
            for (uint32_t i = 0; i + 1 < info.n_outputs; i += 2) {
                const auto& co = info.outputs[i + 1];
                if (co.dims.size() == 4 && co.dims[1] > 0) {
                    return co.dims[1];
                }
            }
            return 0;
        }
        case DecodeType::kDflDist: {
            // 3 组一组：第 2 个输出是 cls [1,C,H,W]
            for (uint32_t i = 1; i < info.n_outputs; i += 3) {
                const auto& co = info.outputs[i];
                if (co.dims.size() == 4 && co.dims[1] > 0) {
                    return co.dims[1];
                }
            }
            return 0;
        }
        case DecodeType::kE2e:
            // E2E 输出 [1,N,F] 不含显式类别数；class 列为值 → 0=未知（由用户/config 明确）
            return 0;
        default:
            return 0;
    }
}

// 解码类型 → 可读名称（仅日志/诊断用）
const char* ModelAdapter::decode_type_name(DecodeType t) {
    switch (t) {
        case DecodeType::kSingle: return "single";
        case DecodeType::kDfl: return "dfl";
        case DecodeType::kE2e: return "e2e";
        case DecodeType::kDflDist: return "dfl_dist";
        case DecodeType::kDflPairDist: return "dfl_pair_dist";
        default: return "unknown";
    }
}

// ---------------------------------------------------------------------------
// analyze：rknn_query 结果 + 用户配置 → 完整 ModelMetadata
// ---------------------------------------------------------------------------

// 由运行时模型信息 + 用户配置构建完整 ModelMetadata；解码类型无法推断时返回 false
bool ModelAdapter::analyze(const RknnModelInfo& info, const ModelAdapterConfig& cfg,
                           std::string* error) {
    if (info.n_inputs < 1 || info.outputs.empty()) {
        if (error) *error = "模型信息无效（无输入或无输出）";
        return false;
    }
    cfg_ = cfg;
    ModelMetadata m;

    // ---- 输入 ----
    m.input_width = info.input_width;
    m.input_height = info.input_height;
    // 输入通道：dims 布局 NCHW=[1,C,H,W] / NHWC=[1,H,W,C]
    if (info.input_fmt == 1 && info.input_dims.size() >= 4) {          // NHWC
        m.input_channels = info.input_dims[3];
    } else if (info.input_dims.size() >= 4) {                          // NCHW
        m.input_channels = info.input_dims[1];
    }
    m.input_dtype = info.input_type;
    m.input_layout = info.input_fmt;
    m.input_size = info.input_size;
    m.quantization_type =
        (info.input_type == 2) ? QuantType::kInt8 :
        (info.input_type == 3) ? QuantType::kUint8 : QuantType::kNone;
    m.color_order = cfg_.color_order;

    // ---- 输出 ----
    m.output_count = info.n_outputs;
    for (const auto& oi : info.outputs) {
        m.output_dtypes.push_back(oi.type);
        m.output_layouts.push_back(oi.fmt);
        m.output_shapes.push_back(oi.dims);
    }

    // ---- 解码推断 ----
    m.decode_type = infer_decode_type(info);
    if (m.decode_type == DecodeType::kUnknown) {
        if (error) *error = "无法推断模型解码类型（输出结构不支持）";
        return false;
    }
    m.class_count = infer_class_count(info, m.decode_type);
    m.objectness = false;
    m.dfl = false;
    m.nms_type = NmsType::kClasswise;
    m.coordinate_format = CoordFormat::kXywh;
    switch (m.decode_type) {
        case DecodeType::kSingle: {
            const uint32_t c = info.outputs[0].dims[1];
            m.objectness = (c > 4 && c - 4 == 81);
            m.coordinate_format = CoordFormat::kXywh;
            break;
        }
        case DecodeType::kDfl: {
            m.dfl = true;
            m.coordinate_format = CoordFormat::kLtrb;
            // strides：每对 box[1,1,4,M]（M=grid²），stride = input_h / grid
            for (uint32_t i = 0; i + 1 < info.n_outputs; i += 2) {
                const auto& bo = info.outputs[i];
                if (bo.dims.size() == 4) {
                    const uint32_t m_anchors = bo.dims[3];
                    const uint32_t grid = static_cast<uint32_t>(std::sqrt(m_anchors));
                    if (grid > 0) m.strides.push_back(m.input_height / grid);
                }
            }
            break;
        }
        case DecodeType::kDflDist: {
            m.dfl = true;
            m.coordinate_format = CoordFormat::kLtrb;
            // strides：reg [1,64,H,W]，grid = H（方阵 H==W），stride = input_h / grid
            for (uint32_t i = 0; i + 2 < info.n_outputs; i += 3) {
                const auto& ro = info.outputs[i];
                if (ro.dims.size() == 4) {
                    const uint32_t grid = ro.dims[2];
                    if (grid > 0) m.strides.push_back(m.input_height / grid);
                }
            }
            break;
        }
        case DecodeType::kDflPairDist: {
            m.dfl = true;
            m.coordinate_format = CoordFormat::kLtrb;
            // strides：成对 reg [1,64,H,W]（无 aux），grid = H，stride = input_h / grid
            for (uint32_t i = 0; i + 1 < info.n_outputs; i += 2) {
                const auto& ro = info.outputs[i];
                if (ro.dims.size() == 4) {
                    const uint32_t grid = ro.dims[2];
                    if (grid > 0) m.strides.push_back(m.input_height / grid);
                }
            }
            break;
        }
        case DecodeType::kE2e: {
            m.coordinate_format = CoordFormat::kXyxy;
            break;
        }
        default:
            break;
    }

    // ---- 默认阈值 ----
    m.default_conf = 0.25f;
    m.default_iou = 0.45f;

    m.label = info.outputs[0].dims.size() >= 3
                  ? "model" + std::to_string(info.outputs[0].dims[0]) + "x" +
                        std::to_string(info.outputs[0].dims[1])
                  : "model";
    metadata_ = m;

    TTBOX_LOG_INFO("ModelAdapter analyze: " + std::to_string(m.input_width) + "x" +
                   std::to_string(m.input_height) + "x" + std::to_string(m.input_channels) +
                   " in_dtype=" + std::to_string(m.input_dtype) +
                   " in_fmt=" + std::to_string(m.input_layout) +
                   " color=" + std::string(m.color_order == ColorOrder::kRgb ? "RGB" : "BGR") +
                   " quant=" + std::to_string(static_cast<int>(m.quantization_type)) +
                   " | out=" + std::to_string(m.output_count) +
                   " decode=" + decode_type_name(m.decode_type) +
                   " classes=" + std::to_string(m.class_count) +
                   " strides=" + [&] {
                       std::string s;
                       for (auto v : m.strides) { s += std::to_string(v) + ","; }
                       return s;
                   }());
    return true;
}

// ---------------------------------------------------------------------------
// create_decoder：依据 metadata + 用户配置注入参数
// ---------------------------------------------------------------------------

// 依据 metadata 与用户配置注入参数并构造解码器；配置失败返回 nullptr
std::unique_ptr<Decoder> ModelAdapter::create_decoder(std::string* error) const {
    DecodeParams dp;
    dp.conf_thres = effective_conf();
    dp.iou_thres = effective_iou();
    dp.classwise = (metadata_.nms_type == NmsType::kClasswise);
    dp.input_w = cfg_.input_width_override > 0 ? cfg_.input_width_override : metadata_.input_width;
    dp.input_h = cfg_.input_height_override > 0 ? cfg_.input_height_override : metadata_.input_height;
    dp.frame_w = 0;  // 由调用方（worker）按原图设置
    dp.frame_h = 0;
    dp.class_filter = cfg_.class_filter;
    dp.max_detections = cfg_.max_detections;
    // A-8：E2E 模型内部已 TopK/NMS → 跳过无条件二次 NMS（v26m 类）
    dp.e2e_skip_nms = (metadata_.decode_type == DecodeType::kE2e);

    auto decoder = std::make_unique<DecoderImpl>();
    if (!decoder->configure(dp, error)) {
        return nullptr;
    }
    return decoder;
}

}  // namespace ttbox::core
