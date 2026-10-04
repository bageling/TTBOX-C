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

    // ★★ V1.0.31（2026-10-04）：预览按**业界标准三层**绘制（对照 GitHub sunone_aimbot，
    //   1.6k star，其 overlay/debug 窗口就是这三层 + 每样一个开关）：
    //   ① **全部检测框**（细框）—— 看得到 AI 此刻检出了哪些候选、各在哪；
    //   ② **选中的那个框**（粗框）—— 控制链真正在用的那个；
    //   ③ **中心 → 落点的连线**（sunone 的 `show_target_line`）+ FOV 圆
    //      （sunone 的 `circle_capture` / 各类 aimbot 的 `cv2.circle`）。
    //   为什么推翻 V1.0.25 的「只画瞄准框」：那个口径**看不见其它候选** ⇒
    //   业主根本判断不了「它为什么选了这个」⇒ 2026-10-04 业主看到框罩在头盔上
    //   （实际选中了训练场的球）却无从判断，正是这个原因。**调试画面必须透明。**
    //   ★ 框的几何一律用**模型给的原框**，不裁不缩（业界一致；落点在框内由 offset_y 定位）。
    using AimBoxProvider = std::function<bool(DetectionBox*)>;
    void set_aim_box_provider(AimBoxProvider provider) {
        std::lock_guard<std::mutex> lock(provider_mutex_);
        aim_box_provider_ = std::move(provider);
    }

    // ① 全部检测框（本帧 AI 检出的每一个候选，原框）
    using DetectionsProvider = std::function<std::vector<DetectionBox>()>;
    void set_detections_provider(DetectionsProvider provider) {
        std::lock_guard<std::mutex> lock(provider_mutex_);
        detections_provider_ = std::move(provider);
    }

    // ③ 辅助线：中心→落点 的连线端点，以及 FOV 圆半径（像素）。
    //   aim_point 给的是**落点**（ty = 框顶 + offset_y×框高），与 sunone 的
    //   `show_target_line` 同义。fov_radius <= 0 表示不画圆。
    struct AimGuides {
        bool has_aim_point = false;
        float aim_x = 0.0f;
        float aim_y = 0.0f;
        float fov_radius = 0.0f;   // 像素；<=0 不画
        bool fov_circle_crop = false;  // true = 圆心在裁剪区中心（默认）
    };
    using GuidesProvider = std::function<AimGuides()>;
    void set_guides_provider(GuidesProvider provider) {
        std::lock_guard<std::mutex> lock(provider_mutex_);
        guides_provider_ = std::move(provider);
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
    // ① 全部检测框（细框，弱化色，只为看清候选分布，不抢戏）
    void draw_detections_list(uint8_t* crop, uint32_t width, uint32_t height, uint32_t stride,
                              const std::vector<DetectionBox>& boxes,
                              uint32_t origin_x, uint32_t origin_y) const;
    // ③ 中心→落点 连线 + FOV 圆（画法对照 sunone_aimbot / 各类 aimbot 的 cv2.circle）
    void draw_guides(uint8_t* crop, uint32_t width, uint32_t height, uint32_t stride,
                     const AimGuides& guides, uint32_t origin_x, uint32_t origin_y) const;
    // 把像素坐标平移到"裁剪区坐标系"（框是全帧坐标，预览只画裁剪区那块）
    static bool to_crop_coords(float x, float y, uint32_t origin_x, uint32_t origin_y,
                              uint32_t width, uint32_t height, int* ox, int* oy);
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
    DetectionsProvider detections_provider_;
    GuidesProvider guides_provider_;

    // 预览线程独占，按尺寸复用，避免每帧重复申请 640×640×3 临时缓冲。
    std::vector<uint8_t> crop_buffer_;
    // 仅在 ROI 超过输出上限（如全帧中心正方形 1440）时用于等比缩小，同样按尺寸复用。
    std::vector<uint8_t> scaled_buffer_;

    mutable std::mutex jpeg_mutex_;
    std::vector<uint8_t> jpeg_;

    Metrics metrics_;
};

}  // namespace ttbox::core
