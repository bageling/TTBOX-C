// CoreContracts.hpp — TTBOX Core 稳定数据契约
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "common/Types.hpp"

namespace ttbox::core {

// Frame 是 FrameBuffer 的短别名（契约层的惯用名）。
using Frame = FrameBuffer;

// 检测结果（Core 内部契约类型；含所属帧号与采集时间戳）。
struct Detection {
    int class_id = 0;
    float confidence = 0.0f;
    float x1 = 0.0f;
    float y1 = 0.0f;
    float x2 = 0.0f;
    float y2 = 0.0f;
    uint64_t frame_number = 0;
    uint64_t timestamp_us = 0;

    float width() const { return x2 - x1; }
    float height() const { return y2 - y1; }
};

// 一次鼠标移动指令（相对位移 + 所属帧与有效性标记）。
struct MouseCommand {
    int32_t dx = 0;
    int32_t dy = 0;
    uint64_t frame_number = 0;
    uint64_t timestamp_us = 0;
    bool valid = false;
};

// DetectionBox（解码层）→ Detection（契约层），可附带帧号与采集时间戳。
inline Detection to_detection(const DetectionBox& box, uint64_t frame_number = 0,
                              uint64_t timestamp_us = 0) {
    return {box.class_id, box.score, box.x1, box.y1, box.x2, box.y2,
            frame_number, timestamp_us};
}

// ★ 2026-10-04 代码体检：本函数**生产代码零引用**（全仓只有这一定义处），
//   两个测试也没用它。属预留便捷函数，保留不删（删除是业主的决定）。
//   记在这里是为了下次「为什么找不到调用点」时不必再查一遍。
inline DetectionBox to_detection_box(const Detection& detection) {
    return {detection.x1, detection.y1, detection.x2, detection.y2,
            detection.confidence, detection.class_id};
}

}  // namespace ttbox::core
