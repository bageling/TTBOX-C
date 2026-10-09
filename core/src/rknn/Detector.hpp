// Detector.hpp — RKNN + 解码统一检测边界
#pragma once
#include <cstdint>
#include <memory>
#include <string>
#include <vector>
#include "common/CoreContracts.hpp"
#include "detector/IDetector.hpp"
#include "model/Decoder.hpp"
#include "model/ModelAdapter.hpp"
#include "rknn/Preprocess.hpp"
#include "rknn/RKNNEngine.hpp"

namespace ttbox::core {
// DetectorResult — 一次检测的输出结果（推理耗时 + 检测框列表）
struct DetectorResult {
    bool ok = false;
    double inference_ms = 0.0;
    std::vector<Detection> detections;
};

// Detector — 检测器：把"预处理好的帧 → NPU 推理 → 解码"打包成一个统一入口。
// 输入：PreprocessedFrame（模型输入尺寸的图像）
// 输出：DetectorResult（Detection 列表，原图坐标）
//
// ★ 接线状态：**未接线**（2026-10-06 全仓核实）。注释原先写的"谁用：HardwareRunner /
//   imgdetect"**与事实不符**——两者都没有引用本类。实际推理链路是
//   rknn::WorkerPool 直接持有 RKNNEngine + DecodeNMS 完成推理与解码，不经过本类。
//   本类作为 IDetector 的参考实现保留（未来接第二后端时用），当前不参与生产路径，
//   改动它不会影响运行时行为。
class Detector : public detector::IDetector {
public:
    bool init(const std::string& model_path, int core_mask, bool pass_through,
              float conf_thres, float iou_thres, uint32_t frame_w, uint32_t frame_h,
              int color_order, std::string* error = nullptr,
              const ModelAdapter* adapter = nullptr);
    bool detect(const PreprocessedFrame& frame, DetectorResult* result,
                std::string* error = nullptr);
    bool detect(const Frame& frame, std::vector<Detection>* detections,
                std::string* error = nullptr) override;
    const DecodeStats& stats() const { return decoder_->stats(); }
    const RknnModelInfo& model_info() const { return engine_->info(); }
    bool initialized() const { return engine_ && engine_->initialized(); }
private:
    std::unique_ptr<RKNNEngine> engine_;
    std::unique_ptr<Decoder> decoder_;
    std::vector<std::vector<uint8_t>> raw_outputs_;
    std::vector<void*> raw_ptrs_;
    std::vector<size_t> raw_sizes_;
};
}
