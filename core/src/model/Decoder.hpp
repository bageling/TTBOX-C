// Decoder.hpp — 解码器统一接口（阶段 A-7）
//
// 目标：把"YOLO 固定实现"抽象为 Decoder 接口，由 ModelAdapter 依据
// ModelMetadata.decode_type 创建对应解码器；新增模型只加 Decoder/Metadata，
// 不修改高速底座。
//
// 现有实现（委托 DecodeNMS，其 process() 已按输出结构自动分发）：
//   - Single Decoder（yolo261n: (1,C,M) xywh+cls）
//   - DFL Decoder（黄瓦: 成对 box/cls 距离解码）
//   - E2E Decoder（v26m: [1,N,F] 已解码 xyxy+score+class）
//
// 禁止按模型文件名分发；解码语义完全由 ModelMetadata 决定。
#pragma once

#include <string>
#include <vector>

#include "common/Types.hpp"
// 契约类型（RknnModelInfo / InferenceProfile / FovProfile）已下沉到 common/ 叶子层
//   （common/RknnContract.hpp、common/RuntimeProfiles.hpp），见各头说明。
#include "common/RknnContract.hpp"
// DecoderImpl 以 DecodeNMS 为成员（值持有），所以本头仍需 DecodeNMS 的完整定义。
//   把 impl 拆到另一个头（纯接口与实现分离）是后续可选的依赖面优化，不在本次范围。
#include "rknn/DecodeNMS.hpp"

namespace ttbox::core {

// 解码器统一接口（每个 worker 独立实例；线程安全 = 无共享可变状态）
class Decoder {
public:
    virtual ~Decoder() = default;

    // 处理一帧原生 RKNN 输出（want_float=0 预分配 buffer）
    virtual bool process(const RknnModelInfo& info, const void* const* out_bufs,
                         std::vector<DetectionBox>* detections,
                         std::string* error = nullptr) = 0;
    // A-7：运行期更新原图坐标映射（不覆盖 conf/iou/filter 等参数）
    virtual void set_frame(uint32_t frame_w, uint32_t frame_h) = 0;
    // A-8：热更新运行时参数（conf/iou/class_filter/max_detections/FOV）
    virtual void apply_runtime(const InferenceProfile& inf, const FovProfile& fov) = 0;
    // A-8：安全点热更新 ROI（与 RGA ROI 一致）
    virtual void set_roi(uint32_t roi_x, uint32_t roi_y, uint32_t roi_w, uint32_t roi_h) = 0;
    virtual const DecodeStats& stats() const = 0;
    virtual void reset_stats() = 0;
};

// 通用实现：包装 DecodeNMS（其内部按输出结构自动选择 single/dfl/e2e 路径）。
// 配置（conf/iou/class_filter/max_detections/输入尺寸）由 ModelAdapter 从
// ModelMetadata + 用户配置注入，不写死任何数值。
class DecoderImpl : public Decoder {
public:
    // 配置底层 DecodeNMS（注入 conf/iou/类别过滤/输入尺寸），失败经 error 回传原因
    bool configure(const DecodeParams& params, std::string* error = nullptr) {
        return impl_.configure(params, error);
    }
    bool process(const RknnModelInfo& info, const void* const* out_bufs,
                 std::vector<DetectionBox>* detections,
                 std::string* error = nullptr) override {
        return impl_.process(info, out_bufs, detections, error);
    }
    void set_frame(uint32_t frame_w, uint32_t frame_h) override {
        impl_.set_frame(frame_w, frame_h);
    }
    void apply_runtime(const InferenceProfile& inf, const FovProfile& fov) override {
        impl_.apply_runtime(inf, fov);
    }
    void set_roi(uint32_t roi_x, uint32_t roi_y, uint32_t roi_w, uint32_t roi_h) override {
        impl_.set_roi(roi_x, roi_y, roi_w, roi_h);
    }
    const DecodeStats& stats() const override { return impl_.stats(); }
    void reset_stats() override { impl_.reset_stats(); }

private:
    DecodeNMS impl_;
};

}  // namespace ttbox::core
