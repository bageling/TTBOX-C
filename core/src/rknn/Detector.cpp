// Detector.cpp — RKNN 推理与解码的统一边界
#include "rknn/Detector.hpp"

#include <chrono>

namespace ttbox::core {

// 初始化：加载模型并按适配器创建解码器，预分配原生输出缓冲。
bool Detector::init(const std::string& model_path, int core_mask, bool pass_through,
                   float conf_thres, float iou_thres, uint32_t frame_w, uint32_t frame_h,
                   int color_order, std::string* error, const ModelAdapter* adapter) {
    if (model_path.empty()) {
        if (error) *error = "ActiveModel 模型路径为空";
        return false;
    }
    engine_ = std::make_unique<RKNNEngine>();
    RKNNEngine::Params params;
    params.model_path = model_path;
    params.core_mask = core_mask;
    params.pass_through = pass_through;
    if (!engine_->init(params, error)) {
        engine_.reset();
        return false;
    }

    ModelAdapter local_adapter;
    const ModelAdapter* effective_adapter = adapter;
    if (!effective_adapter) {
        ModelAdapterConfig config;
        config.color_order = color_order == 0 ? ColorOrder::kBgr : ColorOrder::kRgb;
        config.conf_thres = conf_thres;
        config.iou_thres = iou_thres;
        if (!local_adapter.analyze(engine_->info(), config, error)) {
            engine_.reset();
            return false;
        }
        effective_adapter = &local_adapter;
    }
    decoder_ = effective_adapter->create_decoder(error);
    if (!decoder_) {
        engine_.reset();
        return false;
    }
    decoder_->set_frame(frame_w, frame_h);
    raw_outputs_.clear();
    raw_ptrs_.clear();
    raw_sizes_.clear();
    for (const auto& info : engine_->info().outputs) {
        raw_outputs_.emplace_back(info.size, 0);
        raw_ptrs_.push_back(raw_outputs_.back().data());
        raw_sizes_.push_back(info.size);
    }
    return true;
}

// 对已预处理帧执行「推理 + 解码」，输出检测结果与耗时。
bool Detector::detect(const PreprocessedFrame& frame, DetectorResult* result,
                      std::string* error) {
    if (!initialized()) {
        if (error) *error = "Detector 未初始化";
        return false;
    }
    if (!result) {
        if (error) *error = "Detector 输出为空";
        return false;
    }
    *result = {};
    const void* input = frame.tensor_data ? frame.tensor_data : frame.data;
    const size_t input_size = frame.tensor_size ? frame.tensor_size : frame.size;
    if (!input || input_size == 0) {
        if (error) *error = "PreprocessedFrame 没有有效 tensor";
        return false;
    }
    const auto begin = std::chrono::steady_clock::now();
    std::vector<DetectionBox> boxes;
    if (!engine_->set_input(input, input_size, error) || !engine_->run(error) ||
        !engine_->get_raw_outputs(raw_ptrs_.data(), raw_sizes_.data(), error) ||
        !decoder_->process(engine_->info(), raw_ptrs_.data(), &boxes, error)) {
        return false;
    }
    result->detections.clear();
    result->detections.reserve(boxes.size());
    for (const auto& box : boxes) result->detections.push_back(to_detection(box));
    result->inference_ms = std::chrono::duration<double, std::milli>(
        std::chrono::steady_clock::now() - begin).count();
    result->ok = true;
    return true;
}

// 对原始帧执行「推理 + 解码」，输出 Detection 列表（补齐帧号/时间戳）。
bool Detector::detect(const Frame& frame, std::vector<Detection>* detections,
                      std::string* error) {
    if (!detections) {
        if (error) *error = "Detection 输出为空";
        return false;
    }
    PreprocessedFrame prepared;
    prepared.source = frame.info;
    prepared.detect_width = frame.info.width;
    prepared.detect_height = frame.info.height;
    prepared.format = frame.info.format;
    prepared.data = frame.info.cpu_va ? frame.info.cpu_va : frame.data.get();
    prepared.size = frame.size;
    DetectorResult result;
    if (!detect(prepared, &result, error)) return false;
    detections->clear();
    detections->reserve(result.detections.size());
    for (const auto& detection : result.detections) {
        auto converted = detection;
        converted.frame_number = frame.info.frame_number != 0 ? frame.info.frame_number : frame.info.sequence;
        converted.timestamp_us = frame.info.timestamp_us != 0
            ? frame.info.timestamp_us
            : static_cast<uint64_t>(frame.info.timestamp_ms * 1000.0);
        detections->push_back(converted);
    }
    return true;
}  // namespace ttbox::core
