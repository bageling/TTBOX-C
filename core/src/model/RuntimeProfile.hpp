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
// ★ 2026-10-06 架构整理：本文件原来 include 的是 "rknn/DetectionGeometryFilter.hpp"
//   （只为拿两个纯值类型），导致 model → rknn 的模块依赖；而 rknn/DecodeNMS.hpp
//   又反过来 include 本文件，形成 model ↔ rknn 环。值类型已下沉到 common/ 叶子头，
//   本行改依赖它 ⇒ 此环断开，model 重新变成"看不见 rknn"的模块。
#include "common/DetectionGeometryTypes.hpp"
// 各子配置的**值类型**（Capture/Inference/Fov/Preview/Video + Capture ROI 范围常量）
//   已下沉到 common/RuntimeProfiles.hpp。此前它们与聚合体 RuntimeProfile、热更新容器
//   RuntimeConfig 挤在一个 182 行的上帝头里，任何模块想读一个 float 都得拖走
//   Json.hpp + MouseTypes.hpp。剩下的本文件只负责"聚合"与"热更新机制"。
#include "common/RuntimeProfiles.hpp"

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
// CaptureProfile、InferenceProfile、FovShape、FovProfile、PreviewProfile、VideoProfile
//   以及 Capture ROI 范围常量（kMin/kMaxCaptureRoiPx）已于 2026-10-06 下沉到
//   common/RuntimeProfiles.hpp（纯值类型，干净叶子层）。本文件现在只负责
//   「聚合」与「热更新机制」，不再塞值类型。

// InferenceProfile / FovShape / FovProfile / PreviewProfile / VideoProfile 已于 2026-10-06
//   下沉到 common/RuntimeProfiles.hpp（纯值类型，干净叶子层；见文件头与上方说明）。
//   其中 InferenceProfile 与 FovProfile 正是 rknn/DecodeNMS.hpp 直接需要的两个类型 ——
//   下沉后 DecodeNMS 不必再为读它们而 include 整个本文件，model ↔ rknn 的依赖环
//   从这一侧被切断。

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
    // 序列化为 JSON（仅供配置管理/持久化，推理路径禁止逐帧解析）
    JsonValue to_json() const;
    // 从 JSON 反序列化：缺键取默认、历史坏值自愈、老配置兼容
    static RuntimeProfile from_json(const JsonValue& v);
    // 读文件并解析为 RuntimeProfile；解析失败返回空对象并填 error
    static RuntimeProfile from_json_file(const std::string& path, std::string* error = nullptr);

    // 简单校验：数值范围（错误返回 false + reason）
    bool validate(std::string* error = nullptr) const;
};

// ---------------------------------------------------------------------------
// RuntimeConfig：内存热更新配置（读端 = 单次短锁拷贝 shared_ptr 快照）
//   允许运行时修改：confidence / iou / class_filter / max_detections / FOV / ROI
//   禁止每帧 JSON/IPC；更新通过 update()（线程安全）。
//
// ★ 2026-10-06 纠偏：本类原先注释自称 "Lock-free 读"，与实现不符 ——
//   snapshot() 每次都要 lock_guard（见下）。实测锁占空比约 0.004%、真实争用≈0，
//   **刻意不改**：改成 atomic<shared_ptr> 并不会变成无锁（libstdc++ 上 C++17
//   atomic_load/store 与 C++20 atomic<shared_ptr> 都是锁实现），只会把锁挪进库内部。
//   这里改为如实描述，避免后人据此在"无锁"假设下写出错误代码。
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

    // 是否尚无任何已发布配置（无快照）
    bool empty() const { return snapshot() == nullptr; }

private:
    mutable std::mutex mtx_;
    std::shared_ptr<const RuntimeProfile> current_;
};

}  // namespace ttbox::core
