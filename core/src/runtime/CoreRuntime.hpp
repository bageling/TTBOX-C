// CoreRuntime.hpp — 正式 AI Runtime 生命周期入口。
// 本类统一拥有 AimThread；Capture/Worker 接入在板端硬件阶段完成。
#pragma once
#include <atomic>
#include <memory>
#include <string>
#include "aim/AimThread.hpp"
#include "common/FrameRateMeter.hpp"
#include "common/Metrics.hpp"
#include "output/IHidOutput.hpp"
#include "output/OutputBackend.hpp"
#include "pipeline/AimTargetMailbox.hpp"
#include "capture/V4L2Capture.hpp"
#include "preview/PreviewModule.hpp"
#include "rknn/WorkerPool.hpp"
#include "model/RuntimeProfile.hpp"
#include "input/PhysicalMouseReader.hpp"
namespace ttbox::core {

// ★ M2.03：特性级启停门控（会话边界由 Application 读一次 LicenseGate 快照后下传）。
//   「默认全 true」= initialize 构建期保持历史行为（构建期尚未 publish 卡态时按最大能力构建，
//   后续由 set_feature_gates() 在会话边界按真实卡态收窄 —— 见 Application::run）。
//   语义只由 Application 依 LicenseGate 推导（单一真相源）；CoreRuntime 只按位启停模块。
struct FeatureGates {
    bool capture = true;    // V4L2 采集（= 整链是否可启动的唯一前置，pipeline_allowed 同义）
    bool inference = true;  // RKNN WorkerPool（模型推理）
    bool aim = true;        // AimThread（瞄准/输出）
};

// 全功能 ⇔ capture ∧ inference ∧ aim（唯一判据；预览降级与 module 启停共用）。
inline bool feature_gates_full(const FeatureGates& gates) {
    return gates.capture && gates.inference && gates.aim;
}

// ★ M2.03：受限预览帧率封顶（无 inference/aim 时 min(配置值, 5)；规格 §2.7）。
//   命名空间级常量 = Application 与 CoreRuntime 的**同一取值**（避免两处各写 5）。
constexpr int kRestrictedPreviewFps = 5;

// CoreRuntime — 核心运行时："总指挥"。
// 职责：把采集(Capture)、推理(WorkerPool)、瞄准(AimThread)、预览(Preview)、
//       IPC、输出(Output) 组装在一起，统一管理它们的启动/停止/状态。
// 输入：Params（采集/Worker/预览/输出/配置）
// 输出：运行状态 + collect_metrics() 聚合指标（帧率/延迟/检测数）
class CoreRuntime {
public:
    // 析构自动停机；默认构造后须先 initialize() 再 start()。
    CoreRuntime() = default; ~CoreRuntime(){stop();}
    // 让 `CoreRuntime::FeatureGates` 与命名空间级同名类型等价（Application 侧引用点两种写法均可）。
    using FeatureGates = ttbox::core::FeatureGates;
    struct Params {
        V4L2Capture::Params capture;
        WorkerPool::Params workers;
        PreviewModule::Params preview;
        std::shared_ptr<output::IHidOutput> output;
        RuntimeConfig* runtime_config = nullptr;
        // ★ M2.03：特性级启停（默认全 true = 构建期保持最大能力；会话边界由 set_feature_gates 收窄）。
        FeatureGates gates;
        // TTBOX 独立按键事件通道；空值时 PhysicalMouseReader 使用自己的默认值。
        std::string mouse_event_socket;
        // 第13阶段：链路诊断（config 控制；默认关闭）
        bool pipeline_debug_enabled = false;
        uint32_t pipeline_debug_interval = 60;
        // 第13阶段：PID Trace 采集（config 控制；默认关闭，只记录不改变行为）
        bool pid_trace_enabled = false;
        std::string pid_trace_path;
        // V1.0.46：逐帧原始检测框记录（回放数据源）
        bool det_trace_enabled = false;
        std::string det_trace_path;
        // 第15阶段：目标预测时域（秒；0=关闭预测，保持原行为）
        float prediction_time_s = 0.0f;
    };
    // 保存参数并装配 mailbox/capture/workers（不启动线程）；要求 output 非空、worker 数 1~3。
    bool initialize(const Params& params, std::string* error=nullptr);
    // 按 gates 逐模块启动（capture→workers→mouse→aim→preview），失败条件回滚；已运行则返回 false。
    bool start(std::string* error=nullptr); void stop(); bool running() const { return running_.load(); }
    // Hot model switch: only rebuild RKNN worker pool while capture/preview/aim stay alive.
    bool reload_workers(const WorkerPool::Params& params, std::string* error=nullptr);
    // 预加载：并行完成模型加载 + 预热，但不拉起轮询线程（等 start() 时补上）。
    // 目的：消除「点开始后要等一会儿才出结果」里的模型加载部分。失败不致命。
    bool preload_workers(std::string* error=nullptr);
    // ★ M2.03：会话边界刷新特性 gate + 预览降级参数（授权变化后下次 start() 生效）。
    //   · gates        = Application 依 LicenseGate 快照推导（唯一真相源；本层只执行不推导）。
    //   · brand_upper  = 卡内 ui_brand 的大写形（应用层从 Gate 快照取；用于水印文本）。
    //   · configured_fps = 配置的原始预览帧率（未降级值；用于全功能/受限两态间可逆恢复）。
    //   降级规则（与 Application::apply_preview_degrade 同源）：
    //     全功能 ⇒ fps=configured_fps、无水印；否则 ⇒ fps=min(configured_fps,5)、水印=brand_upper+" - LIMITED"。
    void set_feature_gates(const FeatureGates& gates,
                           const std::string& brand_upper,
                           int configured_fps);
    // 只有至少一帧真实 RKNN 推理并成功进入 Decode 后才算模型 ready。
    bool model_ready() const;
    // 推理出错累计计数（无 worker 时为 0）。
    uint64_t model_errors() const;
    // 目标邮箱（供测试/工具取最新任务）。
    aim::AimTargetMailbox* aim_mailbox(){return mailbox_.get();}
    // 物理鼠标读取器（其 button_source() 供输出层做热键二次防线）。
    input::PhysicalMouseReader& mouse_reader(){return mouse_reader_;}

    // G1：聚合真实运行指标（capture/worker 统计 + 最近任务目标数）。
    // 全部来自现有统计，无估算；runtime 未启动时各值保持 0（= unavailable）。
    void collect_metrics(PipelineMetrics* out) const;

    // Phase2：低帧实时预览（10fps，独立线程，不影响 AI 流水线）
    PreviewModule* preview() { return preview_.get(); }

    // 采集模块（Application 看门狗用：检测帧数长时间不增长后自动重枚举）
    V4L2Capture* capture() const { return capture_.get(); }

    // 推理模块已成功处理的总帧数（看门狗用；所有 worker 聚合，无锁原子计数）。
    uint64_t workers_processed() const {
        return workers_ ? workers_->total_processed() : 0;
    }
private:
    std::unique_ptr<aim::AimTargetMailbox> mailbox_;
    std::unique_ptr<V4L2Capture> capture_;
    std::unique_ptr<WorkerPool> workers_;
    RuntimeConfig* runtime_config_ = nullptr;
    WorkerPool::Params worker_params_{};
    bool workers_preloaded_ = false;  // 模型已加载+预热，start() 时只需拉起轮询线程
    PreviewModule::Params preview_params_{};
    bool pipeline_debug_enabled_ = false;      // 第13阶段：链路诊断
    uint32_t pipeline_debug_interval_ = 60;
    bool pid_trace_enabled_ = false;           // 第13阶段：PID Trace
    std::string pid_trace_path_;
    bool det_trace_enabled_ = false;           // V1.0.46：逐帧检测框记录
    std::string det_trace_path_;
    std::string mouse_event_socket_;
    float prediction_time_s_ = 0.0f;           // 第15阶段：目标预测时域
    // ★ M2.03：特性级启停态（会话边界由 Application 下传；start() 据此逐模块启停）。
    FeatureGates gates_{};
    // 预览降级辅助：原始配置帧率（用于全功能/受限两态间可逆恢复）+ 水印品牌（大写形）。
    int configured_preview_fps_ = 15;
    std::string brand_upper_ = "TTBOX";
    aim::AimThread aim_thread_;
    input::PhysicalMouseReader mouse_reader_;
    std::shared_ptr<output::IHidOutput> output_;
    std::atomic<bool> running_{false};
    std::atomic<int64_t> start_steady_ms_{0};
    // 推理瞬时帧率计（滚动窗口）。worker 每发布一帧 tick，metrics 采样时读。
    // 取代旧的「published ÷ 启动至今秒数」累计平均口径（会缓慢爬升，见 FrameRateMeter.hpp）。
    // ★ worker 参数的**唯一绑定点**：所有会把 WorkerPool::Params 交给 WorkerPool 的路径
    //   （initialize / start / reload_workers / reload 回滚）都必须先过这里。
    //   为什么必须收敛成一处：fps_meter 是指向本对象的裸指针，**不在**外部构造的 Params 里。
    //   曾经漏在 reload_workers 一条路径上（next = params 整体覆盖 ⇒ fps_meter 变 nullptr）
    //   ⇒ worker 从不 tick ⇒ fps() 恒 0 ⇒ 静默回退成"published ÷ 启动至今秒数"的累计平均
    //   ⇒ 面板帧率又变成"一点一点往上爬"（1.5.37 修了口径，却被这条路径绕过）。
    // ★ 同理，frame_w/frame_h 也在这里从 capture_ 现取回填（2026-09-23）：外部构造的
    //   Params 从不设这两个值，热切换后曾恒为 0 ⇒ 检测框不映射回原图坐标。
    void bind_worker_params(WorkerPool::Params& p);
    FrameRateMeter fps_meter_;
    std::unique_ptr<PreviewModule> preview_;  // G1：start() 时刻（steady 时钟，算推理 FPS 分母）
};
}
