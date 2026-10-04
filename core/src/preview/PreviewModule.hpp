// PreviewModule.hpp — 低帧预览（裁剪范围跟随 RuntimeProfile::capture 的截取尺寸）
#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "capture/V4L2Capture.hpp"
#include "common/Types.hpp"
#include "model/RuntimeProfile.hpp"
#include "preview/PreviewRoi.hpp"

namespace ttbox::core {

class PreviewModule {
public:
    struct Params {
        // 预览**输出上限**（来自 preview.width/height）。裁剪范围由 RuntimeProfile::capture
        // 的截取尺寸决定（见 preview/PreviewRoi.hpp），这里只用来限制 JPEG 体量与编码耗时。
        uint32_t crop_width = 640;
        uint32_t crop_height = 640;
        int fps = 15;
        int jpeg_quality = 70;
        RuntimeConfig* runtime_config = nullptr;
        bool draw_detections = false;
        // ★ M2.03：受限预览水印（由 Application 依 LicenseGate 快照计算并写入；本模块**只执行参数，
        //   不查询授权**）。watermark_text 全 ASCII（内嵌 5×7 位图字体可绘），形如 "<BRAND> - LIMITED"。
        bool watermark = false;
        std::string watermark_text;
    };

    PreviewModule() = default;
    ~PreviewModule() { stop(); }
    PreviewModule(const PreviewModule&) = delete;
    PreviewModule& operator=(const PreviewModule&) = delete;

    bool start(const LatestFrame* frame_source, const Params& params, std::string* error = nullptr);
    void stop();
    bool running() const { return running_.load(); }
    bool snapshot(std::vector<uint8_t>* jpeg_out) const;

    // ★★ V1.0.25（2026-10-04，业主定口径）：**预览画的就是实际瞄准的那一个框**。
    //   旧实现画的是本帧**全部**检测框（红=class0 / 绿=class1 加粗、假装是"头" / 黄=其余），
    //   两个框叠在一起看着像"又大又含头" —— 那是**候选框集合**，不是瞄准框。
    //   预览与控制必须是同一个几何，否则画面在骗人（业主原话：「预览和实际瞄准框应该一致」）。
    //   ⇒ provider 只给**一个**框；返回 false = 本帧没有选中目标 ⇒ 画面上不画框
    //     （旧逻辑会在丢失后继续保留 3 帧旧框 = 画出已经不瞄的框，同样是骗人）。
    using AimBoxProvider = std::function<bool(DetectionBox*)>;
    void set_aim_box_provider(AimBoxProvider provider) {
        std::lock_guard<std::mutex> lock(provider_mutex_);
        aim_box_provider_ = std::move(provider);
    }

    struct Metrics {
        std::atomic<uint64_t> frames{0};
        std::atomic<uint64_t> dropped{0};
        std::atomic<double> fps{0.0};
        std::atomic<double> encode_ms{0.0};
        std::atomic<uint32_t> width{0};
        std::atomic<uint32_t> height{0};
        std::atomic<uint32_t> bytes{0};
    };
    const Metrics& metrics() const { return metrics_; }

private:
    void loop();
    bool encode_frame(const FrameBuffer& frame, std::vector<uint8_t>* jpeg_out,
                      std::string* error);
    // ★ V1.0.25：画**唯一一个**框 = 控制链这一帧实际在用的那个（醒目亮绿 + 类别号）。
    //   刻意**不再按 class_id 上色**：旧的「class 1 加粗当头」是出厂 2 类模型的老约定，
    //   板端现役是 7 类模型（class_names 为空）⇒ 那个映射在瞎猜，画出来会误导人。
    //   也不再二次平滑：传进来的框已由 AimThread 的 One-Euro 平滑过，再过一道
    //   alpha=0.35 的低通只会更滞后、离控制链更远。
    void draw_aim_box(uint8_t* crop, uint32_t width, uint32_t height, uint32_t stride,
                      const DetectionBox& box, uint32_t origin_x, uint32_t origin_y) const;
    // ★ M2.03：受限态水印绘制（内嵌 5×7 ASCII 位图字体；纯数组写入，**never block 帧输出**）。
    void draw_watermark(uint8_t* crop, uint32_t width, uint32_t height,
                        uint32_t stride) const;
    // 依据 RuntimeProfile::capture（截取尺寸 + 偏移）算出预览裁剪矩形与输出尺寸。
    // 纯计算在 preview/PreviewRoi.hpp（host 可单测），这里只负责取 profile 快照。
    void resolve_preview_geometry(uint32_t frame_w, uint32_t frame_h, PreviewRoi* roi,
                                  uint32_t* out_width, uint32_t* out_height) const;

    const LatestFrame* latest_ = nullptr;
    Params params_{};
    std::atomic<bool> running_{false};
    std::thread thread_;

    mutable std::mutex provider_mutex_;
    AimBoxProvider aim_box_provider_;

    // 预览线程独占，按尺寸复用，避免每帧重复申请 640×640×3 临时缓冲。
    std::vector<uint8_t> crop_buffer_;
    // 仅在 ROI 超过输出上限（如全帧中心正方形 1440）时用于等比缩小，同样按尺寸复用。
    std::vector<uint8_t> scaled_buffer_;

    mutable std::mutex jpeg_mutex_;
    std::vector<uint8_t> jpeg_;

    Metrics metrics_;
};

}  // namespace ttbox::core
