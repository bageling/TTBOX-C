// RuntimeProfile.hpp — 运行时模型配置（阶段 A-8）
//
// 目标：模型本身（ModelMetadata，A-7）与用户参数彻底分离。
//   - RuntimeProfile 只描述"用户/运行时想怎么跑"，绝不写入 RKNN 或 ModelMetadata。
//   - 所有字段都可被用户修改：ROI / FOV / confidence / iou / class_filter / max_detections。
//
// 配置优先级（不变）：
//   Model Default (ModelMetadata) < Runtime Default < User Config (RuntimeProfile)
//
// 热更新：RuntimeConfig 持有 shared_ptr<const RuntimeProfile>，更新 = 原子替换
// 快照（禁逐帧 JSON/IPC）。解码器/worker 每帧取只读快照。
#pragma once

#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "common/Json.hpp"
#include "mouse/MouseTypes.hpp"
#include "rknn/DetectionGeometryFilter.hpp"

namespace ttbox::core {

// ---------------------------------------------------------------------------
// Capture ROI 合法范围（fail-closed 防线，单一权威源 —— 见 DUP-10）
//   背景：前端把"0=全帧/未知"误钳成 1，产生 capture=1×1 的退化配置，
//   会让 AI ROI 缩成 1 像素（推理停摆）+ 预览裁成 1×1（黑屏/花屏），
//   而系统全程零报错——属于"底层已死却报成功"。
//   这里设硬下限：模型最小输入档 192，留足余量取 64；上限对齐 4K。
//   0 = 全帧（合法）；[1, kMinCaptureRoiPx) = 退化非法区间。
//   由 RuntimeProfile 加载自愈 与 Application SET_CONFIG 校验共用。
// ---------------------------------------------------------------------------
constexpr uint32_t kMinCaptureRoiPx = 64;
constexpr uint32_t kMaxCaptureRoiPx = 3840;

// ---------------------------------------------------------------------------
// Capture ROI：屏幕截取区域（像素）
//   width/height = 截取宽高（≠ 模型输入尺寸；0 = 默认全帧）
//   offset_x/offset_y = 相对屏幕中心的偏移（px，负值=左/上；配合 width/height 使用）
// ---------------------------------------------------------------------------
struct CaptureProfile {
    // ★ V1.0.13（2026-09-30）：offset_x/offset_y（相对屏幕中心偏移）已删 ——
    //   业主口径「参考物太多，落点与设置瞄点对不上」。它把 AI 看的那块从屏幕中心
    //   挪走，等效于在"瞄点"之外又开了一个能移落点的口子（面板「看的位置上下移」）。
    //   裁剪区恒以屏幕中心为心；落点只剩「瞄点」一个入口。
    uint32_t width = 0;      // 0 = 默认（全帧）
    uint32_t height = 0;     // 0 = 默认（全帧）

    bool valid(uint32_t frame_w, uint32_t frame_h, std::string* error = nullptr) const;
};

// ---------------------------------------------------------------------------
// Inference：检测参数（用户可改）
// ---------------------------------------------------------------------------
struct InferenceProfile {
    float confidence = 0.0f;     // 0 = 用 ModelMetadata.default_conf
    float iou = 0.0f;            // 0 = 用 ModelMetadata.default_iou
    std::vector<int> class_filter;  // 空 = 全部保留
    int max_detections = 0;      // 0 = 不限
};

// FOV 形状
enum class FovShape : int { kCircle = 0, kRect = 1 };

// FOV：最终检测过滤（在 NMS 之后应用）
//   center_x/center_y：归一化（0~1，相对全帧）
//   radius：circle = 归一化半径；rect = 归一化半宽/半高
struct FovProfile {
    bool enabled = false;
    FovShape shape = FovShape::kCircle;
    float radius = 0.5f;    // circle: 归一化半径；rect: 归一化半宽半高
    float center_x = 0.5f;  // 0~1
    float center_y = 0.5f;  // 0~1
};

// ---------------------------------------------------------------------------
// Preview：Web 控制台实时画面（用户可选；与模型输入解耦）
//   显示屏幕正中心 capture.width×capture.height 方框 → Preview
//   capture.width/height = 中心截取尺寸（默认 640x640）
// ---------------------------------------------------------------------------
struct PreviewProfile {
    uint32_t width = 640;    // Preview 输出宽，默认跟随中心截取宽
    uint32_t height = 640;   // Preview 输出高，默认跟随中心截取高
    uint32_t roi_w = 640;    // 兼容旧配置字段；中心截取优先使用 capture.width
    uint32_t roi_h = 640;
    bool center_crop = true;
    uint32_t fps = 0;        // 预览帧率上限（0 = 用 Application 默认 preview_fps）
};

// ---------------------------------------------------------------------------
// Video：采集层（V4L2 硬件裁剪）与 NPU 输入通路（P-ZC-1）
//
//   与 CaptureProfile 不是一回事，勿混：
//     · capture.*  = AI 推理 ROI：帧已经采到了，再在 CPU/RGA 上裁一块送推理（软件裁剪）。
//     · video.crop_* = 让 V4L2 **采集时就只出这一块**（VIDIOC_S_SELECTION，硬件裁剪），
//       它直接决定 RGA 要不要缩放 —— crop 与模型输入同尺寸时 RGA 只剩格式转换。
//
//   crop_* = 0 ⇒ 沿用全局配置（config/default.json 的 crop_width/crop_height）。
//   之所以留这个"未设置"态：运行配置是客户层单文件、OTA 不覆盖（见 P-ZC-1），
//   若这里给死值，老机器升级后反而会被我们悄悄改掉采集尺寸。0 = 不动它。
//
//   zero_copy_input：对应全局键 rknn_external_dma_input。默认 true —— 本项的
//   存在意义就是让已装机设备（全局配置里写着 false）能在面板上打开零拷贝。
// ---------------------------------------------------------------------------
struct VideoProfile {
    uint32_t crop_width = 0;       // 0 = 沿用全局配置
    uint32_t crop_height = 0;      // 0 = 沿用全局配置
    bool zero_copy_input = true;   // RGA 输出的 DMA-BUF 直入 NPU 输入
    // 三态：false = profile 里没有这个键（老配置），此时**沿用全局配置**，
    //       不拿上面的默认值去覆盖用户机器上的显式 false。
    //       只有用户在面板上真的拨过这个开关，才置 true 并以其值为准。
    bool zero_copy_input_set = false;

    bool using_global_crop() const { return crop_width == 0 || crop_height == 0; }
};

// ---------------------------------------------------------------------------
// RuntimeProfile：完整用户配置（模型无关）
// ---------------------------------------------------------------------------
struct RuntimeProfile {
    std::string model_id;       // 关联 installed 模型（空 = 未指定，用激活模型）
    CaptureProfile capture;
    InferenceProfile inference;
    FovProfile fov;
    PreviewProfile preview;     // Web 实时画面尺寸
    VideoProfile video;         // P-ZC-1：采集层裁剪 + 零拷贝开关
    aim::MouseProfile mouse;    // A10：鼠标 AI 注入配置（与模型彻底分离）
    DetectionGeometryFilterConfig geometry_filter;

    // ---- JSON 序列化（仅配置管理/持久化使用；推理路径禁止逐帧解析）----
    JsonValue to_json() const;
    static RuntimeProfile from_json(const JsonValue& v);
    static RuntimeProfile from_json_file(const std::string& path, std::string* error = nullptr);

    // 简单校验：数值范围（错误返回 false + reason）
    bool validate(std::string* error = nullptr) const;
};

// ---------------------------------------------------------------------------
// RuntimeConfig：内存热更新配置（Lock-free 读：读快照 shared_ptr）
//   允许运行时修改：confidence / iou / class_filter / max_detections / FOV / ROI
//   禁止每帧 JSON/IPC；更新通过 update()（线程安全）。
// ---------------------------------------------------------------------------
class RuntimeConfig {
public:
    RuntimeConfig() = default;

    // 原子替换当前配置（线程安全）
    void update(std::shared_ptr<const RuntimeProfile> profile) {
        std::lock_guard<std::mutex> lk(mtx_);
        current_ = std::move(profile);
    }
    void update(const RuntimeProfile& profile) {
        update(std::make_shared<const RuntimeProfile>(profile));
    }

    // 只读快照（每帧调用安全；共享所有权，无拷贝竞争）
    std::shared_ptr<const RuntimeProfile> snapshot() const {
        std::lock_guard<std::mutex> lk(mtx_);
        return current_;
    }

    bool empty() const { return snapshot() == nullptr; }

private:
    mutable std::mutex mtx_;
    std::shared_ptr<const RuntimeProfile> current_;
};

}  // namespace ttbox::core
