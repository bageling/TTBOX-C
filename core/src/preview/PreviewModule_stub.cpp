// PreviewModule_stub.cpp — 无 libjpeg / 无 OpenCV 构建时的 PreviewModule 空实现（T1.18）
// 目的：当 (JPEG_FOUND AND OpenCV_FOUND)==FALSE（构建环境缺 jpeg 或缺 opencv）时，
//       仍提供 PreviewModule 的【全部外部符号】（start/stop/snapshot），使 ttbox_core /
//       ttbox_core_main 正常链接；预览功能整体禁用（running()==false，snapshot() 返回 false）。
// 约束：本文件必须与 PreviewModule.hpp 的声明**逐符号一致**，
//       且**不得** include <jpeglib.h> / <opencv2/*>（这正是它存在的意义）。
// 权威文本：libjpeg-linkage-ruling.md §4.1（架构师裁决 T1.18）。
#include "preview/PreviewModule.hpp"

#include "common/Logger.hpp"   // 与 PreviewModule.cpp 同款日志宏 TTBOX_LOG_WARN

namespace ttbox::core {

// 存根：返回 false 并把"预览未编译"原因写入 error。
bool PreviewModule::start(const LatestFrame*, const Params&, std::string* error) {
    if (error) {
        *error = "preview disabled: built without libjpeg/OpenCV "
                 "(JPEG_FOUND AND OpenCV_FOUND == FALSE)";
    }
    TTBOX_LOG_WARN("Preview 未编译（缺 libjpeg 或 OpenCV）：预览功能整体禁用");
    return false;
}

// 存根：空实现（无预览线程可停）。
void PreviewModule::stop() {}

// 存根：恒返回 false（无可用 JPEG）。
bool PreviewModule::snapshot(std::vector<uint8_t>*) const { return false; }

}  // namespace ttbox::core
