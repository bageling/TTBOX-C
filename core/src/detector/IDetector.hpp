// IDetector.hpp — Core 识别边界
//
// ★ 接线状态：**未接线**（2026-10-06 全仓核实）。本接口只有一个实现
//   rknn/Detector.hpp，而那个实现**从未被实例化**；实际推理链路是
//   rknn::WorkerPool 直接持有 decoder_ + engine_ 调 DecodeNMS，不经过本接口。
//   保留它是为了将来接第二种检测后端（ONNX 等）。在那之前请不要假设
//   "WorkerPool 走的是 IDetector*" —— 它不是，加载新后端也不必改 WorkerPool。
#pragma once

#include <string>
#include <vector>
#include "common/CoreContracts.hpp"

namespace ttbox::core::detector {

class IDetector {
public:
    virtual ~IDetector() = default;
    // 对一帧执行检测并输出 Detection 列表；失败返回 false 并填 error
    virtual bool detect(const Frame& frame, std::vector<Detection>* detections,
                        std::string* error = nullptr) = 0;
};

}  // namespace ttbox::core::detector
