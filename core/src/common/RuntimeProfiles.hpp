// RuntimeProfiles.hpp — 运行期各子配置的**值类型**（叶子头）
//
// ★ 为什么独立成一个头（2026-10-06 架构整理）：
//   下面这些 struct 全是纯值类型（bool/int/float/vector），不含任何行为、不依赖
//   mouse / rknn / model 任何模块。它们此前与「聚合体 RuntimeProfile」和「热更新容器
//   RuntimeConfig」同住在 model/RuntimeProfile.hpp —— 那让"配置值类型"和"配置热更新
//   机制"被绑成一个 182 行的上帝头，任何模块想读一个 float 都得拖走 Json.hpp +
//   MouseTypes.hpp。
//
//   更具体的代价：rknn/DecodeNMS.hpp 为了用 InferenceProfile / FovProfile 两个小结构，
//   不得不 include 整个 model/RuntimeProfile.hpp；而 model 又要 include rknn 的头
//   ⇒ **model ↔ rknn 模块级依赖环**。下沉到 common/ 后，环的一边被切断。
//
// 划分约定（谁住哪儿）：
//   common/RuntimeProfiles.hpp   ← 本文件：各子配置的**值**（纯数据，谁都能依赖）
//   model/RuntimeProfile.hpp      ← 聚合体 RuntimeProfile（把各子配置 + MouseProfile 组合起来）
//                                    + RuntimeConfig（热更新容器，带 mutex 与快照）
//   为什么聚合体留在 model：它含 aim::MouseProfile（依赖 mouse 模块），
//   放进 common 会让叶子层反过来依赖 mouse，那就不是叶子了。
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace ttbox::core {

// Capture ROI 合法范围（fail-closed 防线，单一权威源 —— 见 DUP-10）
//   由 CaptureProfile::valid() 加载自愈 与 Application SET_CONFIG 校验共用。
constexpr uint32_t kMinCaptureRoiPx = 64;
constexpr uint32_t kMaxCaptureRoiPx = 3840;

// ---------------------------------------------------------------------------
// Capture ROI：屏幕截取区域（像素）
//   width/height = 截取宽高（≠ 模型输入尺寸；0 = 默认全帧）
//   offset_x/offset_y 已于 V1.0.13 删除：业主口径「参考物太多，落点与设置瞄点对不上」。
// ---------------------------------------------------------------------------
struct CaptureProfile {
    uint32_t width = 0;      // 0 = 默认（全帧）
    uint32_t height = 0;     // 0 = 默认（全帧）

    // 校验 ROI 宽高是否落在合法域（0 或 kMinCaptureRoiPx~kMaxCaptureRoiPx）；失败写 error。
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
//     · capture.*   = AI 推理 ROI：帧已经采到了，再在 CPU/RGA 上裁一块送推理（软件裁剪）。
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

}  // namespace ttbox::core
