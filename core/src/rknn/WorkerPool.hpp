// WorkerPool.hpp — 多 Worker 并发 RKNN 推理（阶段 A-5）
//
// 目标：验证多个独立 RKNN context（各自绑定 NPU core）能否并行利用
//       RK3588 三个 NPU Core，提升 640×640 FP16 模型整体吞吐。
//
// 设计：
//   - InferenceWorker：独立线程 + 独立 RKNNEngine context + 独立 RgaProcessor + FP16 buffer
//   - 共享 LatestFrame（capture 提供）：无队列、旧帧覆盖；worker 按 seq % N 认领帧
//     （无重复处理、无延迟累积；帧只被一个 worker 消费）
//   - core_mask：单 worker 用 config 值（0=auto）；多 worker 由调用方指定每核绑定（如 {1,2,4}）
//   - 生命周期 RAII：stop() join 线程 → engine.destroy() → rga.destroy()；析构兜底
//   - 并发安全：worker 间无共享可变状态（仅共享只读 LatestFrame.get()，有锁）；
//     帧在 RGA/RKNN 使用期间由 shared_ptr 保活（V4L2 buffer 不提前归还）
//
// 边界：本模块只做"取帧 → RGA → FP16 → RKNN"，不含 Decode/Aim/HID（A-6+）。
#pragma once

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "common/FrameRateMeter.hpp"
#include "common/Stats.hpp"
#include "capture/V4L2Capture.hpp"
#include "model/Decoder.hpp"
#include "model/ModelAdapter.hpp"
#include "mouse/CrosshairProbe.hpp"
#include "model/RuntimeProfile.hpp"
#include "pipeline/AimTargetMailbox.hpp"
#include "rga/RgaProcessor.hpp"
#include "rknn/DecodeNMS.hpp"
#include "rknn/InputPathSummary.hpp"  // T1.15：输入通路诊断快照 + 跨 worker 聚合纯函数
#include "rknn/RKNNEngine.hpp"
#include "rknn/DetectionGeometryFilter.hpp"
#include "rknn/Preprocess.hpp"
// ★ 2026-10-06 移除：原来这里 include "rknn/Detector.hpp"，但本头从未使用 Detector 类型
//   （worker 直接持 RKNNEngine + DecodeNMS 完成推理与解码）。这个多余的 include 会经由
//   Detector.hpp → model/Decoder.hpp + model/ModelAdapter.hpp 把 rknn 反向绑到 model，
//   助长 model ↔ rknn 的模块环。rknn/Detector.{hpp,cpp} 保留为未接线的参考实现
//   （实现 detector::IDetector，供将来接第二后端），但不再被推理主链路拖进来。

namespace ttbox::core {

// Decode/NMS 阶段统计（A-6）
struct DecodeStageStats {
    StatsCollector decode;   // 候选解码（原始输出→过滤）
    StatsCollector nms;      // NMS
    StatsCollector total;    // decode+nms
};

// 单 Worker 统计（计数为原子，跨线程安全）
struct WorkerStats {
    std::atomic<uint64_t> processed{0};  // 成功完成解码的帧数
    std::atomic<uint64_t> rga_ok{0};
    std::atomic<uint64_t> direct_ok{0};  // CPU 直拷帧数（cpu_direct 路径）
    StatsCollector queue_wait;           // 排队等待（帧时间戳→worker 认领，buffer_age 同口径）
    std::atomic<uint64_t> inference_ok{0};
    std::atomic<uint64_t> decode_ok{0};
    std::atomic<uint64_t> published{0};
    std::atomic<uint64_t> errors{0};
    std::atomic<uint64_t> skipped{0};    // 认领判断跳过（分配给其他 worker 或时序落后）
    // ★ 2026-10-08：取帧等待超时次数（wait_new 返回 null）。**这才是真丢帧/断流信号。**
    //   与 skipped 的区别（两者语义完全不同，别混读）：
    //     · skipped     = 帧到了，但 seq%total_workers 不属于我 ⇒ **轮转让帧**，稳态
    //                     ≈ (N−1)×processed，是 N 路轮转的数学必然，不是异常；
    //     · 本字段      = 帧**根本没到**（等满 kFrameWaitTimeoutUs=7ms 仍无新 seq）
    //                     ⇒ 采集断流 / 通知丢失 / worker 被调度饿死。
    //   判读：稳态应≈0；持续增长 = 上游真出问题。stop() 期间可能每 worker 多计 1（见 .cpp 注释）。
    std::atomic<uint64_t> no_frame_timeout{0};
    RknnStageStats stages;               // set_input / run / output / total
    StatsCollector convert;              // uint8->FP16 转换耗时（us）
    StatsCollector rga;                  // RGA process 耗时（us，测量层）
    StatsCollector e2e;                  // 帧采集(单调时钟) -> 推理完成（us）
    DecodeStageStats decode_stages;      // decode / nms / total
    std::atomic<uint64_t> candidates{0}; // conf 过滤后候选数（累计）
    std::atomic<uint64_t> detections{0}; // NMS 后目标数（累计）
    // T1.15：模型输入通路诊断（换模型后"走了哪条快路径"由面板显示，不再靠 UART 日志猜）。
    // 写入时机 = start() 内 engine init/init_zero_copy 之后一次性快照；external_dma_bound
    // 在首帧成功绑定 DMA-BUF 后转 true；stop() 时 reset()。详见 rknn/InputPathSummary.hpp。
    InputPathStats input_path;
};

// 单 Worker：独立 context + RGA + FP16 转换
class InferenceWorker {
public:
    // 单 worker 启动参数（编号、绑定核、模型、尺寸、阈值、解码/邮箱等）。
    struct Params {
        int id = 0;                 // worker 编号（0..N-1），决定认领 seq % N
        int core_mask = 0;          // 绑定 NPU core（0=auto；多核绑定如 1/2/4）
        std::string model_path;
        bool pass_through = false;  // 生产由 config 开启；true=零拷贝+pass_through，
                                    // INT8/NHWC 时 WorkerPool 必须做 uint8→int8 XOR 转换
        bool disable_cache_flush = false;  // 跳过 CPU↔NPU 缓存同步，降低推理延迟
        int warmup_rounds = 3;   // 加载后空跑次数（0=不预热）；消除「点开始后首帧慢」
        bool deferred_start = false;  // true=只加载/预热，不拉起轮询线程（预加载用）
        bool external_dma_input = false; // 实验开关：RGA DMA-BUF 直绑 RKNN，默认关闭
        uint32_t out_w = 0;         // 模型输入尺寸（config）
        uint32_t out_h = 0;
        LatestFrame* latest = nullptr;   // 共享最新帧（capture 提供，非拥有）
        // 共享瞬时帧率计（CoreRuntime 提供，非拥有）：每发布一帧 tick 一次。
        // 为空则不做采样（单测/工具可不传），CoreRuntime 回退到累计平均。
        FrameRateMeter* fps_meter = nullptr;
        int total_workers = 1;
        // A-6 Decode/NMS 参数（阈值来自 config；frame 尺寸用于坐标映射）
        float conf_thres = 0.25f;
        float iou_thres = 0.45f;
        uint32_t frame_w = 0;       // 原图宽（0=不映射）
        uint32_t frame_h = 0;
        int color_order = 0;        // RGA 输出颜色（模型输入要求）：0=BGR, 1=RGB
        ModelAdapter* adapter = nullptr;  // A-7：可选统一适配器
        RuntimeConfig* runtime_config = nullptr;  // A-8：可选内存热更新配置（禁逐帧 JSON）
        aim::AimTargetMailbox* aim_mailbox = nullptr;  // 新架构：Worker -> AimThread 目标邮箱
    };

    InferenceWorker() = default;
    ~InferenceWorker();

    bool start(const Params& params, std::string* error = nullptr);
    void stop();
    bool running() const { return running_.load(); }
    // 预加载（deferred_start=true）后只差这一步：拉起轮询线程。
    bool start_loop(std::string* error = nullptr);
    void set_frame_size(uint32_t w, uint32_t h);
    const WorkerStats& stats() const { return stats_; }
    int id() const { return id_; }

    // ---- 预处理后端诊断（面板「预处理路径」用）----
    // 为什么需要：面板那一格读的是 raw_preprocess_backend，后端此前从未产出该键，
    // 前端只能显示 "-"。此处是首个真源。
    enum class PreprocessBackendKind {
        kNone = 0,        // 未初始化 / 无预处理实例
        kRga = 1,         // 走 RGA 硬件（正常）
        kCpuFallback = 2, // RGA 不可用，退 CPU
        kFailed = 3,      // RGA 有错帧
    };
    PreprocessBackendKind preprocess_backend() const;
    std::string last_preprocess_error() const;

private:
    void loop();
    void record_preprocess_error(const std::string& error);
    // A-8：应用最新 RuntimeProfile（conf/iou/filter/max/FOV/ROI）到 decoder/RGA
    void apply_runtime_profile();

    Params params_;
    std::atomic<bool> running_{false};
    std::thread thread_;
    std::unique_ptr<Preprocess> preprocess_;

    std::unique_ptr<RKNNEngine> engine_;
    std::unique_ptr<Decoder> decoder_;
    std::vector<std::vector<uint8_t>> raw_outputs_;
    std::vector<void*> raw_buf_ptrs_;
    std::vector<size_t> raw_sizes_;
    std::vector<DetectionBox> detections_; // 本帧检测结果，转换为 AimTargetTask
    DetectionGeometryFilter geometry_filter_;
    uint32_t last_seq_ = 0;
    int id_ = -1;
    int bound_input_dma_fd_ = -1;
    WorkerStats stats_;
    // A-8：热更新跟踪（避免每帧重复设置）
    std::shared_ptr<const RuntimeProfile> applied_profile_;
    // 准星找色（急停检测）：参数随 profile 热更新缓存，结果按 interval 节流后跨帧沿用。
    // 取色必须放在有帧的一侧 —— AimTargetMailbox 只传小型检测结果、不传图像，
    // AimThread 拿不到像素，只能消费这里算好的 bool。
    aim::CrosshairProbeParams stop_detect_params_;
    bool stop_detect_hit_ = false;
    uint64_t stop_detect_last_frame_ = 0;
    // 最近一次预处理失败原因（面板「最后错误」用）。锁只在失败路径偶尔取，
    // 成功路径不碰，不影响热路径。
    mutable std::mutex preprocess_err_mu_;
    std::string last_preprocess_error_;
};

// Worker 池：创建/启动/停止 N 个 InferenceWorker，聚合统计
//
// ══════════════ 主链第 2 站「认」+ 第 3 站「找」的引擎侧 —— ★★★ 已冻结（业主 2026-10-08）═══════════════
// 职责：N 个 worker 轮流认领最新帧→ 预处理 → RKNN 推理 → 解码出框 → 几何过滤。
// 业主口径：「前三步看/认/找没问题了，先冻结前三步」⇒
//   **本目录（rknn/）自 2026-10-08 起为冻结区**，改动前必须先问业主并说明理由。
//   已做的两项改动**属于冻结前**，冻结后不再动：
//   · `kFrameWaitTimeoutUs = 7000`（取帧兜底超时；性质是**语义正确性**——
//     400µs 相对 6.94ms 帧周期小 17 倍属错配；**性能收益实测≈0**，因为
//     `LatestFrame::publish()` 持锁 `notify_all()` 无漏唤醒、快路径直接返回，超时稳态不触发）。
//   · `no_frame_timeout` 计数器（真丢帧信号；区别于 `skipped`＝轮转让帧，稳态≈2/3）。
// 已知且**故意不修**的项（别再当成 bug 报）：
//   · `create_workers` 失败路径的 use-after-free（`:560-574`）—— NPU init 失败才触发，改要先定失败回滚策略。
//   · `stop_keep_workers()` 名不副实（会 destroy 引擎）—— 语义问题不是 bug，改动要动上层启停流程。
//   · `Detector.cpp` 花括号不配对 —— **未编入 CORE_SOURCES**，接线前修即可。
// ══════════════════════════════════════════════════════════════════════════════════
class WorkerPool {
public:
    // 池级启动参数（worker 核绑定列表 + 透传给各 worker 的公共配置）。
    struct Params {
        std::string model_path;
        std::vector<int> worker_cores;  // 每 worker core_mask（长度 = worker 数）
        bool pass_through = false;      // 生产由 config 开启；true=零拷贝+pass_through，
                                        // INT8/NHWC 时 WorkerPool 负责 uint8→int8 XOR
        bool external_dma_input = false; // 实验开关：RGA DMA-BUF 直绑 RKNN，默认关闭
        bool disable_cache_flush = false;  // 跳过 CPU↔NPU 缓存同步，降低推理延迟
        int warmup_rounds = 3;       // 透传给每个 worker 的预热次数（0=不预热）
        uint32_t out_w = 0;
        uint32_t out_h = 0;
        LatestFrame* latest = nullptr;
        FrameRateMeter* fps_meter = nullptr;  // 共享瞬时帧率计（CoreRuntime 提供，非拥有）
        // A-6 Decode/NMS（透传给每个 worker；阈值来自 config）
        float conf_thres = 0.25f;
        float iou_thres = 0.45f;
        uint32_t frame_w = 0;
        uint32_t frame_h = 0;
        int color_order = 0;        // RGA 输出颜色（模型输入要求）：0=BGR, 1=RGB
        ModelAdapter* adapter = nullptr;  // A-7：可选统一适配器
        RuntimeConfig* runtime_config = nullptr;  // A-8：可选内存热更新配置（禁逐帧 JSON）
        aim::AimTargetMailbox* aim_mailbox = nullptr;  // 新架构：Worker -> AimThread 目标邮箱
    };

    WorkerPool() = default;
    ~WorkerPool() { stop(); }

    // 创建并启动 N 个 worker（worker_cores.size() 决定数量）
    bool start(const Params& params, std::string* error = nullptr);
    // 预加载：并行完成「模型加载 + 预热」，但不拉起轮询线程。
    // 之后用 start_loops() 补上线程；用 set_frame_size() 刷新真实采集尺寸。
    bool preload(const Params& params, std::string* error = nullptr);
    bool start_loops(std::string* error = nullptr);
    void set_frame_size(uint32_t w, uint32_t h);
    void stop();

    // ★ 2026-10-04：只停线程、**保留 worker 对象**（模型/NPU 上下文留着）。
    //   与 stop() 的唯一区别是不清 workers_ ⇒ 下次 start_loops() 只需拉起线程，
    //   省掉整个模型加载（板端实测可省 ~200ms）。
    //   代价：NPU 三个核心与模型内存**常驻**。
    //   ★ 用哪个由产品语义定："停止"若要彻底释放资源就用 stop()。
    void stop_keep_workers();

    size_t worker_count() const { return workers_.size(); }
    const std::vector<std::unique_ptr<InferenceWorker>>& workers() const { return workers_; }

    // 聚合统计（只读）
    uint64_t total_processed() const;
    uint64_t total_errors() const;
    uint64_t total_skipped() const;
    // 取帧等待超时累计（跨 worker 求和）：**真丢帧/断流信号**，
    // 与 total_skipped()（轮转让帧，稳态 ≈(N−1)×processed）语义相反、必须分开读。
    uint64_t total_no_frame_timeout() const;
    // 预处理后端诊断：取所有 worker 里"最差"的那个——只要有一路退 CPU 或出错就该被看见，
    // 不能因为 worker[0] 正常就把问题藏起来。
    InferenceWorker::PreprocessBackendKind preprocess_backend() const;
    std::string last_preprocess_error() const;

private:
    std::vector<std::unique_ptr<InferenceWorker>> workers_;
};

}  // namespace ttbox::core
