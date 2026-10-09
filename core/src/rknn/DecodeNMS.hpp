// DecodeNMS.hpp — C++ 检测解码 + NMS（阶段 A-6）
//
// 目标：
//   - 直接消费 RKNN want_float=0 原生输出（FP16/INT8/FLOAT32），禁止无意义 float 转换
//   - 与 Python Demo decode_outputs() 单输出分支结果对齐（yolo261n: 单输出 (1,84,8400)）
//   - 坐标解码/置信度/类别/坐标映射（模型输入空间 → 原图）
//   - classwise NMS（conf 过滤 → 每类别 NMS），阈值来自 config（conf/nms）
//   - 不复制整张图像，只处理 RKNN 输出 tensor
//   - 线程：每个 worker 持独立实例；内部无共享可变状态，线程安全
//   - 统计：decode / nms / total（min/p50/avg/p95/p99/max）+ 候选/最终目标计数
//
// 边界：只做"解码 + NMS + 坐标映射"，不含 Aim/HID（A-7+）。
#pragma once

#include <atomic>
#include <cstdint>
#include <string>
#include <vector>

#include "common/Stats.hpp"
#include "common/Types.hpp"
// ★ 2026-10-06 架构整理：本文件原本 include "model/RuntimeProfile.hpp" 与
//   "rknn/RKNNEngine.hpp"，但实际只用到 InferenceProfile / FovProfile 与
//   RknnModelInfo / RknnOutputInfo 四个**契约类型**，从不碰 RKNNEngine 类。
//   契约已下沉到 common/ 叶子头 ⇒ ① rknn 不再反向依赖 model，model ↔ rknn 的模块环
//   由此切断；② 解码器也不再把整个 NPU 引擎头拖进来（缩短编译依赖链）。
#include "common/RuntimeProfiles.hpp"
#include "common/RknnContract.hpp"

namespace ttbox::core {

// 解码参数（阈值/尺寸来自 config，不硬编码）
struct DecodeParams {
    float conf_thres = 0.25f;   // config: conf
    float iou_thres = 0.45f;    // config: nms
    bool classwise = true;      // 按类别 NMS（Python 默认）
    uint32_t input_w = 640;     // 模型输入宽（坐标在输入空间）
    uint32_t input_h = 640;
    uint32_t frame_w = 0;       // 原图宽（坐标映射目标；0=不映射）
    uint32_t frame_h = 0;
    // A-7 用户自定义：类别过滤（空=全部保留）与最大检测数（0=不限）
    std::vector<int> class_filter;
    int max_detections = 0;
    // A-8 FOV Filter（NMS 之后应用；默认关闭）
    bool fov_enabled = false;
    FovShape fov_shape = FovShape::kCircle;  // 0=circle 1=rect
    float fov_radius = 0.5f;     // circle: 归一化半径；rect: 归一化半宽半高
    float fov_center_x = 0.5f;   // 归一化 0~1（相对原图）
    float fov_center_y = 0.5f;
    // A-8 ROI（Capture ROI ≠ 模型输入尺寸）
    //   RGA 裁剪屏幕 ROI(roi_x,roi_y,roi_w,roi_h) 后缩放至模型输入。
    //   roi_w/h == 0 表示未启用 ROI（使用全帧映射）。
    uint32_t roi_x = 0, roi_y = 0, roi_w = 0, roi_h = 0;
    // A-8 E2E：模型已内部 TopK/NMS 时跳过无条件二次 NMS（v26m 类）
    bool e2e_skip_nms = true;
};

// 解码统计（跨线程安全；候选/目标为累计计数）
struct DecodeStats {
    StatsCollector decode;      // 候选解码耗时（us）
    StatsCollector nms;         // NMS 耗时（us）
    StatsCollector total;       // decode+nms 总耗时（us）
    std::atomic<uint64_t> candidates{0};  // conf 过滤后进入 NMS 的候选数（累计）
    std::atomic<uint64_t> detections{0};  // NMS 后最终目标数（累计）
    std::atomic<uint64_t> frames{0};      // 处理帧数
    void reset() {
        decode.clear(); nms.clear(); total.clear();
        candidates.store(0); detections.store(0); frames.store(0);
    }
};

// DecodeNMS — 检测解码 + NMS 去重：把 RKNN 原始输出翻译成检测框(DetectionBox)。
// 输入：模型输出张量(原生类型，按 zp/scale 反量化)
// 输出：DetectionBox 列表（坐标已映射回原图 frame_w/frame_h 坐标系）
// 支持：kSingle(单输出) / kDfl(成对) / kDflDist(3输出组) / kE2e 四种布局
// 谁用：Worker(实时链路) / imgdetect(单图验证) / Detector(统一边界)
class DecodeNMS {
public:
    // 配置解码参数（阈值/输入尺寸等），做基本校验。
    bool configure(const DecodeParams& params, std::string* error = nullptr);

    // A-7：运行期更新原图坐标映射（不覆盖 conf/iou/filter 等已配置参数）
    void set_frame(uint32_t frame_w, uint32_t frame_h) {
        params_.frame_w = frame_w;
        params_.frame_h = frame_h;
    }

    // A-8：热更新运行时参数（conf/iou/class_filter/max_detections/FOV）。
    // 只更新调用方传入字段，不覆盖其他配置（输入尺寸/坐标映射等）。
    void apply_runtime(const InferenceProfile& inf, const FovProfile& fov) {
        if (inf.confidence > 0.0f) params_.conf_thres = inf.confidence;
        if (inf.iou > 0.0f) params_.iou_thres = inf.iou;
        if (!inf.class_filter.empty()) params_.class_filter = inf.class_filter;
        if (inf.max_detections > 0) params_.max_detections = inf.max_detections;
        params_.fov_enabled = fov.enabled;
        params_.fov_shape = fov.shape;
        params_.fov_radius = fov.radius;
        params_.fov_center_x = fov.center_x;
        params_.fov_center_y = fov.center_y;
    }

    // A-8：安全点热更新 ROI（与 RGA ROI 保持一致；worker 单线程调用）
    void set_roi(uint32_t roi_x, uint32_t roi_y, uint32_t roi_w, uint32_t roi_h) {
        params_.roi_x = roi_x;
        params_.roi_y = roi_y;
        params_.roi_w = roi_w;
        params_.roi_h = roi_h;
    }

    const DecodeParams& params() const { return params_; }
    const DecodeStats& stats() const { return stats_; }
    void reset_stats() { stats_.reset(); }

    // 处理一帧原生 RKNN 输出（want_float=0 的预分配 buffer）。
    // 调用方保证 out_bufs 在 process 返回前有效（禁止提前释放）。
    // 兼容单输出 (1,C,M)（yolo261n：前 4 通道 xywh + 类别）与
    // 带 objectness 的单输出（C-4==81）。多输出 DFL 返回错误（本阶段不支持）。
    bool process(const RknnModelInfo& info,
                 const void* const* out_bufs,
                 std::vector<DetectionBox>* detections,
                 std::string* error = nullptr);

private:
    static float read_elem(const RknnOutputInfo& oi, const uint8_t* buf, size_t idx);
    static float half_to_float(uint16_t h);
    void nms_boxes(const std::vector<DetectionBox>& cands,
                   std::vector<int>* keep) const;
    // 单输出格式（yolo261n: (1,C,M) 前 4 通道 xywh + 类别）
    bool process_single(const RknnModelInfo& info, const void* const* out_bufs,
                        std::vector<DetectionBox>* detections, std::string* error);
    // 多输出 DFL（黄瓦模型：3 尺度 × [box(1,1,4,M), cls(1,C,H,W)]）
    bool process_dfl(const RknnModelInfo& info, const void* const* out_bufs,
                     std::vector<DetectionBox>* detections, std::string* error);
    // 多输出 reg-bins DFL（大腕256：3 尺度 × [reg(1,64,H,W), cls(1,C,H,W), aux(1,1,H,W)]）
    bool process_dfl_pair_dist(const RknnModelInfo&, const void* const*, std::vector<DetectionBox>*, std::string*);
    bool process_dfl_dist(const RknnModelInfo& info, const void* const* out_bufs,
                          std::vector<DetectionBox>* detections, std::string* error);
    // 端到端单输出（v26m：模型内已做 TopK 选择，输出 [1,N,F]，F=6: xyxy+score+class）
    bool process_e2e(const RknnModelInfo& info, const void* const* out_bufs,
                     std::vector<DetectionBox>* detections, std::string* error);
    // A-7：输出后处理（class_filter 过滤 + max_detections 截断，按 score 降序）
    void apply_post_filter(std::vector<DetectionBox>* dets) const;
    // A-8：FOV Filter（NMS 之后；按框中心落入 FOV 区域过滤）
    void apply_fov_filter(std::vector<DetectionBox>* dets) const;
    // A-8：坐标映射（含 ROI 偏移：模型输入空间 → ROI 空间 → 原图）
    void map_coords(std::vector<DetectionBox>* dets) const;

    DecodeParams params_;
    DecodeStats stats_;
    // 复用工作 buffer（避免每帧堆分配）
    std::vector<DetectionBox> cands_;
    std::vector<DetectionBox> cands_cls_;
    std::vector<DetectionBox> kept_;
    std::vector<int> keep_idx_;
};

}  // namespace ttbox::core
