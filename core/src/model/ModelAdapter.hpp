// ModelAdapter.hpp — 模型统一适配器（阶段 A-7）
//
// 职责：
//   - analyze(): 从 RKNN runtime 查询结果（RknnModelInfo）+ 用户/运行时配置，
//     构建完整 ModelMetadata（20 项）。输入输出属性全部来自 runtime 查询，
//     解码类型/stride/DFL/objectness 等从输出结构自动推断——禁止按模型名/文件名写逻辑。
//   - create_decoder(): 依据 Metadata 创建解码器（注入用户 conf/iou/class_filter/max_detections）。
//
// 加载生命周期（ModelManager 语义，见 test_model_runtime）：
//   load model → rknn query → analyze → create adapter → create decoder → validate → runtime
#pragma once

#include <memory>
#include <string>

#include "model/Decoder.hpp"
#include "model/ModelMetadata.hpp"
// ★ 2026-10-06 架构整理：本行原来 include "rknn/RKNNEngine.hpp"，只为了拿 RknnModelInfo
//   （analyze/infer_* 的入参），却把整个 NPU 引擎头拖进 model 层。契约类型已下沉到
//   common/RknnContract.hpp，这里改依赖叶子 —— model 不再为读一个结构体而看见 rknn。
#include "common/RknnContract.hpp"

namespace ttbox::core {

// 用户/运行时配置（优先级：Model Default < Runtime Default < User Config）
struct ModelAdapterConfig {
    // 颜色顺序（模型输入要求；来自 config/用户，不允许模型特判）
    ColorOrder color_order = ColorOrder::kBgr;
    // 检测参数（用户可配；最终以用户配置为准，默认值来自 ModelMetadata）
    float conf_thres = 0.0f;        // 0 = 使用 metadata.default_conf
    float iou_thres = 0.0f;         // 0 = 使用 metadata.default_iou
    // 类别过滤（空 = 全部保留）
    std::vector<int> class_filter;
    // 最大检测数（0 = 不限）
    int max_detections = 0;
    // 输入尺寸覆盖（0 = 用模型 dims；一般无需设置）
    uint32_t input_width_override = 0;
    uint32_t input_height_override = 0;
};

// 模型统一适配器（轻量：持有 metadata + 配置，可跨 worker 共享只读）
class ModelAdapter {
public:
    // 构建 metadata：info 来自 RKNNEngine::info()（rknn_query 结果），
    // cfg 来自用户/运行时配置。失败返回 false 并给出原因。
    bool analyze(const RknnModelInfo& info, const ModelAdapterConfig& cfg,
                 std::string* error = nullptr);

    const ModelMetadata& metadata() const { return metadata_; }

    // 创建解码器（每个 worker 独立实例；配置已注入 conf/iou/filter/max/尺寸）。
    // 失败返回 nullptr（error 填充原因）。
    std::unique_ptr<Decoder> create_decoder(std::string* error = nullptr) const;

    // 有效 conf/iou（用户配置优先，否则 metadata 默认）
    float effective_conf() const {
        return cfg_.conf_thres > 0.0f ? cfg_.conf_thres : metadata_.default_conf;
    }
    float effective_iou() const {
        return cfg_.iou_thres > 0.0f ? cfg_.iou_thres : metadata_.default_iou;
    }

    static const char* decode_type_name(DecodeType t);

private:
    // 从输出结构推断 decode_type（禁止文件名/标签判断）
    static DecodeType infer_decode_type(const RknnModelInfo& info);
    static uint32_t infer_class_count(const RknnModelInfo& info, DecodeType t);

    ModelMetadata metadata_;
    ModelAdapterConfig cfg_;
};

}  // namespace ttbox::core
